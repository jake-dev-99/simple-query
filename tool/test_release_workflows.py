"""Exercise workflow argument routing, hosted Deploy boundary, and real CLI preview."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest

import yaml
import test_release_coordinator as fixtures

ROOT = Path(__file__).resolve().parents[1]


def workflow(name):
    # BaseLoader keeps YAML's `on` key as a string rather than a YAML 1.1 boolean.
    return yaml.load((ROOT / '.github/workflows' / name).read_text(), Loader=yaml.BaseLoader)


def shell_step(name, step_name):
    for job in workflow(name)['jobs'].values():
        for step in job['steps']:
            if step.get('name') == step_name:
                return step['run']
    raise AssertionError('Missing workflow step: ' + step_name)


class WorkflowTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=str(self.bin)+':'+os.environ['PATH'],
                        EVENT_NAME='workflow_dispatch', OPERATION='cut', PACKAGE='all',
                        BUMP='minor', DRY_RUN='false')

    def fake(self, name, code):
        file = self.bin / name
        file.write_text(code)
        file.chmod(0o755)

    def step(self, workflow_name, name, cwd=None):
        return subprocess.run(['bash', '-euo', 'pipefail', '-c', shell_step(workflow_name, name)],
                              cwd=cwd or self.root, env=self.env, text=True, capture_output=True)

    def route(self):
        self.fake('python3', '#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
        result = self.step('release.yml', 'Coordinate batch release')
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.splitlines()

    def test_manual_batch_and_preview_reach_coordinator_without_shell_expansion(self):
        self.env.update(PACKAGE='all', DRY_RUN='true')
        self.assertEqual(self.route(), ['tool/release_coordinator.py', '--operation', 'cut',
                         '--package', 'all', '--bump', 'minor', '--dry-run'])
        self.env.update(PACKAGE='$(exit 91)')
        self.assertIn('$(exit 91)', self.route())

    def test_hourly_schedule_only_resumes_authorized_work(self):
        self.env.update(EVENT_NAME='schedule', OPERATION='cut', DRY_RUN='true')
        self.assertEqual(self.route(), ['tool/release_coordinator.py', '--operation', 'resume', '--scheduled'])

    def test_resume_and_cancel_reach_same_coordinator(self):
        for operation in ['resume', 'cancel']:
            self.env['OPERATION'] = operation
            self.assertEqual(self.route()[1:3], ['--operation', operation])

    def test_workflows_are_valid_and_all_shell_blocks_parse(self):
        packages = json.loads((ROOT / 'tool/release-packages.json').read_text())
        release = workflow('release.yml')
        self.assertEqual(set(release['on']['workflow_dispatch']['inputs']['package']['options']), set(packages)|{'all'})
        self.assertEqual(release['permissions']['actions'], 'write')
        self.assertEqual(release['concurrency']['cancel-in-progress'], 'false')
        self.assertTrue(release['on']['schedule'])
        for file in (ROOT / '.github/workflows').glob('*.yml'):
            parsed = yaml.load(file.read_text(), Loader=yaml.BaseLoader)
            for job in parsed.get('jobs', {}).values():
                for step in job.get('steps', []):
                    if 'run' in step:
                        result = subprocess.run(['bash', '-n'], input=step['run'], text=True, capture_output=True)
                        self.assertEqual(result.returncode, 0, str(file)+': '+result.stderr)

    def test_deploy_disables_sdk_retries_so_coordinator_owns_cooldown(self):
        self.assertEqual(workflow('deploy.yml')['jobs']['publish']['env']['PUB_MAX_HTTP_RETRIES'], '1')

    def test_deploy_uploads_only_from_original_tag_push(self):
        steps = {step['name']: step for step in workflow('deploy.yml')['jobs']['publish']['steps'] if 'name' in step}
        self.assertIn("github.event_name == 'push'", steps['Publish']['if'])
        self.assertIn("github.event_name != 'push'", steps['Validate publish (dry run)']['if'])
        self.assertEqual(steps['Validate publish (dry run)']['run'], 'flutter pub publish --dry-run')

    def test_deploy_removes_overrides_before_hosted_resolution(self):
        package = self.root / 'package'
        package.mkdir()
        (package / 'pubspec.yaml').write_text('name: fixture\nversion: 1.0.0\n')
        (package / 'pubspec_overrides.yaml').write_text('dependency_overrides: {}\n')
        example = package / 'example'; example.mkdir()
        (example / 'pubspec_overrides.yaml').write_text('dependency_overrides: {}\n')
        self.fake('flutter', '''#!/usr/bin/env bash
if [ -n "$(find . -name pubspec_overrides.yaml -type f -print -quit)" ]; then exit 91; fi
if [ "$1 $2" != 'pub get' ]; then exit 92; fi
''')
        result = self.step('deploy.yml', 'Install dependencies', package)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((package / 'pubspec_overrides.yaml').exists())
        self.assertFalse((example / 'pubspec_overrides.yaml').exists())

    def test_actual_cli_preview_uses_source_graph_and_leaves_remote_untouched(self):
        fixture = fixtures.CoordinatorTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        root = fixture.root
        (root / 'tool').mkdir()
        for file in ROOT.glob('tool/release_*.py'):
            shutil.copy2(file, root / 'tool' / file.name)
        (root / 'tool/release-packages.json').write_text(json.dumps(fixture.fixture.packages))
        calls = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                calls.append(self.path)
                response = [] if self.path.startswith('/repos/') else fixture.pub.package(self.path.rsplit('/',1)[1])
                self.send_response(200); self.end_headers()
                self.wfile.write(json.dumps(response).encode())
            def do_POST(self):
                calls.append('MUTATION'); self.send_error(500)
            do_PATCH = do_POST
            def log_message(self, *args): pass
        server = ThreadingHTTPServer(('127.0.0.1',0), Handler)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        url = f'http://127.0.0.1:{server.server_port}'
        (self.bin / 'flutter').symlink_to(root / 'fake-flutter')
        env = {**fixture.repo.env, **self.env, 'GITHUB_API_URL':url, 'PUB_HOSTED_URL':url,
               'GH_TOKEN':'fixture-token','GITHUB_REPOSITORY':'owner/repo','GITHUB_RUN_ID':'12345'}
        before = fixture.fixture.git('ls-remote','origin')
        result = subprocess.run([sys.executable, 'tool/release_coordinator.py', '--dry-run'],
                                cwd=root, env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('demo_android',result.stdout)
        self.assertTrue(fixture.trace.exists())
        self.assertEqual(fixture.fixture.git('ls-remote','origin'),before)
        self.assertNotIn('MUTATION',calls)
        self.assertIsNone(fixture.journal.load())


if __name__ == '__main__': unittest.main()
