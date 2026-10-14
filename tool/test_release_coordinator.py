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

    def logs(self, run_id, step=None):
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
import json, os, pathlib, sys, tarfile
import yaml
if sys.argv[1:] == ['--version', '--machine']:
 print(json.dumps({'frameworkVersion': '3.47.7', 'frameworkRevision': 'a'*40, 'channel': 'stable'}))
 sys.exit(0)
p=pathlib.Path.cwd(); spec=yaml.safe_load((p/'pubspec.yaml').read_text())
with open(os.environ['GATE_TRACE'], 'a') as f:
 f.write(json.dumps({'name':spec['name'],'version':spec['version'],
  'linked':(p/'pubspec_overrides.yaml').exists(),'args':sys.argv[1:]})+'\\n')
if sys.argv[1:4] == ['pub','publish','--to-archive']:
 with tarfile.open(sys.argv[4], 'w:gz') as archive:
  for file in p.rglob('*'):
   if file.is_file(): archive.add(file, arcname=str(file.relative_to(p)))
 sys.exit(0)
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

    # Purpose: Keep example-generated files outside the frozen package archive.
    # /**
    #  * @param self The coordinator fixture with real git and a controlled SDK.
    #  * @returns None; asserts example validation and exact-source publication succeed.
    #  * @throws AssertionError if generated outputs contaminate archive verification.
    #  */
    def test_example_generation_does_not_change_expected_source_archive(self):
        example = self.root / self.fixture.packages['demo'] / 'example'
        example.mkdir()
        (example / 'pubspec.yaml').write_text(
            'name: demo_example\nversion: 1.0.0\npublish_to: none\ndependencies:\n'
            '  demo:\n    path: ../\n')
        self.fixture.commit('Add bundled example')
        self.fixture.git('push', 'origin', 'main')
        sdk = Path(self.repo.flutter)
        script = sdk.read_text()
        marker = "if sys.argv[1:4] == ['pub','publish','--to-archive']:"
        generation = ("if p.name == 'example' and sys.argv[1:] == ['pub','get']:\n"
                      " (p/'GeneratedPluginRegistrant.swift').write_text('// generated by example validation')\n"
                      " sys.exit(0)\n")
        self.assertIn(marker, script)
        sdk.write_text(script.replace(marker, generation + marker, 1))
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        self.assertEqual([p['state'] for p in plan['packages']], ['published', 'published'])
        gates = [json.loads(line) for line in self.trace.read_text().splitlines()]
        self.assertTrue(any(g['name'] == 'demo_example' and g['args'] == ['pub', 'get']
                            for g in gates))

    # Purpose: Rebuild archive evidence from frozen source rather than a stale cache.
    # /**
    #  * @param self The coordinator fixture with an exact published source archive.
    #  * @returns None; asserts stale validation output cannot block a correct receipt.
    #  * @throws AssertionError if source identity changes or cached output is trusted.
    #  */
    def test_receipt_rebuilds_archive_evidence_from_the_same_frozen_source(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        package = plan['packages'][-1]
        frozen = {key: copy.deepcopy(plan[key])
                  for key in ['source_sha', 'prepared_sha', 'files']}
        package['artifact_fingerprint'] = 'validation-output-cache'
        receipt = self.pub.receipt(package['name'], package['version'])
        from release_services import Blocked
        try:
            self.repo.verify_receipt(plan, package, receipt, self.pub)
        except Blocked as error:
            self.fail(f'Valid frozen source was rejected because of cached validation output: {error}')
        from release_repository import archive_fingerprint
        self.assertEqual(package['artifact_fingerprint'],
                         archive_fingerprint(self.pub.archives[receipt['archive_url']]))
        for key, value in frozen.items():
            self.assertEqual(plan[key], value)

    # Purpose: Reject changed published bytes even when a cached digest matches them.
    # /**
    #  * @param self The coordinator fixture with a deliberately modified archive.
    #  * @returns None; asserts every published file still binds to frozen source.
    #  * @throws AssertionError if a cached fingerprint bypasses source verification.
    #  */
    def test_matching_cached_digest_cannot_accept_changed_published_source(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        package = plan['packages'][-1]
        receipt = self.pub.receipt(package['name'], package['version'])
        url = receipt['archive_url']
        output = io.BytesIO()
        with (tarfile.open(fileobj=io.BytesIO(self.pub.archives[url])) as source,
              tarfile.open(fileobj=output, mode='w:gz') as target):
            for entry in source:
                if not entry.isfile():
                    continue
                payload = source.extractfile(entry).read()
                if entry.name == 'lib/plugin.dart':
                    payload = b'// modified after frozen validation\n'
                    entry.size = len(payload)
                target.addfile(entry, io.BytesIO(payload))
        import hashlib
        from release_repository import archive_fingerprint
        from release_services import Blocked
        self.pub.archives[url] = output.getvalue()
        receipt['archive_sha256'] = hashlib.sha256(output.getvalue()).hexdigest()
        package['artifact_fingerprint'] = archive_fingerprint(output.getvalue())
        with self.assertRaisesRegex(Blocked, 'does not match frozen source'):
            self.repo.verify_receipt(plan, package, receipt, self.pub)

    # Purpose: Avoid a second network-dependent archive build after fresh validation.
    # /**
    #  * @param self The coordinator fixture with a real source and published archive.
    #  * @returns None; asserts recovery builds one complete archive on a cache miss.
    #  * @throws AssertionError if receipt reconciliation repeats fresh SDK packaging.
    #  */
    def test_receipt_cache_miss_packages_frozen_source_once(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        package = plan['packages'][-1]
        package.pop('artifact_fingerprint')
        receipt = self.pub.receipt(package['name'], package['version'])
        before = len(self.trace.read_text().splitlines())
        self.repo.verify_receipt(plan, package, receipt, self.pub)
        calls = [json.loads(line)['args'] for line in self.trace.read_text().splitlines()[before:]]
        archives = [call for call in calls if call[:3] == ['pub', 'publish', '--to-archive']]
        self.assertEqual(len(archives), 1)

    # Purpose: Reject a moving SDK before rebuilding an immutable receipt archive.
    # /**
    #  * @param self The coordinator with an already frozen SDK identity.
    #  * @returns None; asserts an installed different revision cannot package it.
    #  * @throws AssertionError if SDK drift silently changes expected evidence.
    #  */
    def test_recovery_blocks_before_packaging_with_a_different_sdk(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        plan['packaging_sdk']['revision'] = 'b' * 40
        from release_services import Blocked
        before = self.trace.read_text()
        with self.assertRaisesRegex(Blocked, 'SDK'):
            self.repo.frozen_archive_fingerprint(plan, plan['packages'][-1])
        self.assertEqual(self.trace.read_text(), before)

    # Purpose: Reject journal SDK drift on every direct receipt-recovery path.
    # /**
    #  * @param self The source-pinned release fixture and mutable journal identity.
    #  * @returns None; asserts the prepared SDK pin remains authoritative.
    #  * @throws AssertionError if divergent journal metadata overrides frozen source.
    #  */
    def test_receipt_sdk_metadata_cannot_override_frozen_source_pin(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        plan['packaging_sdk']['revision'] = 'b' * 40
        from release_services import Blocked
        with self.assertRaisesRegex(Blocked, 'Frozen SDK'):
            self.repo.expected_sdk(plan)

    # Purpose: Reconcile a prior batch while retaining a newer SDK for a fresh cut.
    # /**
    #  * @param self The fixture with separately installed current and prior SDKs.
    #  * @returns None; asserts the frozen SDK binary is selected without downgrading current.
    #  * @throws AssertionError if SDK selection loses either packaging identity.
    #  */
    def test_recovery_selects_frozen_binary_and_keeps_current_sdk(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        original_binary = self.repo.flutter
        newer_binary = self.root / 'newer-flutter'
        script = Path(original_binary).read_text().replace("'3.47.7'", "'3.50.0'")
        script = script.replace("'frameworkRevision': 'a'*40", "'frameworkRevision': 'b'*40")
        newer_binary.write_text(script)
        newer_binary.chmod(0o755)
        self.repo.flutter = str(newer_binary)
        self.repo.env['RELEASE_BATCH_FLUTTER'] = original_binary
        self.assertEqual(self.repo.sdk_flutter(plan), original_binary)
        self.assertEqual(self.repo.installed_sdk()['version'], '3.50.0')
        receipt = self.pub.receipt(plan['packages'][-1]['name'], plan['packages'][-1]['version'])
        self.repo.verify_receipt(plan, plan['packages'][-1], receipt, self.pub)

    # Purpose: Carry the same SDK identity in durable intent and prepared source.
    # /**
    #  * @param self The coordinator fixture with a stable installed SDK.
    #  * @returns None; asserts both immutable release records contain its identity.
    #  * @throws AssertionError if publishing cannot install the preflight SDK.
    #  */
    def test_sdk_identity_is_frozen_before_intent_and_in_prepared_source(self):
        self.assertEqual(self.cut(), 'complete')
        plan = self.journal.load()
        intent = json.loads(self.fixture.git('for-each-ref', '--format=%(contents)',
                                            'refs/tags/' + plan['batch_tag']))
        pin = json.loads(self.fixture.git('show', plan['prepared_sha'] + ':tool/release-sdk.json'))
        self.assertEqual(intent['packaging_sdk'], plan['packaging_sdk'])
        self.assertEqual(pin, plan['packaging_sdk'])

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

    def test_unknown_quota_repeatedly_pauses_until_available_then_resumes(self):
        self.journal.outcomes['demo_android'] = 'quota'
        self.journal.failure_log = 'rate limit has been reached (unknown window)'
        retry = self.journal.retry
        def still_limited_once(run_id):
            retry(run_id)
            if len(self.journal.retries) == 1:
                self.journal.outcomes['demo_android'] = 'quota'
        self.journal.retry = still_limited_once
        self.assertEqual(self.cut(), 'deferred')
        first = self.journal.load()
        self.clock.value += 86399
        self.assertEqual(self.runner().run('resume', scheduled=True), 'deferred')
        self.assertEqual(self.journal.retries, [])
        self.clock.value += 2
        self.assertEqual(self.runner().run('resume', scheduled=True), 'deferred')
        self.assertEqual(self.journal.load()['not_before'], self.clock.now()+86400)
        self.assertNotIn('demo-v1.0.1', self.fixture.git('ls-remote', '--tags', 'origin'))
        self.clock.value += 86401
        self.assertEqual(self.runner().run('resume', scheduled=True), 'complete')
        self.assertEqual(self.journal.load()['prepared_sha'], first['prepared_sha'])
        self.assertEqual(self.journal.retries, [10, 10])

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
        self.assertEqual(self.runner().run('cut',package='demo_ios'), 'blocked')
        self.assertIn('first manual', self.journal.load()['error'])
        self.assertEqual(self.journal.run_calls, [])

    def test_embedded_overrides_block_before_source_push_or_tag(self):
        for relative in ['packages/android/pubspec.yaml', 'packages/android/example/pubspec.yaml']:
            with self.subTest(pubspec=relative):
                file = self.root / relative
                file.parent.mkdir(parents=True, exist_ok=True)
                original = file.read_text() if file.exists() else 'name: demo_example\nversion: 1.0.0\n'
                file.write_text(original + 'dependency_overrides:\n  external_package:\n    path: /unpublished/local/package\n')
                self.fixture.commit('Add embedded development override')
                self.fixture.git('push', 'origin', 'main')
                before = self.fixture.git('ls-remote', 'origin')
                self.assertEqual(self.cut(), 'blocked')
                self.assertIn('dependency_overrides', self.journal.load()['error'])
                self.assertEqual(self.fixture.git('ls-remote', 'origin', 'refs/heads/main'),
                                 '\n'.join(line for line in before.splitlines() if line.endswith('refs/heads/main')))
                self.assertEqual(self.journal.run_calls, [])
                self.assertFalse(self.trace.exists())
                self.journal.file.unlink()
                # Start another authorized fixture cut rather than reuse its intent tag.
                self.fixture.git('push', 'origin', ':refs/tags/sz-release-batch-12345')
                self.fixture.git('tag', '-d', 'sz-release-batch-12345')
                file.write_text(original)
                self.fixture.commit('Remove fixture override')

    def test_individual_cut_ignores_unrelated_embedded_overrides(self):
        file = self.root / 'packages/ios/pubspec.yaml'
        file.write_text(file.read_text() + 'dependency_overrides:\n  external_package:\n    path: /unrelated/development/package\n')
        self.fixture.commit('Add unrelated development override')
        self.fixture.git('push', 'origin', 'main')
        self.assertEqual(self.runner().run('cut', package='demo_android'), 'complete')
        self.assertEqual(self.journal.run_calls, ['demo_android'])

    def test_external_local_and_git_sources_block_before_sdk_or_push(self):
        file = self.root / 'packages/android/pubspec.yaml'
        original = file.read_text()
        for source in ['path: ../external', 'git: https://example.invalid/package.git', 'hosted: https://example.invalid']:
            with self.subTest(source=source):
                file.write_text(original + 'dependencies:\n  external_package:\n    '+source+'\n')
                self.fixture.commit('Add external development source')
                self.fixture.git('push', 'origin', 'main')
                before = self.fixture.git('ls-remote', 'origin', 'refs/heads/main')
                self.assertEqual(self.cut(), 'blocked')
                self.assertEqual(self.fixture.git('ls-remote', 'origin', 'refs/heads/main'), before)
                self.assertFalse(self.trace.exists())
                self.journal.file.unlink()
                self.fixture.git('push', 'origin', ':refs/tags/sz-release-batch-12345')
                self.fixture.git('tag', '-d', 'sz-release-batch-12345')

    def test_invalid_example_path_type_records_blocked_before_sdk(self):
        file = self.root / 'packages/android/example/pubspec.yaml'
        file.parent.mkdir(parents=True)
        file.write_text('name: demo_example\nversion: 1.0.0\ndependencies:\n  demo_android:\n    path: true\n')
        self.fixture.commit('Add invalid example path value')
        self.fixture.git('push', 'origin', 'main')
        self.assertEqual(self.cut(), 'blocked')
        self.assertIn('path', self.journal.load()['error'])
        self.assertEqual(self.journal.run_calls, [])
        self.assertFalse(self.trace.exists())

    def test_validation_disables_sdk_http_quota_retries(self):
        self.assertEqual(self.repo.env.get('PUB_MAX_HTTP_RETRIES'), '1')

    def test_a_hung_validation_command_blocks_instead_of_hanging_the_runner(self):
        slow = self.root / 'slow-flutter'
        slow.write_text('#!/bin/sh\nsleep 5\n')
        slow.chmod(0o755)
        services = importlib.import_module('release_repository')
        repo = services.Repository(self.root, flutter=str(slow), validation_timeout=0.2)
        with self.assertRaisesRegex(services.Blocked, 'demo validation timed out after 0.2s: analyze'):
            repo.run_flutter(['analyze', '--no-fatal-warnings'], self.root, 'demo')

    def test_packaging_is_checked_before_the_release_commit_lands(self):
        self.assertEqual(self.cut(), 'complete')
        gates = [json.loads(line) for line in self.trace.read_text().splitlines()]
        linked = [gate['name'] for gate in gates
                  if gate['linked'] and gate['args'] == ['pub', 'publish', '--dry-run']]
        self.assertEqual(linked, ['demo_android', 'demo'])

    def test_modified_published_assets_block_dependents(self):
        asset=self.root/'packages/android/assets/message.txt'
        asset.parent.mkdir();asset.write_text('frozen asset')
        self.fixture.commit('Add shipped asset');self.fixture.git('push','origin','main')
        publish=self.pub.publish
        def tampered(package,sha):
            publish(package,sha)
            receipt=self.pub.receipt(package['name'],package['version']);url=receipt['archive_url']
            out=io.BytesIO()
            with tarfile.open(fileobj=io.BytesIO(self.pub.archives[url])) as source:
                with tarfile.open(fileobj=out,mode='w:gz') as target:
                    for entry in source:
                        if not entry.isfile():
                            continue
                        payload=source.extractfile(entry).read()
                        if entry.name=='assets/message.txt':payload=b'changed asset';entry.size=len(payload)
                        target.addfile(entry,io.BytesIO(payload))
            import hashlib
            self.pub.archives[url]=out.getvalue()
            receipt['archive_sha256']=hashlib.sha256(out.getvalue()).hexdigest()
        self.pub.publish=tampered
        self.assertEqual(self.cut(),'blocked')
        self.assertNotIn('demo-v1.0.1',self.fixture.git('ls-remote','--tags','origin'))

    def test_metadata_cooldown_preserves_completed_planning_reads(self):
        read=self.pub.package
        interface_reads=[]
        paused=[False]
        def limited(name):
            if name=='demo_interface':interface_reads.append(name)
            if name=='demo_android' and not paused[0]:
                paused[0]=True
                raise importlib.import_module('release_services').Deferred(self.clock.now()+3600,'Metadata quota')
            return read(name)
        self.pub.package=limited
        self.assertEqual(self.cut(),'deferred')
        self.clock.value+=3601
        self.assertEqual(self.runner().run('resume',scheduled=True),'complete')
        self.assertEqual(interface_reads,['demo_interface'])

    def test_unpublished_unrelated_package_does_not_block_individual_cut(self):
        self.pub.metadata.pop('demo_ios')
        self.assertEqual(self.runner().run('cut',package='demo_android'),'complete')
        self.assertEqual([p['name'] for p in self.journal.load()['packages']],['demo_android'])

    def test_all_skips_unchanged_unpublished_package_present_at_repository_baseline(self):
        self.fixture.packages['ghost']='packages/ghost'
        self.fixture.package('ghost','packages/ghost',{})
        file=self.root/'packages/demo/pubspec.yaml'
        file.write_text(file.read_text().replace('version: 1.0.0','version: 1.0.2'))
        self.fixture.commit('Previously released facade with unpublished unused package')
        self.fixture.git('tag','demo-v1.0.2');self.fixture.git('push','origin','main','--tags')
        self.pub.metadata['demo']['versions'].append({'version':'1.0.2'})
        self.assertEqual(self.cut(),'complete')
        self.assertNotIn('ghost',[p['name'] for p in self.journal.load()['packages']])

    def test_cancelled_inflight_deploy_blocks_a_new_cut_without_replacing_journal(self):
        self.journal.outcomes['demo_android']='running'
        self.cut();self.runner().run('cancel');self.clock.value+=121
        before=self.journal.load()
        self.assertEqual(self.runner('12346').run('cut'),'blocked')
        self.assertEqual(self.journal.load(),before)

    def test_preview_after_cancellation_does_not_reconcile_by_writing_journal(self):
        self.journal.outcomes['demo_android']='running'
        self.cut();self.runner().run('cancel');self.clock.value+=121
        before=self.journal.load()
        self.journal.outcomes['demo_android']='success'
        self.assertEqual(self.runner('12346').run('cut',dry_run=True),'preview')
        self.assertEqual(self.journal.load(),before)

    def test_new_cut_after_cancel_reconciles_completed_original_publication(self):
        self.journal.outcomes['demo_android']='running'
        self.cut();self.runner().run('cancel');self.clock.value+=121
        self.journal.outcomes['demo_android']='success'
        self.assertEqual(self.runner('12346').run('cut'),'complete')
        self.assertEqual(self.journal.load()['batch_tag'],'sz-release-batch-12346')
        self.assertEqual(len([v for v in self.pub.metadata['demo_android']['versions']
                             if v['version']=='1.0.1']),1)
        self.assertNotIn('demo-v1.0.1',self.fixture.git('ls-remote','--tags','origin'))

    def test_latest_completed_batch_publications_count_toward_new_agent_budget(self):
        self.assertEqual(self.cut(),'complete')
        self.fixture.git('reset','--hard','origin/main')  # disposable test checkout only
        (self.root/'packages/android/lib/plugin.dart').write_text('// next source change')
        self.fixture.commit('Further Android work');self.fixture.git('push','origin','main')
        self.journal.known_publications=[self.clock.now()-1]*198
        self.assertEqual(self.runner('12346').run('cut'),'deferred')
        self.assertIn('quota',self.journal.load()['error'])
        self.assertNotIn('demo_android-v1.0.2',self.fixture.git('ls-remote','--tags','origin'))

    def test_new_cut_cannot_replace_an_incomplete_batch(self):
        self.journal.outcomes['demo_android'] = 'running'
        self.assertEqual(self.cut(), 'deferred')
        first = self.journal.load()
        self.assertEqual(self.runner('12346').run('cut'), 'blocked')
        self.assertEqual(self.journal.load(), first)


if __name__ == '__main__':
    unittest.main()
