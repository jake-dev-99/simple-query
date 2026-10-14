"""SDK provenance regressions use frozen identities and original Deploy records."""
import json
import unittest
from unittest.mock import Mock

from release_services import Blocked


# Purpose: Cover current versus frozen SDK selection without live release writes.
# /**
#  * @param unittest.TestCase The standard isolated assertion fixture.
#  * @returns A suite exercising stable releases and source-bound SDK recovery.
#  * @throws AssertionError when packaging depends on a moving channel.
#  */
class ToolchainTest(unittest.TestCase):
    # Purpose: Keep SDK drift and its original deployment evidence deterministic.
    # /**
    #  * @param self The isolated SDK fixture.
    #  * @returns None; defines a newer stable release and an older frozen upload.
    #  * @throws ImportError while the SDK-provenance implementation is absent.
    #  */
    def setUp(self):
        import release_toolchain
        self.module = release_toolchain
        self.original = {'version': '3.47.7', 'revision': 'a' * 40, 'channel': 'stable'}
        self.latest = {'version': '3.50.0', 'revision': 'b' * 40, 'channel': 'stable'}
        self.catalog = {'current_release': {'stable': 'b' * 40}, 'releases': [
            {'version': '3.50.0', 'hash': 'b' * 40, 'channel': 'stable'},
            {'version': '3.47.7', 'hash': 'a' * 40, 'channel': 'stable'},
            {'version': '3.51.0-0.1.pre', 'hash': 'c' * 40, 'channel': 'beta'}]}
        self.plan = {'source_sha': 'd' * 40, 'prepared_sha': 'e' * 40,
                     'batch_tag': 'sz-release-batch-123', 'phase': 'publication',
                     'status': 'blocked', 'files': {}, 'packages': [
                         {'name': 'demo', 'tag': 'demo-v1.0.1', 'run_id': 7,
                          'state': 'published'}]}
        self.journal = Mock()
        self.journal.run.return_value = {'id': 7, 'status': 'completed', 'conclusion': 'success'}
        self.journal.logs.return_value = 'FLUTTER_ROOT: /opt/hostedtoolcache/flutter/stable-3.47.7-x64/flutter'

    # Purpose: Allow future cuts to advance while a frozen upload keeps its SDK.
    # /**
    #  * @param self The catalog containing two stable SDK identities.
    #  * @returns None; asserts exact historic and latest-stable selections differ.
    #  * @throws AssertionError if current channel state overrides frozen identity.
    #  */
    def test_selects_exact_release_and_current_stable_separately(self):
        self.assertEqual(self.module.released_sdk(self.catalog), self.latest)
        self.assertEqual(self.module.released_sdk(self.catalog, '3.47.7'), self.original)
        with self.assertRaises(Blocked):
            self.module.released_sdk(self.catalog, '3.51.0-0.1.pre')

    # Purpose: Preserve a newly frozen SDK after stable advances.
    # /**
    #  * @param self The source-bound batch fixture with an immutable SDK pin.
    #  * @returns None; asserts resume keeps the prepared-source SDK.
    #  * @throws AssertionError if recovery chooses the newer stable SDK.
    #  */
    def test_prepared_sdk_pin_survives_stable_advance(self):
        self.plan['packaging_sdk'] = self.original
        self.plan['files']['tool/release-sdk.json'] = json.dumps(self.original)
        result = self.module.resolve_batch_sdk({'plan': self.plan, 'journal': self.journal,
                                                'catalog': self.catalog})
        self.assertEqual(result, self.original)
        self.journal.logs.assert_not_called()

    # Purpose: Reconstruct an uploaded batch from its original source-matched run.
    # /**
    #  * @param self The prior batch without a persisted SDK identity.
    #  * @returns None; asserts original deployment evidence supplies its exact SDK.
    #  * @throws AssertionError if SDK recovery mutates state or skips source binding.
    #  */
    def test_recovers_original_sdk_from_source_matched_deploy(self):
        result = self.module.resolve_batch_sdk({'plan': self.plan, 'journal': self.journal,
                                                'catalog': self.catalog})
        self.assertEqual(result, self.original)
        self.journal.run.assert_called_once_with(self.plan['packages'][0], 'e' * 40)
        self.journal.save.assert_not_called()

    # Purpose: Keep ambiguous packaging provenance from authorizing receipt checks.
    # /**
    #  * @param self The historical batch and controlled original-run logs.
    #  * @returns None; asserts absent, mixed and nonmatching run evidence blocks.
    #  * @throws AssertionError if missing provenance falls back to current stable.
    #  */
    def test_missing_mixed_and_wrong_run_evidence_fail_closed(self):
        context = {'plan': self.plan, 'journal': self.journal, 'catalog': self.catalog}
        for log in ['', self.journal.logs.return_value + '\nFLUTTER_ROOT: /cache/flutter/stable-3.50.0-x64/flutter']:
            self.journal.logs.return_value = log
            with self.assertRaises(Blocked):
                self.module.resolve_batch_sdk(context)
        self.journal.run.return_value = {'id': 8, 'status': 'completed', 'conclusion': 'success'}
        with self.assertRaises(Blocked):
            self.module.resolve_batch_sdk(context)

    # Purpose: Bind the SDK pin in the prepared files to its batch metadata.
    # /**
    #  * @param self The immutable-source SDK fixture.
    #  * @returns None; asserts a changed pin cannot replace original metadata.
    #  * @throws AssertionError if inconsistent source and SDK identities are accepted.
    #  */
    def test_mismatched_prepared_sdk_pin_is_rejected(self):
        self.plan['packaging_sdk'] = self.original
        self.plan['files']['tool/release-sdk.json'] = json.dumps(self.latest)
        with self.assertRaises(Blocked):
            self.module.resolve_batch_sdk({'plan': self.plan, 'journal': self.journal,
                                           'catalog': self.catalog})

    # Purpose: Permit a new current-stable cut to reconcile an older frozen batch.
    # /**
    #  * @param self The cancelled historical batch plus newer stable catalog.
    #  * @returns None; asserts both SDK identities are explicit and independent.
    #  * @throws AssertionError if a new cut downgrades or loses prior SDK provenance.
    #  */
    def test_cut_can_install_current_and_previous_batch_sdks(self):
        self.plan['status'] = 'cancelled'
        result = self.module.resolve_workflow_sdk({'plan': self.plan, 'journal': self.journal,
                                                   'catalog': self.catalog, 'operation': 'cut'})
        self.assertEqual(result['current'], self.latest)
        self.assertEqual(result['batch'], self.original)
        self.assertEqual(result['context']['prepared_sha'], 'e' * 40)

    # Purpose: Keep cancellation independent of artifact SDK reconstruction.
    # /**
    #  * @param self The frozen batch whose prior logs are unavailable.
    #  * @returns None; asserts a cancellation does not inspect publication SDKs.
    #  * @throws AssertionError if missing SDK proof prevents cancelling future work.
    #  */
    def test_cancel_does_not_require_original_sdk_logs(self):
        self.journal.run.return_value = None
        result = self.module.resolve_workflow_sdk({'plan': self.plan, 'journal': self.journal,
                                                   'catalog': self.catalog, 'operation': 'cancel'})
        self.assertEqual(result['current'], self.latest)
        self.assertEqual(result['context'], {})
        self.journal.run.assert_not_called()

    # Purpose: Allow a new cut after cancellation before any prepared source exists.
    # /**
    #  * @param self The cancelled planning intent without a publishable commit.
    #  * @returns None; asserts nonexistent artifact evidence cannot block a new cut.
    #  * @throws AssertionError if an unprepared cancellation demands SDK logs.
    #  */
    def test_unprepared_cancelled_intent_allows_new_current_stable_cut(self):
        self.plan.update(status='cancelled', phase='planning', prepared_sha=None, packages=[])
        result = self.module.resolve_workflow_sdk({'plan': self.plan, 'journal': self.journal,
                                                   'catalog': self.catalog, 'operation': 'cut'})
        self.assertEqual(result['current'], self.latest)
        self.assertEqual(result['context'], {})
        self.journal.run.assert_not_called()

    # Purpose: Permit replacement of prepared source that was never tagged.
    # /**
    #  * @param self A historical cancelled plan without packaging SDK metadata.
    #  * @returns None; asserts no SDK proof is demanded before any publication.
    #  * @throws AssertionError if preparation alone blocks the next authorized cut.
    #  */
    def test_prepared_but_untagged_cancelled_batch_allows_new_cut(self):
        self.plan['status'] = 'cancelled'
        self.plan['packages'][0].update(state='pending', run_id=None)
        repository = Mock()
        repository.remote_tag_sha.return_value = None
        result = self.module.resolve_workflow_sdk({'plan': self.plan, 'journal': self.journal,
                                                   'repository': repository,
                                                   'catalog': self.catalog, 'operation': 'cut'})
        self.assertEqual(result['current'], self.latest)
        self.assertEqual(result['context'], {})
        self.journal.run.assert_not_called()

    # Purpose: Detect a real tag push whose state checkpoint was interrupted.
    # /**
    #  * @param self The pending journal with a source-bound remote package tag.
    #  * @returns None; asserts prior SDK recovery still covers an unrecorded push.
    #  * @throws AssertionError if pending state masks actual deployment evidence.
    #  */
    def test_uncheckpointed_tag_push_still_recovers_original_sdk(self):
        self.plan['status'] = 'cancelled'
        self.plan['packages'][0].update(state='pending', run_id=None)
        repository = Mock()
        repository.remote_tag_sha.return_value = 'e' * 40
        result = self.module.resolve_workflow_sdk({'plan': self.plan, 'journal': self.journal,
                                                   'repository': repository,
                                                   'catalog': self.catalog, 'operation': 'cut'})
        self.assertEqual(result['batch'], self.original)

    # Purpose: Inspect every cancelled package before choosing a recovery SDK.
    # /**
    #  * @param self The multi-package batch with one matching and one wrong tag.
    #  * @returns None; asserts an earlier valid tag cannot mask later source drift.
    #  * @throws AssertionError if tag inspection returns before checking the batch.
    #  */
    def test_later_cancelled_package_tag_mismatch_is_not_masked(self):
        self.plan['status'] = 'cancelled'
        self.plan['packages'].append({'name': 'second', 'tag': 'second-v1.0.1',
                                       'state': 'pending', 'run_id': None})
        repository = Mock()
        repository.remote_tag_sha.side_effect = ['e' * 40, 'f' * 40]
        with self.assertRaisesRegex(Blocked, 'Cancelled package tag'):
            self.module.needs_batch_sdk({'plan': self.plan, 'repository': repository,
                                         'operation': 'cut'})

    # Purpose: Keep early recovery bound to the immutable SDK authorization.
    # /**
    #  * @param self The mutable journal and original intent-tag SDK identity.
    #  * @returns None; asserts a different valid stable SDK is rejected before prepare.
    #  * @throws AssertionError if planning/preflight can replace the authorized SDK.
    #  */
    def test_early_sdk_selection_must_match_immutable_intent(self):
        repository = Mock()
        repository.intent_sdk.return_value = self.original
        for phase in ['planning', 'preflight', 'push', 'publication']:
            self.plan.update(phase=phase, prepared_sha=None, packaging_sdk=self.latest)
            self.plan['files']['tool/release-sdk.json'] = json.dumps(self.latest)
            with self.assertRaisesRegex(Blocked, 'intent'):
                self.module.resolve_batch_sdk({'plan': self.plan, 'journal': self.journal,
                                               'repository': repository, 'catalog': self.catalog})


if __name__ == '__main__':
    unittest.main()
