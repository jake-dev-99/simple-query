"""Installed Flutter publishing against a local quota server; no live uploads."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest

from release_services import upload_deadline


@unittest.skipUnless(os.environ.get('RELEASE_SDK_SMOKE') == 'true', 'SDK integration job only')
class SDKQuotaTest(unittest.TestCase):
    def exercise(self, window, seconds):
        requests = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                self.send_response(429 if self.path == '/api/packages/versions/new' else 200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Retry-After', str(seconds))
                self.end_headers()
                payload = {'error': {'code': 'RateLimit', 'message':
                    f'Publishing rate limit has been reached (12 in the {window}).'}} if self.path == '/api/packages/versions/new' else {
                    'name': 'sdk_quota_fixture', 'latest': {'version': '0.9.0', 'pubspec': {'name': 'sdk_quota_fixture', 'version': '0.9.0'}},
                    'versions': [{'version': '0.9.0', 'pubspec': {'name': 'sdk_quota_fixture', 'version': '0.9.0'}}]}
                self.wfile.write(json.dumps(payload).encode())
            def log_message(self, *args): pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        with tempfile.TemporaryDirectory(prefix='sdk-quota-') as temp:
            root = Path(temp)
            (root/'lib').mkdir()
            (root/'lib/sdk_quota_fixture.dart').write_text('const value = 1;\n')
            # publish_to points only the upload at the fixture; the SDK's own
            # dependency resolution keeps using the real hosted URL.
            (root/'pubspec.yaml').write_text(f'name: sdk_quota_fixture\nversion: 1.0.0\ndescription: Local publishing quota recovery verification fixture.\nhomepage: https://example.invalid/quota\npublish_to: http://127.0.0.1:{server.server_port}\nenvironment:\n  sdk: ^3.6.0\n')
            for file, content in [('README.md', '# SDK quota fixture\n'), ('CHANGELOG.md', '## 1.0.0\nInitial fixture.\n'), ('LICENSE', 'MIT License\nCopyright 2026 Fixture\n')]:
                (root/file).write_text(content)
            env = {**os.environ, 'PUB_MAX_HTTP_RETRIES': '1', 'CI': 'true'}
            result = subprocess.run([shutil.which('flutter') or 'flutter', 'pub', 'publish', '--force'], cwd=root, env=env, text=True, capture_output=True, timeout=90)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertEqual(requests.count('/api/packages/versions/new'), 1, output)
        self.assertEqual(upload_deadline(output, 1700000000), 1700000000+seconds, output)

    def test_burst_quota(self): self.exercise('last 2 minutes', 120)
    def test_hourly_quota(self): self.exercise('last hour', 3600)
    def test_daily_quota(self): self.exercise('last day', 86400)

    def test_unknown_quota(self): self.exercise('unknown window', 86400)
