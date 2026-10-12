"""Planner regressions use real package histories, never live release services."""
import importlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


def planner(test):
    try:
        return importlib.import_module('release_plan')
    except ModuleNotFoundError:
        test.fail('Batch planner is missing; All cannot select affected packages')


class PlannerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.packages = {'demo_interface': 'packages/interface',
                         'demo_android': 'packages/android',
                         'demo_ios': 'packages/ios', 'demo': 'packages/demo'}
        self.git('init', '-b', 'main')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'user.name', 'Release fixture')
        for name, path in self.packages.items():
            deps = {'demo_interface': {}, 'demo_android': {'demo_interface': '^1.0.0'},
                    'demo_ios': {'demo_interface': '^1.0.0'},
                    'demo': {'demo_android': '^1.0.0', 'demo_ios': '^1.0.0'}}[name]
            self.package(name, path, deps)
        self.commit('Baseline')
        for name in self.packages:
            self.git('tag', name + '-v1.0.0')
        self.published = {name: {'name': name, 'versions': [{'version': '1.0.0'}]}
                          for name in self.packages}

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root,
                                       stderr=subprocess.DEVNULL, text=True).strip()

    def commit(self, message):
        self.git('add', '.')
        self.git('commit', '-m', message)

    def package(self, name, path, deps):
        directory = self.root / path
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'lib').mkdir(exist_ok=True)
        (directory / 'lib/plugin.dart').write_text('// baseline\n')
        manifest = f'# Keep package comments\nname: {name}\nversion: 1.0.0\ndependencies:\n'
        manifest += ''.join(f'  {dep}: {floor}\n' for dep, floor in deps.items())
        (directory / 'pubspec.yaml').write_text(manifest)
        (directory / 'CHANGELOG.md').write_text('## 1.0.0\n\n- Initial.\n')

    def change(self, name):
        (self.root / self.packages[name] / 'lib/plugin.dart').write_text('// new behavior\n')
        self.commit('Change ' + name)

    def plan(self, package='all', bump='patch'):
        return planner(self).build_plan(self.root, self.packages, package, bump,
                                       self.published, self.git('rev-parse', 'HEAD'))

    def test_android_change_selects_android_then_facade_skips_ios_and_interface(self):
        self.change('demo_android')
        result = self.plan()
        self.assertEqual([p['name'] for p in result['packages']], ['demo_android', 'demo'])
        self.assertEqual([p['version'] for p in result['packages']], ['1.0.1', '1.0.1'])
        self.assertIn('demo_android: ^1.0.1', result['files']['packages/demo/pubspec.yaml'])
        self.assertIn('# Keep package comments', result['files']['packages/demo/pubspec.yaml'])
        self.assertIn('dependency', result['packages'][1]['reason'])

    def test_interface_change_propagates_transitively_in_dependency_order(self):
        self.change('demo_interface')
        result = self.plan(bump='minor')
        self.assertEqual([p['name'] for p in result['packages']],
                         ['demo_interface', 'demo_android', 'demo_ios', 'demo'])
        self.assertIn('demo_interface: ^1.1.0', result['files']['packages/ios/pubspec.yaml'])
        self.assertEqual(result['packages'][-1]['prerequisites'],
                         {'demo_android': '1.1.0', 'demo_ios': '1.1.0'})

    def test_unchanged_repo_is_noop_and_does_not_recut_versions(self):
        self.assertEqual(self.plan()['packages'], [])
        self.assertEqual(self.plan()['files'], {})

    def test_unpublished_tag_is_not_a_successful_release_baseline(self):
        self.change('demo_android')
        self.git('tag', 'demo_android-v1.0.1')
        self.assertEqual(self.plan()['packages'][0]['name'], 'demo_android')

    def test_root_package_does_not_claim_nested_platform_source_changes(self):
        self.packages['demo'] = '.'
        self.package('demo', '.', {'demo_android': '^1.0.0', 'demo_ios': '^1.0.0'})
        self.commit('Place facade at root')
        self.git('tag', '-f', 'demo-v1.0.0')
        self.change('demo_android')
        facade = self.plan()['packages'][-1]
        self.assertIn('dependency', facade['reason'])
        self.assertNotIn('source', facade['reason'])

    def test_release_machinery_and_overrides_do_not_force_package_release(self):
        self.packages['demo'] = '.'
        self.package('demo', '.', {})
        self.commit('Root facade')
        self.git('tag', '-f', 'demo-v1.0.0')
        for relative in ['tool/run.py', '.github/workflows/release.yml',
                         'packages/android/pubspec_overrides.yaml']:
            file = self.root / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('tooling change\n')
        self.commit('Release tooling')
        self.assertEqual(self.plan()['packages'], [])

    def test_authored_unreleased_notes_are_frozen_in_candidate_changelog(self):
        changelog = self.root / 'packages/android/CHANGELOG.md'
        changelog.write_text('## Unreleased\n\n### Fixed\n- Correct call fields.\n\n'
                             '## 1.0.0\n\n- Initial.\n')
        self.commit('Correct call fields')
        before = changelog.read_text()
        result = self.plan()
        frozen = result['files']['packages/android/CHANGELOG.md']
        self.assertTrue(frozen.startswith('## 1.0.1\n\n### Fixed'))
        self.assertNotIn('## Unreleased', frozen)
        self.assertIn('## 1.0.0\n\n- Initial.', frozen)
        self.assertEqual(changelog.read_text(), before)
        self.assertEqual(json.loads(json.dumps(result)), result)

    def test_cycles_fail_before_a_release_plan_is_created(self):
        self.package('demo_interface', 'packages/interface', {'demo_android': '^1.0.0'})
        self.commit('Cycle')
        with self.assertRaisesRegex(ValueError, 'cycle'):
            self.plan()

    def test_lower_candidate_than_an_existing_required_floor_is_rejected(self):
        file = self.root / 'packages/demo/pubspec.yaml'
        file.write_text(file.read_text().replace('demo_android: ^1.0.0', 'demo_android: ^2.0.0'))
        self.commit('Future required API')
        self.change('demo_android')
        with self.assertRaisesRegex(ValueError, 'downgrade|below'):
            self.plan()

    def test_individual_cut_does_not_silently_release_other_packages(self):
        self.change('demo_android')
        result = self.plan(package='demo_android')
        self.assertEqual([p['name'] for p in result['packages']], ['demo_android'])

    def test_missing_verified_tag_requires_evidence_instead_of_inferred_change(self):
        self.git('tag','-d','demo_ios-v1.0.0')
        import yaml
        self.published['demo_ios']['latest']={'version':'1.0.0','pubspec':yaml.safe_load(
            (self.root/'packages/ios/pubspec.yaml').read_text())}
        with self.assertRaisesRegex(ValueError,'baseline|attention'):
            self.plan()

    def test_known_hosted_manifest_difference_proves_change_without_a_tag(self):
        self.git('tag','-d','demo_ios-v1.0.0')
        import yaml
        file=self.root/'packages/ios/pubspec.yaml'
        self.published['demo_ios']['latest']={'version':'1.0.0','pubspec':yaml.safe_load(file.read_text())}
        file.write_text(file.read_text().replace('version: 1.0.0','version: 1.0.1'))
        self.commit('Owned manifest change')
        self.assertEqual([p['name'] for p in self.plan()['packages']],['demo_ios','demo'])

    def test_invalid_constraint_is_reported_without_mutating_checkout(self):
        file = self.root / 'packages/android/pubspec.yaml'
        file.write_text(file.read_text().replace('^1.0.0', 'bad constraint'))
        self.commit('Invalid dependency')
        before = file.read_text()
        with self.assertRaisesRegex(ValueError, 'constraint'):
            self.plan()
        self.assertEqual(file.read_text(), before)


if __name__ == '__main__':
    unittest.main()
