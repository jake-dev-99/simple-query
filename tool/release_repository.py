"""Immutable git preparation and Flutter validation for a release batch."""
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
from urllib.parse import urlparse

import yaml

from release_services import Blocked


def fingerprint(files):
    digest = hashlib.sha256()
    for name, data in sorted(files.items()):
        digest.update(name.encode() + b'\0' + hashlib.sha256(data).digest())
    return digest.hexdigest()


def archive_fingerprint(raw):
    files = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        for entry in archive:
            if entry.isdir():
                continue
            if not entry.isfile() or entry.name in files:
                raise Blocked('Published archive contains links or duplicate files')
            files[entry.name] = archive.extractfile(entry).read()
    return fingerprint(files)


class Repository:
    def __init__(self, root, *, flutter='flutter', env=None, validation_timeout=1200):
        self.root = Path(root).resolve()
        self.flutter = flutter
        self.validation_timeout = validation_timeout
        self.env = {**os.environ, **(env or {}), 'PUB_MAX_HTTP_RETRIES': '1'}

    def git(self, *args, input=None, env=None, check=True):
        result = subprocess.run(['git', *args], cwd=self.root,
                                input=input, capture_output=True,
                                env={**self.env, **(env or {})})
        if check and result.returncode:
            raise Blocked(f'Git {args[0]} failed: {result.stderr.decode(errors="replace").strip()}')
        return result.stdout.decode().strip() if check else result

    def source_sha(self):
        self.git('fetch', 'origin', 'main', '--tags')
        return self.git('rev-parse', 'origin/main')

    def recover_intent(self, existing=None):
        # The immutable annotated tag survives a stopped runner between tag and
        # journal creation. Never resurrect an older completed/cancelled batch.
        self.git('fetch', 'origin', 'main', '--tags')
        names = self.git('tag', '--list', 'sz-release-batch-*').splitlines()
        cutoff = int(existing['batch_tag'].rsplit('-', 1)[1]) if existing else -1
        candidates = [name for name in names if name.rsplit('-', 1)[1].isdigit()
                      and int(name.rsplit('-', 1)[1]) > cutoff]
        if not candidates:
            return None
        tag = max(candidates, key=lambda name: int(name.rsplit('-', 1)[1]))
        try:
            plan = json.loads(self.git('for-each-ref', '--format=%(contents)', 'refs/tags/' + tag))
        except (ValueError, TypeError) as error:
            raise Blocked('Orphan batch intent has invalid metadata; preserve for inspection') from error
        if (plan.get('schema') != 1 or plan.get('batch_tag') != tag
                or self.git('rev-parse', tag + '^{commit}') != plan.get('source_sha')
                or self.git('merge-base', '--is-ancestor', plan['source_sha'], 'origin/main', check=False).returncode):
            raise Blocked('Orphan batch intent does not match main source')
        return plan

    def prepare(self, plan):
        # Fixed author/date, parent and tree make the prepared SHA reproducible.
        with tempfile.TemporaryDirectory() as temp:
            index = str(Path(temp) / 'index')
            env = {'GIT_INDEX_FILE': index}
            self.git('read-tree', plan['source_sha'], env=env)
            for file, contents in plan['files'].items():
                blob = self.git('hash-object', '-w', '--stdin', input=contents.encode())
                self.git('update-index', '--add', '--cacheinfo', f'100644,{blob},{file}', env=env)
            tree = self.git('write-tree', env=env)
        timestamp = datetime.fromtimestamp(plan['created_at'], timezone.utc).isoformat()
        identity = {'GIT_AUTHOR_NAME': 'github-actions[bot]',
                    'GIT_COMMITTER_NAME': 'github-actions[bot]',
                    'GIT_AUTHOR_EMAIL': '41898282+github-actions[bot]@users.noreply.github.com',
                    'GIT_COMMITTER_EMAIL': '41898282+github-actions[bot]@users.noreply.github.com',
                    'GIT_AUTHOR_DATE': timestamp, 'GIT_COMMITTER_DATE': timestamp}
        refs = sorted(set(__import__('re').findall(r'\b[A-Z]+-\d+\b', '\n'.join(plan['files'].values()))))
        message = f'chore(release): prepare {plan["batch_tag"]}' + (f' ({", ".join(refs)})' if refs else '') + '\n'
        sha = self.git('commit-tree', tree, '-p', plan['source_sha'], input=message.encode(), env=identity)
        if plan.get('prepared_sha') and plan['prepared_sha'] != sha:
            raise Blocked('Prepared release SHA differs from frozen journal')
        return sha

    def push_prepared(self, plan):
        self.git('fetch', 'origin', 'main')
        actual = self.git('rev-parse', 'origin/main')
        if self.git('merge-base', '--is-ancestor', plan['prepared_sha'], actual, check=False).returncode == 0:
            return
        if actual != plan['source_sha']:
            raise Blocked('main advanced before the prepared release push; review a new source plan')
        self.git('push', 'origin', f'{plan["prepared_sha"]}:refs/heads/main')

    def remote_tag_sha(self, name):
        remote = self.git('ls-remote', 'origin', f'refs/tags/{name}', f'refs/tags/{name}^{{}}')
        if not remote:
            return None
        refs = dict(line.split()[::-1] for line in remote.splitlines())
        return refs.get(f'refs/tags/{name}^{{}}', refs[f'refs/tags/{name}'])

    def tag(self, name, sha, *, message=None):
        self.git('fetch', 'origin', 'main', '--tags')
        if self.git('merge-base', '--is-ancestor', sha, 'origin/main', check=False).returncode:
            raise Blocked('Release tag SHA is not on main')
        remote = self.git('ls-remote', 'origin', f'refs/tags/{name}', f'refs/tags/{name}^{{}}')
        if remote:
            # Annotated refs have both the tag-object SHA and peeled commit SHA.
            refs = dict(line.split()[::-1] for line in remote.splitlines())
            actual = refs.get(f'refs/tags/{name}^{{}}', refs[f'refs/tags/{name}'])
            if actual != sha:
                raise Blocked(f'Existing tag {name} points to a different source SHA')
            return
        local = self.git('rev-parse', '--verify', f'refs/tags/{name}^{{commit}}', check=False)
        if local.returncode == 0 and local.stdout.decode().strip() != sha:
            raise Blocked(f'Local tag {name} points to a different source SHA')
        if local.returncode:
            self.git('tag', '-a', name, sha, '-m', message or f'Release intent {name}')
        self.git('push', 'origin', f'refs/tags/{name}')

    @contextmanager
    def snapshot(self, plan):
        raw = subprocess.check_output(['git', 'archive', plan['source_sha']], cwd=self.root)
        with tempfile.TemporaryDirectory(prefix='release-check-') as temp:
            root = Path(temp)
            with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
                archive.extractall(root, filter='data')
            for file, contents in plan['files'].items():
                destination = root / file
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(contents)
            # Tracked development overrides are removed, as Deploy does, so every
            # dependency must resolve from hosted packages; manifest
            # dependency_overrides and local/git sources are rejected by
            # validate_sources.
            for file in root.rglob('pubspec_overrides.yaml'):
                file.unlink()
            # Pub inherits ignore rules from the git repository root. Retain
            # that boundary for nested packages, with a clean disposable index.
            subprocess.run(['git', 'init', '-q', str(root)], check=True, capture_output=True)
            subprocess.run(['git', '-C', str(root), 'add', '--force', '--all'], check=True, capture_output=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Release validation',
                            '-c', 'user.email=release-validation@example.invalid',
                            'commit', '-q', '-m', 'Frozen release validation snapshot'],
                           check=True, capture_output=True)
            yield root

    def validate_sources(self, root, directory, plan):
        """Permit hosted dependencies, SDK packages, and the example's own batch links."""
        manifests = [directory / 'pubspec.yaml', *(directory / 'example').rglob('pubspec.yaml')]
        selected = {p['name']: (root / p['path']).resolve() for p in plan['packages']}
        for file in manifests:
            spec = yaml.safe_load(file.read_text()) or {}
            if 'dependency_overrides' in spec:
                raise Blocked(f'{file.relative_to(root)} contains dependency_overrides; remove development overrides before releasing')
            for section in ('dependencies', 'dev_dependencies'):
                for name, source in (spec.get(section) or {}).items():
                    if isinstance(source, str):
                        continue
                    if not isinstance(source, dict):
                        raise Blocked(f'{name} has an unsupported dependency source')
                    if 'path' in source:
                        if not isinstance(source['path'], str):
                            raise Blocked(f'{name} path dependency must be a string')
                        expected = selected.get(name)
                        if (file != directory / 'pubspec.yaml' and expected is not None
                                and set(source) == {'path'}
                                and (file.parent / source['path']).resolve() == expected):
                            continue
                        raise Blocked(f'{name} local path dependency is outside intentional selected-batch example links')
                    if 'sdk' in source and set(source) == {'sdk'} and source['sdk'] in ('flutter', 'dart'):
                        continue
                    if set(source).issubset({'version', 'hosted'}):
                        hosted = source.get('hosted', 'https://pub.dev')
                        if isinstance(hosted, dict):
                            hosted = hosted.get('url')
                        if hosted in ('https://pub.dev', 'https://pub.dev/', 'https://pub.dartlang.org'):
                            continue
                    raise Blocked(f'{name} external dependency must resolve from pub.dev or a supported SDK')

    def validate(self, plan, package=None, *, linked=False):
        packages = [package] if package else plan['packages']
        with self.snapshot(plan) as root:
            for current in packages:
                self.validate_sources(root, root / current['path'], plan)
            if linked:
                for current in packages:
                    links = {p['name']: {'path': str(root / p['path'])}
                             for p in plan['packages'] if p['name'] != current['name']}
                    if links:
                        (root / current['path'] / 'pubspec_overrides.yaml').write_text(
                            yaml.safe_dump({'dependency_overrides': links}))
            for current in packages:
                directory = root / current['path']
                example = directory / 'example'
                if linked and (example / 'pubspec.yaml').exists():
                    links = {p['name']: {'path': str(root / p['path'])} for p in plan['packages']}
                    (example / 'pubspec_overrides.yaml').write_text(yaml.safe_dump({'dependency_overrides': links}))
                commands = [(directory, ['pub', 'get', '--no-example'])]
                if (example / 'pubspec.yaml').exists():
                    commands.append((example, ['pub', 'get']))
                commands.append((directory, ['analyze', '--no-fatal-warnings']))
                if (directory / 'test').is_dir():
                    commands.append((directory, ['test']))
                # Packaging problems (license, changelog, size, pubspec fields)
                # do not depend on hosted dependencies: find them before the
                # release commit lands on main.
                commands.append((directory, ['pub', 'publish', '--dry-run']))
                for command_dir, command in commands:
                    print(f'Validate {current["name"]} {current["version"]} ({"source graph" if linked else "hosted"}): {" ".join(command)}', flush=True)
                    self.run_checked_flutter(command, command_dir, current['name'])
                if not linked:
                    current['artifact_fingerprint'] = self.frozen_archive_fingerprint(plan, current)

    # Purpose: Stop validation and packaging on an SDK failure or quota response.
    # /**
    #  * @param self The repository and its configured stable Flutter process.
    #  * @param command The Flutter command arguments.
    #  * @param cwd The isolated working directory for this command.
    #  * @param name The package identifier used in diagnostic messages.
    #  * @returns None after a successful SDK command.
    #  * @throws Blocked on validation failure, or Deferred on a quota cooldown.
    #  */
    def run_checked_flutter(self, command, cwd, name):
        result = self.run_flutter(command, cwd, name)
        if result.returncode:
            output = result.stdout + result.stderr
            from release_services import Deferred, upload_deadline
            deadline = upload_deadline(output, __import__('time').time())
            if deadline:
                raise Deferred(deadline, f'Validation quota for {name}')
            raise Blocked(f'{name} validation failed:\n{output[-8000:]}')

    # Purpose: Derive complete SDK archive evidence from untouched frozen source.
    # /**
    #  * @param self The repository with the configured stable Flutter SDK.
    #  * @param plan The immutable source SHA and frozen release manifest files.
    #  * @param package The selected package name, path, and release version.
    #  * @returns The fingerprint of every file packaged by the SDK.
    #  * @throws Blocked on invalid sources or SDK failure; Deferred on quotas.
    #  */
    def frozen_archive_fingerprint(self, plan, package):
        # Example resolution and tests may generate files in their own sandbox.
        # Package a separate clean snapshot using the same preparation as Deploy.
        with self.snapshot(plan) as root, tempfile.TemporaryDirectory(prefix='release-artifact-') as temp:
            directory = root / package['path']
            self.validate_sources(root, directory, plan)
            artifact = Path(temp) / (package['name'] + '.tar.gz')
            for command in [['pub', 'get', '--no-example'],
                            ['pub', 'publish', '--to-archive', str(artifact)]]:
                print(f'Archive {package["name"]} {package["version"]} (frozen source): '
                      f'{" ".join(command)}', flush=True)
                self.run_checked_flutter(command, directory, package['name'])
            return archive_fingerprint(artifact.read_bytes())

    def run_flutter(self, command, cwd, name):
        """Run one validation command; a hung SDK blocks the batch for attention."""
        try:
            return subprocess.run([self.flutter, *command], cwd=cwd, env=self.env, text=True,
                                  capture_output=True, timeout=self.validation_timeout)
        except subprocess.TimeoutExpired as error:
            raise Blocked(f'{name} validation timed out after {self.validation_timeout}s: '
                          f'{" ".join(command)}') from error

    def verify_receipt(self, plan, package, receipt, pub):
        url = receipt.get('archive_url', '')
        parsed = urlparse(url)
        if parsed.scheme != 'https' or parsed.hostname not in ('pub.dev', 'storage.googleapis.com'):
            raise Blocked('Published artifact has an unexpected archive URL')
        raw = pub.get(url, binary=True)
        if raw is None or hashlib.sha256(raw).hexdigest() != receipt.get('archive_sha256'):
            raise Blocked('Published artifact archive checksum is missing or different')
        if not package.get('artifact_fingerprint'):
            self.validate(plan, package, linked=False)
        expected = self.frozen_archive_fingerprint(plan, package)
        if archive_fingerprint(raw) != expected:
            raise Blocked(f'Published artifact for {package["name"]} does not match frozen source')
        # This is derived evidence, never the authority for frozen source identity.
        package['artifact_fingerprint'] = expected
