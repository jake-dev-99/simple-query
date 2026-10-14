"""Draft-release journal and original tag-triggered Deploy run operations."""
import base64
import io
import json
import os
import re
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlparse
from urllib.request import HTTPRedirectHandler, Request, build_opener
import zipfile
import zlib

from release_services import Blocked, Deferred, publication_times, retry_deadline


class SafeRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        old, new = urlparse(req.full_url), urlparse(newurl)
        if old.scheme == 'https' and new.scheme != 'https':
            raise Blocked('Refusing an insecure GitHub redirect')
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if old.netloc != new.netloc:
            redirected.remove_header('Authorization')
        return redirected


def encode(plan):
    data = dict(plan)
    files = json.dumps(data.pop('files', {})).encode()
    data['files_zlib'] = base64.b64encode(zlib.compress(files, 9)).decode()
    body = json.dumps(data, separators=(',', ':'))
    if len(body.encode()) > 120000:
        raise Blocked('Frozen release journal exceeds GitHub body capacity')
    return body


def decode(body):
    try:
        plan = json.loads(body)
        decoder = zlib.decompressobj()
        files = decoder.decompress(base64.b64decode(plan.pop('files_zlib')), 5_000_001)
        if len(files) > 5_000_000 or not decoder.eof:
            raise ValueError('Oversize or incomplete frozen file data')
        plan['files'] = json.loads(files)
        if plan.get('schema') != 1:
            raise ValueError('Unknown schema')
        return plan
    except (ValueError, KeyError, TypeError, zlib.error) as error:
        raise Blocked('Invalid batch journal; preserve it for inspection') from error


class GitHubClient:
    def __init__(self, repository, token, *, base=None, now=time.time):
        self.repository, self.token, self.now = repository, token, now
        self.base = (base or os.environ.get('GITHUB_API_URL', 'https://api.github.com')).rstrip('/')
        self.path = '/repos/' + repository
        self.opener = build_opener(SafeRedirect())

    def request(self, method, path, payload=None, *, binary=False):
        data = json.dumps(payload).encode() if payload is not None else None
        request = Request(self.base + path, data=data, method=method,
                          headers={'Authorization': 'Bearer ' + self.token,
                                   'Accept': 'application/vnd.github+json',
                                   'Content-Type': 'application/json',
                                   'X-GitHub-Api-Version': '2022-11-28',
                                   'User-Agent': 'simple-zen-release-coordinator'})
        try:
            with self.opener.open(request, timeout=30) as response:
                raw = response.read()
                return raw if binary else json.loads(raw) if raw else None
        except HTTPError as error:
            error.close()
            deadline = retry_deadline(error.headers.get('Retry-After'), self.now())
            if error.code in (429, 403) and (deadline is not None or error.headers.get('X-RateLimit-Remaining') == '0'):
                if deadline is None:
                    try:
                        deadline = float(error.headers['X-RateLimit-Reset'])
                    except (ValueError, TypeError, KeyError):
                        raise Blocked('GitHub quota has no retry deadline') from error
                raise Deferred(max(self.now(), deadline), 'GitHub API cooldown') from error
            if error.code in (408, 500, 502, 503, 504):
                # Mutations can have succeeded despite an interrupted response.
                # Resume reconciles remote state instead of replaying the POST.
                raise Deferred(deadline or self.now() + 120, f'GitHub HTTP {error.code}; reconcile on resume') from error
            raise Blocked(f'GitHub {method} {path} HTTP {error.code}') from error
        except (URLError, TimeoutError) as error:
            raise Deferred(self.now() + 120, 'GitHub request interrupted; reconcile on resume') from error

    def remember_publications(self, plan):
        recent = publication_times({'versions': [
            {'published': package['published_at']} for package in plan.get('packages', [])
            if package.get('published_at')]})
        self.known_publications.extend(t for t in recent if self.now() - 86400 < t <= self.now())

    def load(self):
        # Keep only one full journal and rolling-window timestamps, never all
        # historical frozen files. An incomplete bounded scan requires attention.
        latest = None
        self.known_publications = []
        for page in range(1, 11):
            releases = self.request('GET', self.path + f'/releases?per_page=100&page={page}')
            for release in releases:
                # GitHub can assign draft releases an untagged-* tag name.
                # The internal title and frozen body retain the batch identity.
                identity = release['tag_name']
                if not identity.startswith('sz-release-batch-'):
                    title = release.get('name') or ''
                    if not title.startswith('Internal batch '):
                        continue
                    identity = title.removeprefix('Internal batch ')
                if not identity.startswith('sz-release-batch-'):
                    continue
                plan = decode(release.get('body') or '')
                if (not re.fullmatch(r'sz-release-batch-\d+', identity)
                        or plan.get('batch_tag') != identity):
                    raise Blocked('Batch journal identity does not match its frozen plan')
                plan['journal_id'] = release['id']
                if not release['draft']:
                    plan.update(status='blocked', error='Batch journal is no longer an internal draft release')
                if latest is None or plan['journal_id'] > latest['journal_id']:
                    if latest:
                        self.remember_publications(latest)
                    latest = plan
                else:
                    self.remember_publications(plan)
            if len(releases) < 100:
                return latest
        raise Blocked('Release history exceeds 1000 records; inspect/archive obsolete journals before resuming')

    def create(self, plan):
        release = self.request('POST', self.path + '/releases',
                               {'tag_name': plan['batch_tag'], 'target_commitish': plan['source_sha'],
                                'name': 'Internal batch ' + plan['batch_tag'],
                                'draft': True, 'prerelease': True, 'body': encode(plan)})
        plan['journal_id'] = release['id']

    def save(self, plan):
        self.request('PATCH', self.path + f'/releases/{plan["journal_id"]}',
                     {'body': encode(plan), 'draft': True})

    def run(self, package, sha):
        # The API was checked against actual tag-push runs in simple-query.
        query = urlencode({'event': 'push', 'branch': package['tag'], 'head_sha': sha, 'per_page': 100})
        runs = self.request('GET', self.path + '/actions/workflows/deploy.yml/runs?' + query)['workflow_runs']
        matching = [run for run in runs if run['event'] == 'push' and run['head_sha'] == sha
                    and run['head_branch'] == package['tag']]
        if not matching:
            return None
        run = max(matching, key=lambda value: value['id'])
        return {'id': run['id'], 'status': run['status'], 'conclusion': run['conclusion'],
                'attempt': run['run_attempt']}

    def logs(self, run_id, step=None):
        """Return the run's logs, or only the named step's when `step` is given."""
        raw = self.request('GET', self.path + f'/actions/runs/{run_id}/logs', binary=True)
        in_step = re.compile(rf'/\d+_{re.escape(step)}\.txt$') if step else None
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            if sum(file.file_size for file in archive.infolist()) > 50_000_000:
                raise Blocked('Deploy logs exceed bounded inspection size')
            return '\n'.join(archive.read(file).decode(errors='replace')
                             for file in archive.namelist()
                             if file.endswith('.txt') and (in_step is None or in_step.search(file)))

    def retry(self, run_id):
        self.request('POST', self.path + f'/actions/runs/{run_id}/rerun-failed-jobs', {})
