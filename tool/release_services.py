"""Release boundaries: hosted HTTP, quotas, git snapshots, and GitHub journal."""
from dataclasses import dataclass
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
import json
import random
import re
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


class Blocked(RuntimeError):
    """An error requiring attention; never automatically retry publication."""


SDK_IDENTITY_FIELDS = frozenset({'version', 'revision', 'channel'})
SDK_VERSION_PATTERN = r'\d+\.\d+\.\d+'
SDK_REVISION_PATTERN = r'[0-9a-f]{40}'


# Purpose: Reject incomplete or moving SDK descriptions before recovery can authorize them (UNFY-402).
# /**
#  * @param value The candidate SDK identity from immutable source or workflow metadata.
#  * @returns Whether all fields identify one exact stable framework release.
#  * @throws Nothing; malformed values fail the predicate.
#  */
def is_exact_stable_sdk(value):
    return (isinstance(value, dict) and set(value) == SDK_IDENTITY_FIELDS
            and value['channel'] == 'stable'
            and re.fullmatch(SDK_VERSION_PATTERN, str(value['version'])) is not None
            and re.fullmatch(SDK_REVISION_PATTERN, str(value['revision'])) is not None)


# Purpose: Keep only exact released SDK identities in source and workflow metadata.
# /**
#  * @param value The declared Flutter version, framework revision and channel.
#  * @returns The validated stable identity without accepting version ranges.
#  * @throws Blocked for incomplete, experimental or malformed identities.
#  */
def validate_sdk(value):
    if not is_exact_stable_sdk(value):
        raise Blocked('Packaging SDK must be an exact stable version and framework revision')
    return dict(value)

class Deferred(RuntimeError):
    def __init__(self, until, reason):
        super().__init__(reason)
        self.until = until


# Purpose: Keep schedule policy explicit at coordinator and SDK-selection boundaries (UNFY-402).
# /**
#  * @param plan The latest journal plan, or None when no journal exists.
#  * @param scheduled Whether the hourly schedule started this run.
#  * @returns Immutable inputs for the shared blocked-batch policy.
#  * @throws Nothing; construction performs no I/O.
#  */
@dataclass(frozen=True, kw_only=True)
class ScheduleContext:
    plan: dict | None
    scheduled: bool


# Purpose: Bind a blocker to the loaded journal without positional argument ambiguity (UNFY-402).
# /**
#  * @param journal The durable journal that persists batch plans.
#  * @param plan The latest loaded journal plan, or None when no journal exists.
#  * @param error The failure requiring operator attention.
#  * @returns Immutable inputs for recording a batch blocker.
#  * @throws Nothing; construction performs no I/O.
#  */
@dataclass(frozen=True, kw_only=True)
class BlockerContext:
    journal: object
    plan: dict | None
    error: Blocked


BLOCKER_PROTECTED_STATUSES = frozenset({'blocked', 'complete', 'cancelled'})


# Purpose: Decide whether a scheduled run must leave a batch alone until an operator acts (UNFY-402).
# /**
#  * @param context The loaded plan and explicit schedule policy inputs.
#  * @returns True when the batch is blocked and only a manual dispatch may touch it.
#  * @throws KeyError if a present plan lacks its required status.
#  */
def quiet_on_schedule(context):
    return bool(context.scheduled and context.plan is not None and context.plan['status'] == 'blocked')


# Purpose: Prevent blocker handling from re-drafting terminal or non-journal records (UNFY-402).
# /**
#  * @param plan The latest journal plan, or None when no journal exists.
#  * @returns Whether this in-flight journal may persist a new blocker.
#  * @throws KeyError if an identified journal lacks its required status.
#  */
def can_record_blocker(plan):
    return (plan is not None and plan.get('journal_id') is not None
            and plan['status'] not in BLOCKER_PROTECTED_STATUSES)


# Purpose: Make scheduled resumes go quiet after a blocker raised outside the coordinator's run (UNFY-402).
# /**
#  * @param context The journal, loaded plan and blocker bound to this operation.
#  * @returns None; only an in-flight journal is rewritten. Absent journals and
#  *          complete, cancelled or already blocked ones are left alone, so a
#  *          journal that is no longer a draft is never re-drafted by save().
#  * @throws Blocked or a transport error when the journal cannot be saved.
#  */
def record_blocker(context):
    plan = context.plan
    if not can_record_blocker(plan):
        return
    plan.update(status='blocked', error=str(context.error))
    context.journal.save(plan)


# Purpose: Make blocked release attempts reconstructable without parsing freeform output (UNFY-402).
# /**
#  * @param context The operation, batch tag when known, and blocker to report.
#  * @returns None; emits one structured error event on stderr.
#  * @throws OSError if the diagnostic cannot be written.
#  */
def log_blocker(context):
    print(json.dumps({'level': 'error', 'event': 'release_batch_blocked',
                      'operation': context['operation'], 'batch_tag': context['batch_tag'],
                      'error': str(context['error'])}), file=sys.stderr, flush=True)


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
    # The SDK can hide reset headers. Use the longest known publication window
    # and persist another pause if the server is still limited on the next run.
    return now + 86400


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
                    raise Deferred(deadline if deadline is not None else self.now()+86400,
                                   'Hosted API quota cooldown') from error
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
