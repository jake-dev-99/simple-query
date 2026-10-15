"""Manual release intent, sequential tag publication, and durable automatic resume."""
import argparse
import json
import os
from pathlib import Path
import re
import random
import time

import yaml

from release_plan import build_plan, bump_version
from release_services import Blocked, Deferred, publication_times, quota_deadline, upload_deadline


class Coordinator:
    def __init__(self, repo, journal, pub, packages, *, now=time.time,
                 sleep=time.sleep, run_id=None, wait_budget=2700):
        self.repo, self.journal, self.pub, self.packages = repo, journal, pub, packages
        self.now, self.sleep = now, sleep
        self.run_id = str(run_id or os.environ['GITHUB_RUN_ID'])
        self.deadline = now() + wait_budget

    def save(self, plan):
        self.journal.save(plan)

    def validate_plan(self, plan):
        if plan.get('schema') != 1 or plan.get('package_paths') != self.packages:
            raise Blocked('Journal schema or package configuration differs from this repository')
        if not re.fullmatch(r'[0-9a-f]{40}', plan.get('source_sha', '')):
            raise Blocked('Invalid frozen source SHA')
        if not re.fullmatch(r'sz-release-batch-\d+', plan.get('batch_tag', '')):
            raise Blocked('Invalid batch intent tag')
        records = {p['name']: p for p in plan['packages']}
        allowed = set()
        for package in plan['packages']:
            if package['name'] not in self.packages or package['path'] != self.packages[package['name']]:
                raise Blocked('Journal contains an unknown package path')
            if bump_version(package['current'], plan['bump']) != package['version']:
                raise Blocked('Journal version differs from frozen bump')
            if package['tag'] != package['name'] + '-v' + package['version']:
                raise Blocked('Journal tag differs from candidate version')
            prefix = '' if package['path'] == '.' else package['path'] + '/'
            allowed.update({prefix + 'pubspec.yaml', prefix + 'CHANGELOG.md'})
            manifest = yaml.safe_load(plan['files'][prefix + 'pubspec.yaml'])
            if manifest['name'] != package['name'] or str(manifest['version']) != package['version']:
                raise Blocked('Frozen manifest does not match its package')
            expected = {name: records[name]['version'] for section in ('dependencies', 'dev_dependencies')
                        for name in (manifest.get(section) or {}) if name in records}
            if package['prerequisites'] != expected:
                raise Blocked('Journal prerequisites differ from frozen dependency graph')
        from release_toolchain import SDK_FILE, validate_sdk
        if SDK_FILE in plan['files']:
            allowed.add(SDK_FILE)
            if validate_sdk(json.loads(plan['files'][SDK_FILE])) != validate_sdk(plan.get('packaging_sdk')):
                raise Blocked('Frozen SDK file differs from the batch identity')
        if set(plan['files']) != allowed:
            raise Blocked('Journal contains files outside the selected release manifests/changelogs')
        seen = set()
        for package in plan['packages']:
            if not set(package['prerequisites']).issubset(seen):
                raise Blocked('Journal packages are not in dependency order')
            seen.add(package['name'])

    def reconcile_cancelled(self, plan, *, dry_run=False):
        if not plan.get('prepared_sha'):
            return
        if self.now() < plan['not_before']:
            raise Blocked('Cancelled batch cooldown is still active; wait before authorizing another cut')
        for package in plan['packages']:
            if package['state'] in ('published', 'aborted'):
                continue
            tagged = self.repo.remote_tag_sha(package['tag'])
            if tagged is None:
                continue
            if tagged != plan['prepared_sha']:
                raise Blocked('Cancelled package tag differs from frozen source; inspect before a new cut')
            run = self.journal.run(package, plan['prepared_sha'])
            if not run or run['status'] != 'completed':
                raise Blocked('Cancelled batch still has an in-flight Deploy; a new cut cannot replace it')
            receipt = self.pub.receipt(package['name'], package['version'])
            if receipt:
                self.repo.verify_receipt(plan, package, receipt, self.pub)
                package.update(state='published', published_at=receipt.get('published'))
                if not dry_run:
                    self.save(plan)
                continue
            if run['conclusion'] == 'success':
                raise Blocked('Cancelled Deploy succeeded; wait for its exact hosted receipt before a new cut')
            until = package.get('cancel_check_after')
            if until is None:
                until = upload_deadline(self.journal.logs(run['id'], step='Publish'), self.now()) or self.pub.next_check(package['name'])
                package['cancel_check_after'] = until
                if not dry_run:
                    self.save(plan)
            if self.now() < until:
                raise Blocked('Cancelled failed Deploy needs a final hosted/quota reconciliation after its cooldown')
            package['state'] = 'aborted'
            if not dry_run:
                self.save(plan)

    def wait(self, until, reason):
        if until > self.deadline:
            raise Deferred(until, reason)
        remaining = max(0, until - self.now())
        while remaining:
            self.sleep(min(60, remaining))
            remaining = max(0, until - self.now())

    def run(self, operation, *, package='all', bump='patch', dry_run=False, scheduled=False):
        plan = self.load_authorized(dry_run=dry_run)
        if operation == 'cut' and plan and plan['batch_tag'] == 'sz-release-batch-' + self.run_id:
            operation = 'resume'
        if operation == 'cut' and not self.can_cut(plan, dry_run=dry_run):
            return 'blocked'
        if operation == 'cancel':
            return self.cancel(plan, dry_run=dry_run)
        if operation == 'resume':
            early = self.resume_status(plan, dry_run=dry_run, scheduled=scheduled)
            if early:
                return early
        else:
            plan = self.new_plan(plan, package, bump, dry_run=dry_run)
        try:
            return self.execute(plan, operation, dry_run=dry_run)
        except Deferred as error:
            if not dry_run:
                plan.update(status='deferred', error=str(error), not_before=error.until)
                self.save(plan)
            print(f'Batch paused until {error.until}: {error}', flush=True)
            return 'deferred'
        except (Blocked, ValueError) as error:
            if not dry_run:
                plan.update(status='blocked', error=str(error), not_before=0)
                self.save(plan)
            print(f'Batch blocked: {error}', flush=True)
            return 'blocked'


    def load_authorized(self, *, dry_run=False):
        """Recover the immutable manual intent if journal creation was interrupted."""
        plan = self.journal.load()
        if hasattr(self.repo, 'recover_intent'):
            orphan = self.repo.recover_intent(plan)
            if orphan:
                plan = orphan
                if not dry_run:
                    self.journal.create(plan)
        return plan

    def can_cut(self, plan, *, dry_run=False):
        operation = 'cut'
        if operation == 'cut' and plan and plan['status'] not in ('complete', 'cancelled'):
            print('An incomplete batch already exists; resume or cancel that batch.', flush=True)
            return False
        if operation == 'cut' and plan and plan['status'] == 'cancelled':
            try:
                self.reconcile_cancelled(plan, dry_run=dry_run)
            except (Blocked, Deferred) as error:
                print(f'New cut blocked: {error}', flush=True)
                return False
        return True

    def cancel(self, plan, *, dry_run=False):
        if not plan:
            return 'noop'
        if dry_run:
            return 'preview'
        plan.update(status='cancelled', error='Cancelled by manual dispatch')
        self.save(plan)
        return 'cancelled'

    def resume_status(self, plan, *, dry_run=False, scheduled=False):
        if not plan or plan['status'] in ('complete', 'cancelled'):
            return 'noop'
        if scheduled and plan['status'] == 'blocked':
            return 'noop'
        if self.now() < plan['not_before']:
            return 'deferred'
        if dry_run:
            print(json.dumps(plan, indent=2), flush=True)
            return 'preview'
        return None

    def new_plan(self, plan, package, bump, *, dry_run=False):
        """Write manual intent before querying mutable hosted metadata."""
        if plan:
            previous = publication_times({'versions': [{'published': p['published_at']}
                for p in plan['packages'] if p.get('published_at')]})
            self.journal.known_publications = getattr(self.journal, 'known_publications', []) + previous
        sdk = self.repo.installed_sdk()
        expected_sdk = self.repo.env.get('RELEASE_CURRENT_SDK')
        if expected_sdk and json.loads(expected_sdk) != sdk:
            raise Blocked('Current installed SDK differs from the selected official stable release')
        plan = {'schema': 1, 'packaging_sdk': sdk, 'source_sha': self.repo.source_sha(), 'prepared_sha': None,
                'status': 'planning', 'phase': 'planning', 'not_before': 0,
                'error': None, 'package_paths': self.packages, 'packages': [], 'files': {},
                'target': package, 'bump': bump, 'created_at': self.now(),
                'batch_tag': 'sz-release-batch-' + self.run_id}
        if dry_run:
            self.repo.authorize_preview(plan)
        else:
            self.repo.tag(plan['batch_tag'], plan['source_sha'], message=json.dumps(plan))
            self.journal.create(plan)
        return plan

    def plan_candidates(self, plan, operation, *, dry_run=False):
        """Persist metadata progress before source validation or publication."""
        if plan['phase'] == 'planning':
            published = dict(plan.get('planning_metadata', {}))
            refresh_missing = operation == 'resume' and plan['status'] == 'blocked'
            for name in self.packages:
                if name in published and (published[name] is not None or not refresh_missing):
                    continue
                metadata = self.pub.package(name)
                published[name] = None if metadata is None else {
                    'versions': [{'version': item['version']} for item in metadata.get('versions', [])],
                    'latest': metadata.get('latest')}
                plan.setdefault('planning_metadata', {})[name] = published[name]
                if not dry_run:
                    self.save(plan)  # metadata quotas must not restart reads from package one
            candidate = build_plan(self.repo.root, self.packages, plan['target'], plan['bump'],
                                   published, plan['source_sha'])
            for selected in candidate['packages']:
                if published[selected['name']] is None:
                    raise Blocked(f'{selected["name"]} needs its first manual pub.dev publication before automated cuts')
            from release_toolchain import SDK_FILE
            candidate['files'][SDK_FILE] = json.dumps(plan['packaging_sdk'], indent=2) + '\n'
            plan.update(candidate, status='planned', phase='preflight')
            plan.pop('planning_metadata', None)
            print('Release plan:\n' + json.dumps(plan['packages'], indent=2), flush=True)
            if dry_run:
                self.repo.validate(plan, linked=True)
                return 'preview'
            self.save(plan)
        return None

    def prepare_publication(self, plan):
        """Checkpoint reproducible git preparation and the ordinary main push."""
        self.validate_plan(plan)
        if not plan['packages']:
            plan.update(status='complete', phase='complete', error=None)
            self.save(plan)
            return False
        if plan['phase'] == 'preflight':
            self.repo.validate(plan, linked=True)
            plan.update(prepared_sha=self.repo.prepare(plan), phase='push', status='prepared')
            self.save(plan)
        if plan['phase'] == 'push':
            self.repo.prepare(plan)  # recreate git objects on a fresh runner
            self.repo.push_prepared(plan)
            plan.update(phase='publication', status='publishing', error=None, not_before=0)
            self.save(plan)
        self.repo.prepare(plan)
        return True

    def execute(self, plan, operation, *, dry_run=False):
        preview = self.plan_candidates(plan, operation, dry_run=dry_run)
        if preview:
            return preview
        if self.prepare_publication(plan):
            self.publish_packages(plan)
        plan.update(status='complete', phase='complete', error=None, not_before=0)
        self.save(plan)
        return 'complete'

    def publish_packages(self, plan):
        """Reconcile each exact receipt before advancing to its dependents."""
        for current in plan['packages']:
            if current['state'] == 'published':
                continue
            while True:
                # Reconcile a possibly successful upload before every retry.
                receipt = self.pub.receipt(current['name'], current['version'])
                if receipt:
                    self.repo.verify_receipt(plan, current, receipt, self.pub)
                    current.update(state='published', retry_pending=False, published_at=receipt.get('published'))
                    plan.update(status='publishing', error=None, not_before=0)
                    self.save(plan)
                    break
                metadata = self.pub.package(current['name'])
                known = getattr(self.journal, 'known_publications', []) + publication_times(
                    {'versions': [{'published': p['published_at']} for p in plan['packages'] if p.get('published_at')]})
                deadline = quota_deadline(publication_times(metadata), self.now(),
                                          [(3, 120), (6, 3600), (12, 86400)])
                deadline = max(deadline, quota_deadline(known, self.now(),
                                                       [(200, 120), (200, 3600), (200, 86400)]))
                if deadline > self.now():
                    raise Deferred(deadline, f'{current["name"]} known publication quota cooldown')
                if current['state'] == 'pending':
                    for dep, wanted in current['prerequisites'].items():
                        if self.pub.receipt(dep, wanted) is None:
                            raise Deferred(self.pub.next_check(dep), f'Waiting for {dep} {wanted}')
                    self.repo.validate(plan, current, linked=False)
                    self.save(plan)  # exact publish contents checkpoint precedes tag push
                    self.repo.tag(current['tag'], plan['prepared_sha'])
                    current['state'] = 'tagged'
                    self.save(plan)
                run = self.journal.run(current, plan['prepared_sha'])
                if not run:
                    self.wait(self.now() + 120, 'Waiting for tag-triggered Deploy run')
                    continue
                current['run_id'] = run['id']
                if self.pub.receipt(current['name'], current['version']):
                    continue
                if run['status'] != 'completed':
                    self.save(plan)
                    self.wait(self.now() + 30, 'Waiting for Deploy completion')
                    continue
                if run['conclusion'] == 'success':
                    self.save(plan)
                    self.wait(self.pub.next_check(current['name']), 'Waiting for exact version visibility')
                    continue
                if self.retry_if_pending(plan, current, run):
                    continue
                self.handle_failure(plan, current, run)

    def retry_if_pending(self, plan, current, run):
        """An accepted rerun must become visible before inspecting its result."""
        if not current.get('retry_pending'):
            return False
        if run['attempt'] > current['attempts']:
            current['retry_pending'] = False
            current.pop('retry_requested_at', None)
            return False
        if current.get('retry_requested_at') is not None:
            if self.now() - current['retry_requested_at'] > 900:
                raise Blocked('Rerun acceptance is ambiguous after 15 minutes; inspect original Deploy run')
            self.wait(self.now() + 120, 'Waiting for the accepted rerun attempt to become visible')
            return True
        current['retry_requested_at'] = self.now()
        self.save(plan)
        self.journal.retry(run['id'])
        self.save(plan)
        return True

    def handle_failure(self, plan, current, run):
        """Convert server quota windows and bounded transient errors into pauses."""
        current['attempts'] = run['attempt']
        # Only the upload step can report a publishing quota; other steps may
        # print unrelated rate-limit text.
        deadline = upload_deadline(self.journal.logs(run['id'], step='Publish'), self.now())
        if deadline:
            current['retry_pending'] = True
            raise Deferred(deadline, f'Upload quota cooldown for {current["name"]}')
        if re.search(r'HTTP(?: error)?[ :]+(?:408|500|502|503|504)\b|SocketException|Connection reset|Connection timed out', self.journal.logs(run['id']), re.I):
            failures = current.get('transient_failures', 0) + 1
            current['transient_failures'] = failures
            if failures <= 3:
                current['retry_pending'] = True
                raise Deferred(self.now() + 60 * 2 ** (failures - 1) + random.uniform(0, 15),
                               f'Transient upload failure for {current["name"]}; bounded retry {failures}/3')
        raise Blocked(f'Deploy run {run["id"]} {run["conclusion"]}; inspect its validation/authentication error')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--operation', choices=('cut', 'resume', 'cancel'), default='cut')
    parser.add_argument('--package', default='all')
    parser.add_argument('--bump', choices=('patch', 'minor', 'major'), default='patch')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--scheduled', action='store_true')
    args = parser.parse_args()
    from release_github import GitHubClient
    from release_repository import Repository
    from release_services import PubClient
    root = Path(__file__).resolve().parents[1]
    packages = json.loads((root / 'tool/release-packages.json').read_text())
    journal = GitHubClient(os.environ['GITHUB_REPOSITORY'], os.environ['GH_TOKEN'])
    coordinator = Coordinator(Repository(root), journal, PubClient(os.environ.get('PUB_HOSTED_URL', 'https://pub.dev')), packages)
    result = coordinator.run(args.operation, package=args.package, bump=args.bump,
                             dry_run=args.dry_run, scheduled=args.scheduled)
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        plan = journal.load()
        with open(summary, 'a') as file:
            file.write(f'## Batch release: {result}\n\n')
            if plan:
                file.write(f'Intent: `{plan["batch_tag"]}`\n\n')
                for package in plan['packages']:
                    file.write(f'- `{package["name"]} {package["version"]}`: {package["state"]}\n')
                if plan.get('error'):
                    file.write('\n' + plan['error'] + '\n')
                if result == 'deferred':
                    file.write('\nThe authorized batch will resume automatically after its cooldown.\n')
    raise SystemExit(1 if result == 'blocked' else 0)


if __name__ == '__main__':
    main()
