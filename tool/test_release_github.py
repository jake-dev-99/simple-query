import importlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading
import unittest

from test_release_services import Clock


class GitHubTest(unittest.TestCase):
    def setUp(self):
        try:
            self.module = importlib.import_module('release_github')
        except ModuleNotFoundError:
            self.fail('Durable GitHub journal adapter is missing')
        self.clock = Clock()
        self.responses, self.requests = [], []
        test = self
        class Handler(BaseHTTPRequestHandler):
            def respond(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
                test.requests.append((self.command, self.path, json.loads(body) if body else None))
                code, headers, response = test.responses.pop(0)
                self.send_response(code)
                for k, v in headers.items(): self.send_header(k, v)
                self.end_headers()
                if response is not None: self.wfile.write(json.dumps(response).encode())
            do_GET = do_POST = do_PATCH = respond
            def log_message(self, *args): pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.client = self.module.GitHubClient('owner/repo', 'fixture-token',
            base=f'http://127.0.0.1:{self.server.server_port}', now=self.clock.now)

    def test_journal_roundtrip_keeps_frozen_files_without_exceeding_body_limit(self):
        plan = {'schema': 1, 'batch_tag': 'sz-release-batch-12345', 'source_sha': 'a'*40,
                'status': 'planned', 'created_at': self.clock.now(),
                'files': {'CHANGELOG.md': 'Authored release notes\n' * 20000}}
        self.responses = [(201, {}, {'id': 123}), (200, {}, [])]
        self.client.create(plan)
        body = self.requests[0][2]['body']
        self.assertLess(len(body.encode()), 120000)
        self.responses = [(200, {}, [{'id': 123, 'draft': True,
             'tag_name': 'sz-release-batch-12345', 'body': body}])]
        restored = self.client.load()
        self.assertEqual(restored['files'], plan['files'])
        self.assertEqual(restored['journal_id'], 123)

    def test_missing_journal_is_noop_and_other_releases_are_ignored(self):
        self.responses = [(200, {}, [{'id': 1, 'draft': False, 'tag_name': 'demo-v1.0.0', 'body': 'notes'}])]
        self.assertIsNone(self.client.load())

    def test_rate_limit_respects_retry_after_on_mutation_without_retrying_post(self):
        services = importlib.import_module('release_services')
        self.responses = [(429, {'Retry-After': '86400'}, {'message': 'quota'})]
        with self.assertRaises(services.Deferred) as result:
            self.client.create({'schema': 1, 'batch_tag': 'sz-release-batch-12345',
                                'source_sha': 'a'*40, 'files': {}})
        self.assertEqual(result.exception.until, 1700086400)
        self.assertEqual(len(self.requests), 1)

    def test_deploy_lookup_rejects_other_tag_event_and_source_sha(self):
        self.responses = [(200, {}, {'workflow_runs': [
            {'id': 1, 'head_sha': 'a'*40, 'head_branch': 'demo-v1.0.1', 'event': 'workflow_dispatch'},
            {'id': 2, 'head_sha': 'b'*40, 'head_branch': 'demo-v1.0.1', 'event': 'push'},
            {'id': 3, 'head_sha': 'a'*40, 'head_branch': 'other-v1.0.1', 'event': 'push'},
            {'id': 4, 'head_sha': 'a'*40, 'head_branch': 'demo-v1.0.1', 'event': 'push',
             'status': 'completed', 'conclusion': 'failure', 'run_attempt': 2}]} )]
        run = self.client.run({'tag': 'demo-v1.0.1', 'run_id': None}, 'a'*40)
        self.assertEqual(run, {'id': 4, 'status': 'completed', 'conclusion': 'failure', 'attempt': 2})
        self.assertIn('event=push', self.requests[0][1])

    def test_cross_host_log_redirect_does_not_forward_github_token(self):
        seen = []
        class Destination(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.headers.get('Authorization'))
                self.send_response(200); self.end_headers(); self.wfile.write(b'log bytes')
            def log_message(self, *args): pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Destination)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        self.responses = [(302, {'Location':f'http://127.0.0.1:{server.server_port}/logs'}, None)]
        self.assertEqual(self.client.request('GET', '/logs', binary=True), b'log bytes')
        self.assertEqual(seen,[None])

    def test_corrupt_journal_is_preserved_for_attention(self):
        services = importlib.import_module('release_services')
        self.responses = [(200, {}, [{'id':1,'draft':True,'tag_name':'sz-release-batch-12345','body':'{}'}])]
        with self.assertRaisesRegex(services.Blocked,'Invalid batch journal'):
            self.client.load()
        self.assertEqual(len(self.requests),1)

    def test_history_scan_is_bounded_before_using_incomplete_quota_accounting(self):
        from release_services import Blocked
        page = [{'id': n, 'tag_name': 'demo-v1.0.0', 'draft': False} for n in range(100)]
        self.responses = [(200, {}, page)] * 10
        with self.assertRaisesRegex(Blocked, 'exceeds 1000'):
            self.client.load()
        self.assertEqual(len(self.requests), 10)

    def test_history_retains_only_prior_publications_in_the_daily_window(self):
        from datetime import datetime, timezone
        def body(when):
            return self.module.encode({'schema': 1, 'files': {}, 'packages': [
                {'published_at': datetime.fromtimestamp(t, timezone.utc).isoformat()} for t in when]})
        self.responses = [(200, {}, [
            {'id': 2, 'draft': True, 'tag_name': 'sz-release-batch-2', 'body': body([])},
            {'id': 1, 'draft': True, 'tag_name': 'sz-release-batch-1',
             'body': body([self.clock.now()-10, self.clock.now()-86401])}])]
        self.assertEqual(self.client.load()['journal_id'], 2)
        self.assertEqual(self.client.known_publications, [self.clock.now()-10])

    def test_retry_preserves_original_tag_event_and_only_reruns_failed_jobs(self):
        self.responses = [(201, {}, None)]
        self.client.retry(4)
        self.assertEqual(self.requests, [('POST', '/repos/owner/repo/actions/runs/4/rerun-failed-jobs', {})])


if __name__ == '__main__': unittest.main()
