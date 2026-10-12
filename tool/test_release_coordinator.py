"""Batch recovery uses real git snapshots, bare remotes, and durable journals.

Only GitHub Deploy and pub.dev publication are controlled external services.
"""
import copy
import importlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import unittest

import test_release_plan as fixtures
from test_release_services import Clock


class Journal:
    def __init__(self, file, pub, repo):
        self.file, self.pub, self.repo = file, pub, repo
        self.outcomes = {}
        self.retries = []
        self.run_calls = []
        self.run_attempts = {}
        self.before_save = None

    def load(self):
        return json.loads(self.file.read_text()) if self.file.exists() else None

    def save(self, plan):
        if self.before_save:
            self.before_save(plan)
        self.file.write_text(json.dumps(plan))

    def create(self, plan):
        plan['journal_id'] = 1
        self.save(plan)

    def run(self, package, sha):
        self.run_calls.append(package['name'])
        outcome = self.outcomes.get(package['name'], 'success')
        if outcome == 'success':
            self.pub.publish(package, sha)
        return {'id': 10 if package['name'] == 'demo_android' else 20,
                'status': 'in_progress' if outcome == 'running' else 'completed',
                'conclusion': None if outcome == 'running' else
                              'failure' if outcome not in ('success', 'visibility') else 'success',
                'attempt': self.run_attempts.get(package['name'], 1)}

    def logs(self, run_id):
        return getattr(self, 'failure_log', 'Manifest validation failed')

    def retry(self, run_id):
        self.retries.append(run_id)
        for name, outcome in list(self.outcomes.items()):
            if outcome not in ('success', 'running'):
                self.outcomes[name] = 'success'
                self.run_attempts[name] = self.run_attempts.get(name, 1) + 1


class Pub:
    def __init__(self, fixture, repo, clock):
        self.metadata = copy.deepcopy(fixture.published)
        self.repo, self.clock, self.archives = repo, clock, {}
        self.raise_on_read = None

    def package(self, name):
        if self.raise_on_read:
            raise self.raise_on_read
        return self.metadata.get(name)

    def receipt(self, name, wanted):
        return next((v for v in (self.package(name) or {}).get('versions', [])
                     if v['version'] == wanted), None)

    def next_check(self, name):
        return self.clock.now() + 120

    def publish(self, package, sha):
        if self.receipt(package['name'], package['version']):
            return
        archive = subprocess.check_output(['git', 'archive', sha, package['path']], cwd=self.repo.root)
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            out = io.BytesIO()
            with tarfile.open(fileobj=out, mode='w:gz') as target:
                for entry in source:
                    prefix = '' if package['path'] == '.' else package['path'] + '/'
                    if entry.isfile():
                        payload = source.extractfile(entry).read()
                        entry.name = entry.name.removeprefix(prefix)
                        target.addfile(entry, io.BytesIO(payload))
        raw = out.getvalue()
        import hashlib
        url = f'https://pub.dev/api/archives/{package["name"]}-{package["version"]}.tar.gz'
        self.archives[url] = raw
        self.metadata[package['name']]['versions'].append(
            {'version': package['version'], 'archive_url': url,
             'archive_sha256': hashlib.sha256(raw).hexdigest(),
             'published': '2023-11-14T22:13:20Z'})

    def get(self, url, *, binary=False):
        assert binary
        return self.archives[url]


class CoordinatorTest(unittest.TestCase):
    def setUp(self):
        try:
            self.module = importlib.import_module('release_coordinator')
            services = importlib.import_module('release_repository')
        except (ModuleNotFoundError, AttributeError):
            self.fail('Durable batch coordinator is missing')
        self.fixture = fixtures.PlannerTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.change('demo_android')
        self.root = self.fixture.root
        self.remote = self.root.parent / (self.root.name + '-remote.git')
        subprocess.run(['git', 'init', '--bare', str(self.remote)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(__import__('shutil').rmtree, self.remote)
        self.fixture.git('remote', 'add', 'origin', str(self.remote))
        self.fixture.git('push', '-u', 'origin', 'main')
        self.clock = Clock()
        self.trace = self.root / 'gate-trace.jsonl'
        flutter = self.root / 'fake-flutter'
        flutter.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
import yaml
p=pathlib.Path.cwd(); spec=yaml.safe_load((p/'pubspec.yaml').read_text())
with open(os.environ['GATE_TRACE'], 'a') as f:
 f.write(json.dumps({'name':spec['name'],'version':spec['version'],
  'linked':(p/'pubspec_overrides.yaml').exists(),'args':sys.argv[1:]})+'\\n')
if sys.argv[1:] not in [['pub','get','--no-example'], ['analyze','--no-fatal-warnings'],
                       ['test'], ['pub','publish','--dry-run']]: sys.exit(91)
''')
        flutter.chmod(0o755)
        self.repo = services.Repository(self.root, flutter=str(flutter),
                                       env={'GATE_TRACE': str(self.trace)})
        self.pub = Pub(self.fixture, self.repo, self.clock)
        self.journal = Journal(self.root / 'journal.json', self.pub, self.repo)

    def runner(self, run_id="12345"):
        return self.module.Coordinator(self.repo, self.journal, self.pub,
                                       self.fixture.packages, now=self.clock.now, sleep=self.clock.sleep,
                                       run_id=run_id, wait_budget=0)

    def cut(self, **kwargs):
        return self.runner().run('cut', package='all', bump='patch', **kwargs)

    def test_batch_validates_and_publishes_in_dependency_order(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        self.assertEqual([p['name'] for p in plan['packages']], ['demo_android', 'demo'])
        self.assertEqual([p['state'] for p in plan['packages']], ['published', 'published'])
        self.assertEqual(self.journal.run_calls, ['demo_android', 'demo'])
        gates = [json.loads(line) for line in self.trace.read_text().splitlines()]
        hosted = [gate['name'] for gate in gates if not gate['linked'] and
                  gate['args'] == ['pub', 'publish', '--dry-run']]
        self.assertEqual(hosted, ['demo_android', 'demo'])
        self.assertNotIn('demo_ios-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))

    def test_daily_rate_limit_pauses_and_resumes_with_original_versions(self):
        self.journal.outcomes['demo_android'] = 'quota'
        self.journal.failure_log = ('The "package-published" operation is blocked, as its rate limit '
                                    'has been reached (12 in the last day). Please try again later.')
        self.assertEqual(self.cut(), 'deferred')
        first = self.journal.load()
        self.assertEqual(first['not_before'], 1700086400)
        self.assertNotIn('demo-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))
        self.clock.value += 86399
        self.assertEqual(self.runner().run('resume', scheduled=True), 'deferred')
        self.assertEqual(self.journal.retries, [])
        self.clock.value += 2
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        resumed = self.journal.load()
        self.assertEqual(resumed['prepared_sha'], first['prepared_sha'])
        self.assertEqual([p['version'] for p in resumed['packages']], ['1.0.1', '1.0.1'])
        self.assertEqual(self.journal.retries, [10])

    def test_partial_success_is_not_uploaded_again_after_a_fresh_run(self):
        self.journal.outcomes['demo'] = 'quota'
        self.journal.failure_log = 'rate limit has been reached (6 in the last hour)'
        self.assertEqual(self.cut(), 'deferred')
        first_sha = self.journal.load()['prepared_sha']
        self.clock.value += 3601
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        self.assertEqual(self.journal.run_calls.count('demo_android'), 1)
        self.assertEqual(self.journal.load()['prepared_sha'], first_sha)

    def test_successful_deploy_waits_for_exact_version_visibility(self):
        self.journal.outcomes['demo_android'] = 'visibility'
        self.assertEqual(self.cut(), 'deferred')
        self.assertEqual(self.journal.load()['packages'][0]['state'], 'tagged')
        self.assertNotIn('demo-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))
        self.clock.value += 121
        self.pub.publish(self.journal.load()['packages'][0], self.journal.load()['prepared_sha'])
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        self.assertEqual(self.journal.retries, [])

    def test_existing_wrong_artifact_at_candidate_version_blocks_batch(self):
        self.journal.outcomes['demo_android'] = 'running'
        self.assertEqual(self.cut(), 'deferred')
        plan = self.journal.load()
        self.pub.publish(plan['packages'][0], plan['source_sha'])
        self.clock.value += 121
        self.assertEqual(self.runner().run('resume'), 'blocked')
        self.assertIn('artifact', self.journal.load()['error'])
        self.assertNotIn('demo-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))

    def test_initial_metadata_quota_keeps_authorized_intent_for_automatic_resume(self):
        self.pub.raise_on_read = importlib.import_module('release_services').Deferred(
            self.clock.now() + 3600, 'Metadata quota')
        self.assertEqual(self.cut(), 'deferred')
        self.assertEqual(self.journal.load()['phase'], 'planning')
        self.clock.value += 3601
        self.pub.raise_on_read = None
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')

    def test_dry_run_keeps_git_and_journal_unchanged(self):
        before = self.fixture.git('ls-remote', 'origin')
        self.assertEqual(self.cut(dry_run=True), 'preview')
        self.assertIsNone(self.journal.load())
        self.assertEqual(self.fixture.git('ls-remote', 'origin'), before)

    def test_schedule_without_an_authorized_batch_is_noop(self):
        self.assertEqual(self.runner().run('resume', scheduled=True), 'noop')
        self.assertIsNone(self.journal.load())
        self.assertFalse(self.trace.exists())

    def test_cancellation_prevents_any_further_publication(self):
        self.journal.outcomes['demo_android'] = 'running'
        self.assertEqual(self.cut(), 'deferred')
        self.assertEqual(self.runner().run('cancel'), 'cancelled')
        self.clock.value += 1000
        self.assertEqual(self.runner().run('resume', scheduled=True), 'noop')
        self.assertNotIn('demo-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))

    def test_crash_after_main_push_recovers_same_prepared_sha(self):
        push = self.repo.push_prepared
        called = []
        def crash(plan):
            push(plan)
            called.append(plan['prepared_sha'])
            raise RuntimeError('Runner stopped after source push')
        self.repo.push_prepared = crash
        with self.assertRaisesRegex(RuntimeError, 'Runner stopped'):
            self.cut()
        self.repo.push_prepared = push
        self.assertEqual(self.runner().run('resume'), 'complete')
        self.assertEqual(self.journal.load()['prepared_sha'], called[0])

    def assert_attention(self, message):
        self.journal.outcomes['demo_android'] = 'failure'
        self.journal.failure_log = message
        self.assertEqual(self.cut(), 'blocked')
        self.clock.value += 90000
        self.assertEqual(self.runner().run('resume', scheduled=True), 'noop')
        self.assertEqual(self.journal.retries, [])

    def test_unknown_quota_window_pauses_for_attention(self):
        self.assert_attention('rate limit has been reached (unknown window)')

    def test_validation_errors_are_not_retried_by_schedule(self):
        self.assert_attention('Analyzer error')

    def test_cancel_preview_does_not_cancel_authorized_batch(self):
        self.journal.outcomes['demo_android'] = 'running'
        self.cut()
        before = self.journal.load()
        self.assertEqual(self.runner().run('cancel', dry_run=True), 'preview')
        self.assertEqual(self.journal.load(), before)

    def test_orphan_intent_after_journal_creation_crash_resumes(self):
        create = self.journal.create
        def crash(plan):
            raise RuntimeError('Journal creation interrupted')
        self.journal.create = crash
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            self.cut()
        self.journal.create = create
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        self.assertEqual(self.journal.load()['batch_tag'], 'sz-release-batch-12345')

    def test_original_dispatch_rerun_resumes_instead_of_selecting_new_versions(self):
        self.journal.outcomes['demo_android'] = 'running'
        self.cut()
        before = self.journal.load()
        self.clock.value += 121
        self.journal.outcomes['demo_android'] = 'success'
        self.assertEqual(self.cut(), 'complete')
        self.assertEqual(self.journal.load()['prepared_sha'], before['prepared_sha'])

    def test_burst_quota_waits_full_window_before_original_run_retry(self):
        self.journal.outcomes['demo_android'] = 'quota'
        self.journal.failure_log = 'rate limit has been reached (3 in the last few minutes)'
        self.assertEqual(self.cut(), 'deferred')
        self.clock.value += 119
        self.assertEqual(self.runner().run('resume', scheduled=True), 'deferred')
        self.assertEqual(self.journal.retries, [])
        self.clock.value += 2
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        self.assertEqual(self.journal.retries, [10])

    def test_known_package_quota_defers_before_tagging(self):
        from datetime import datetime, timezone
        for i in range(12):
            self.pub.metadata['demo_android']['versions'].append({
                'version': f'0.0.{i}', 'published': datetime.fromtimestamp(
                    self.clock.now()-i-1, timezone.utc).isoformat()})
        self.assertEqual(self.cut(), 'deferred')
        self.assertEqual(self.journal.run_calls, [])
        self.assertNotIn('demo_android-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))
        self.assertGreater(self.journal.load()['not_before'], self.clock.now()+86000)

    def test_crash_after_package_tag_reconciles_without_retagging(self):
        save = self.journal.save
        def crash(plan):
            if plan['packages'] and plan['packages'][0]['state'] == 'tagged':
                raise RuntimeError('Checkpoint interrupted after tag')
            save(plan)
        self.journal.save = crash
        with self.assertRaisesRegex(RuntimeError, 'after tag'):
            self.cut()
        tagged = self.fixture.git('ls-remote', '--tags', 'origin')
        self.journal.save = save
        self.assertEqual(self.runner().run('resume'), 'complete')
        for ref in tagged.splitlines():
            self.assertIn(ref, self.fixture.git('ls-remote', '--tags', 'origin'))

    def test_crash_after_rerun_request_does_not_rerun_twice(self):
        self.journal.outcomes['demo_android'] = 'quota'
        self.journal.failure_log = 'rate limit has been reached (6 in the last hour)'
        self.cut()
        self.clock.value += 3601
        retry = self.journal.retry
        def crash(run_id):
            retry(run_id)
            raise RuntimeError('Rerun response interrupted')
        self.journal.retry = crash
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            self.runner().run('resume')
        self.journal.retry = retry
        self.assertEqual(self.runner().run('resume'), 'complete')
        self.assertEqual(self.journal.retries, [10])

    def test_transient_upload_error_retries_original_deploy_after_backoff(self):
        self.journal.outcomes['demo_android'] = 'transient'
        self.journal.failure_log = 'HTTP error 503 Service Unavailable'
        self.assertEqual(self.cut(), 'deferred')
        self.assertEqual(self.journal.retries, [])
        self.clock.value = self.journal.load()['not_before'] + 1
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        self.assertEqual(self.journal.retries, [10])

    def test_main_advancement_blocks_release_push_without_package_tags(self):
        push = self.repo.push_prepared
        def advance(plan):
            (self.root / 'concurrent.txt').write_text('Concurrent main commit')
            self.fixture.commit('Concurrent work')
            self.fixture.git('push', 'origin', 'main')
            push(plan)
        self.repo.push_prepared = advance
        self.assertEqual(self.cut(), 'blocked')
        self.assertIn('main advanced', self.journal.load()['error'])
        self.assertNotIn('demo_android-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))

    def test_conflicting_package_tag_is_preserved_and_blocks(self):
        self.fixture.git('tag', 'demo_android-v1.0.1')
        self.fixture.git('push', 'origin', 'demo_android-v1.0.1')
        before = self.fixture.git('rev-parse', 'demo_android-v1.0.1')
        self.assertEqual(self.cut(), 'blocked')
        self.assertIn('different source SHA', self.journal.load()['error'])
        self.assertEqual(self.fixture.git('rev-parse', 'demo_android-v1.0.1'), before)

    def test_known_agent_quota_defers_before_any_package_tag(self):
        self.journal.known_publications = [self.clock.now()-1] * 200
        self.assertEqual(self.cut(), 'deferred')
        self.assertGreater(self.journal.load()['not_before'], self.clock.now()+86000)
        self.assertEqual(self.journal.run_calls, [])

    def test_transient_upload_retries_are_bounded(self):
        self.journal.outcomes['demo_android'] = 'transient'
        self.journal.failure_log = 'HTTP error 503 Service Unavailable'
        def still_failing(run_id):
            self.journal.retries.append(run_id)
            self.journal.run_attempts['demo_android'] = self.journal.run_attempts.get('demo_android', 1)+1
        self.journal.retry = still_failing
        self.assertEqual(self.cut(), 'deferred')
        for i in range(3):
            self.clock.value = self.journal.load()['not_before'] + 1
            self.assertEqual(self.runner().run('resume', scheduled=True), 'blocked' if i == 2 else 'deferred')
        self.assertEqual(self.journal.retries, [10, 10, 10])
        self.assertEqual(self.runner().run('resume', scheduled=True), 'noop')

    def test_rerun_acceptance_waits_for_new_attempt_instead_of_reapplying_old_quota(self):
        self.journal.outcomes['demo_android'] = 'quota'
        self.journal.failure_log = 'rate limit has been reached (12 in the last day)'
        self.cut()
        self.clock.value += 86401
        self.journal.retry = lambda run_id: self.journal.retries.append(run_id)
        self.assertEqual(self.runner().run('resume'), 'deferred')
        plan = self.journal.load()
        self.assertLess(plan['not_before'], self.clock.now()+180)
        self.clock.value = plan['not_before']+1
        self.assertEqual(self.runner().run('resume'), 'deferred')
        self.assertEqual(self.journal.retries,[10])

    def test_first_publication_requires_manual_package_creation(self):
        self.pub.metadata.pop('demo_ios')
        self.assertEqual(self.cut(), 'blocked')
        self.assertIn('first manual', self.journal.load()['error'])
        self.assertEqual(self.journal.run_calls, [])

    def test_new_cut_cannot_replace_an_incomplete_batch(self):
        self.journal.outcomes['demo_android'] = 'running'
        self.assertEqual(self.cut(), 'deferred')
        first = self.journal.load()
        self.assertEqual(self.runner('12346').run('cut'), 'blocked')
        self.assertEqual(self.journal.load(), first)


if __name__ == '__main__':
    unittest.main()
