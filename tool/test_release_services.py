"""Hosted-client behavior is exercised against a local HTTP server."""
import importlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading
import unittest


class Clock:
    def __init__(self):
        self.value = 1700000000.0
        self.sleeps = []

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.value += seconds


class HttpTest(unittest.TestCase):
    def setUp(self):
        try:
            self.module = importlib.import_module('release_services')
        except ModuleNotFoundError:
            self.fail('Hosted-client quota handling is missing')
        self.clock = Clock()
        self.responses = []
        self.requests = []
        test = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                test.requests.append(self.path)
                status, headers, body = test.responses.pop(0)
                self.send_response(status)
                for key, value in headers.items():
                    self.send_header(key, value)
                self.end_headers()
                self.wfile.write(json.dumps(body).encode())

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.client = self.module.PubClient(
            f'http://127.0.0.1:{self.server.server_port}',
            now=self.clock.now, sleep=self.clock.sleep, jitter=lambda: 0)

    def test_cached_metadata_is_not_polled_until_cache_header_expires(self):
        self.responses = [(200, {'Cache-Control': 'public, max-age=120'}, {'versions': []}),
                          (200, {}, {'versions': [{'version': '1.0.1'}]})]
        self.assertEqual(self.client.package('demo')['versions'], [])
        self.clock.value += 119
        self.assertEqual(self.client.package('demo')['versions'], [])
        self.assertEqual(self.requests, ['/api/packages/demo'])
        self.clock.value += 2
        self.assertEqual(self.client.package('demo')['versions'][0]['version'], '1.0.1')
        self.assertEqual(len(self.requests), 2)

    def test_429_numeric_retry_after_defers_without_another_request(self):
        self.responses = [(429, {'Retry-After': '3600'}, {'error': {'code': 'RateLimit'}})]
        with self.assertRaises(self.module.Deferred) as result:
            self.client.package('demo')
        self.assertEqual(result.exception.until, 1700003600.0)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.clock.sleeps, [])

    def test_429_http_date_retry_after_is_respected(self):
        self.responses = [(429, {'Retry-After': 'Tue, 14 Nov 2023 23:13:20 GMT'}, {})]
        with self.assertRaises(self.module.Deferred) as result:
            self.client.package('demo')
        self.assertEqual(result.exception.until, 1700003600.0)

    def test_429_without_reset_time_pauses_a_full_day_without_another_request(self):
        self.responses = [(429, {}, {})]
        with self.assertRaises(self.module.Deferred) as result:
            self.client.package('demo')
        self.assertEqual(result.exception.until, self.clock.now()+86400)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(len(self.requests), 1)

    def test_transient_503_uses_backoff_then_returns_actual_metadata(self):
        self.responses = [(503, {}, {}), (503, {}, {}),
                          (200, {}, {'versions': [{'version': '1.0.1'}]})]
        self.assertEqual(self.client.package('demo')['versions'][0]['version'], '1.0.1')
        self.assertEqual(self.clock.sleeps, [2, 4])

    def test_transient_retries_are_bounded(self):
        self.responses = [(503, {}, {})] * 3
        with self.assertRaisesRegex(self.module.Blocked, '503'):
            self.client.package('demo')
        self.assertEqual(len(self.requests), 3)

    def test_503_retry_after_is_deferred_instead_of_retried_early(self):
        self.responses = [(503, {'Retry-After': '300'}, {})]
        with self.assertRaises(self.module.Deferred) as result:
            self.client.package('demo')
        self.assertEqual(result.exception.until, 1700000300.0)
        self.assertEqual(len(self.requests), 1)

    def test_authentication_error_is_blocked_without_retry(self):
        self.responses = [(403, {}, {'error': {'code': 'AuthenticationFailed'}})]
        with self.assertRaisesRegex(self.module.Blocked, '403'):
            self.client.package('demo')
        self.assertEqual(len(self.requests), 1)

    def test_missing_package_is_distinct_from_empty_version_list(self):
        self.responses = [(404, {'Cache-Control': 'max-age=60'}, {})]
        self.assertIsNone(self.client.package('new_package'))
        self.assertIsNone(self.client.package('new_package'))
        self.assertEqual(len(self.requests), 1)

    def test_cli_quota_messages_defer_for_the_full_reported_window(self):
        for text, delay in [('last few minutes', 120), ('last hour', 3600), ('last day', 86400)]:
            with self.subTest(text=text):
                message = ('The "package-published" operation is blocked, as its rate limit '
                           f'has been reached (12 in the {text}). Please try again later.')
                self.assertEqual(self.module.upload_deadline(message, self.clock.now()),
                                 1700000000.0 + delay)

    def test_numeric_and_spelled_two_minute_quota_messages_pause_120_seconds(self):
        for window in ['last 2 minutes','last two minutes','last 120 seconds']:
            self.assertEqual(self.module.upload_deadline('rate limit has been reached (3 in the '+window+')',
                                                       self.clock.now()),1700000120)

    def test_unrecognized_upload_quota_message_uses_conservative_daily_pause(self):
        self.assertEqual(self.module.upload_deadline('rate limit has been reached (unknown window)', self.clock.now()), self.clock.now()+86400)
        self.assertIsNone(self.module.upload_deadline('Analyzer error: missing import', self.clock.now()))

    def test_known_package_publications_defer_until_rolling_window_clears(self):
        self.assertEqual(self.module.quota_deadline([1699999980, 1699999990, 1699999995],
                                                   self.clock.now(), [(3, 120)]), 1700000100)
        self.assertEqual(self.module.quota_deadline([1699990000] * 12,
                                                   self.clock.now(), [(12, 86400)]), 1700076400)
        self.assertEqual(self.module.quota_deadline([1699990000] * 2,
                                                   self.clock.now(), [(12, 86400)]), 0)


if __name__ == '__main__':
    unittest.main()
