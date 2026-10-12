"""Release boundaries: hosted HTTP, quotas, git snapshots, and GitHub journal."""
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
import json
import random
import re
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


class Blocked(RuntimeError):
    """An error requiring attention; never automatically retry publication."""


class Deferred(RuntimeError):
    def __init__(self, until, reason):
        super().__init__(reason)
        self.until = until


def retry_deadline(value, now):
    if value is None:
        return None
    try:
        seconds = int(value)
        return now + max(0, seconds)
    except ValueError:
        try:
            parsed = parsedate_to_datetime(value)
            if parsed.tzinfo is None:
                parsed = parsed.replace(tzinfo=timezone.utc)
            return max(now, parsed.timestamp())
        except (ValueError, TypeError, OverflowError):
            return None


def upload_deadline(message, now):
    lowered = message.lower()
    if 'rate limit' not in lowered and 'ratelimit' not in lowered and 'too many requests' not in lowered:
        return None
    for window, seconds in [('last day', 86400), ('last hour', 3600), ('last few minutes', 120),
                            ('last 2 minutes', 120), ('last two minutes', 120), ('last 120 seconds', 120)]:
        if window in lowered:
            return now + seconds
    raise Blocked('Unknown upload quota window: pause for attention; no speculative retry')


def quota_deadline(timestamps, now, limits):
    deadline = 0
    for count, window in limits:
        recent = sorted(value for value in timestamps if now - window < value <= now)
        if len(recent) >= count:
            # More than the quota may be known after concurrent external work.
            deadline = max(deadline, recent[len(recent) - count] + window)
    return deadline


def publication_times(metadata):
    return [datetime.fromisoformat(item['published'].replace('Z', '+00:00')).timestamp()
            for item in metadata.get('versions', []) if item.get('published')]


class PubClient:
    def __init__(self, base='https://pub.dev', *, now=time.time, sleep=time.sleep,
                 jitter=lambda: random.uniform(0, 1)):
        self.base, self.now, self.sleep, self.jitter = base.rstrip('/'), now, sleep, jitter
        self.cache = {}

    def get(self, url, *, binary=False):
        cached = self.cache.get(url)
        if cached and self.now() < cached[0]:
            return cached[1]
        for attempt in range(3):
            try:
                request = Request(url, headers={'User-Agent': 'simple-zen-release-coordinator/1'})
                with urlopen(request, timeout=30) as response:
                    raw = response.read()
                    result = raw if binary else json.loads(raw)
                    ttl = re.search(r'(?:^|[, ])max-age=(\d+)', response.headers.get('Cache-Control', ''))
                    self.cache[url] = (self.now() + (int(ttl[1]) if ttl else 120), result)
                    return result
            except HTTPError as error:
                error.close()
                deadline = retry_deadline(error.headers.get('Retry-After'), self.now())
                if error.code == 429:
                    if deadline is None:
                        raise Blocked('Hosted API quota response has no usable Retry-After') from error
                    raise Deferred(deadline, 'Hosted API quota cooldown') from error
                if error.code == 404:
                    self.cache[url] = (self.now() + 120, None)
                    return None
                if error.code not in (408, 500, 502, 503, 504):
                    raise Blocked(f'Hosted API HTTP {error.code}: {url}') from error
                if deadline is not None:
                    raise Deferred(deadline, f'Hosted API HTTP {error.code} cooldown') from error
                if attempt == 2:
                    raise Blocked(f'Hosted API HTTP {error.code} after bounded retries') from error
            except (URLError, TimeoutError) as error:
                if attempt == 2:
                    raise Blocked('Hosted API unavailable after bounded retries') from error
            self.sleep(2 ** (attempt + 1) + self.jitter())
        raise AssertionError('Unreachable HTTP retry state')

    def package(self, name):
        return self.get(self.base + '/api/packages/' + quote(name, safe=''))

    def next_check(self, name):
        cached = self.cache.get(self.base + '/api/packages/' + quote(name, safe=''))
        return max(self.now() + 30, cached[0] if cached else self.now() + 120)

    def receipt(self, name, wanted):
        metadata = self.package(name)
        return next((item for item in (metadata or {}).get('versions', [])
                     if item['version'] == wanted), None)
