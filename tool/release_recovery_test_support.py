"""Recovery fixtures stay reusable without creating another discovered test suite."""
import json

from release_github import ReleaseListing


# Purpose: Keep recovery listing evidence shared without changing file-backed journal behavior.
# /**
#  * @param None The inheriting journal supplies its file and separately journaled batch identities.
#  * @returns Read and listing operations that prevent false orphan recovery from stale journals.
#  * @throws OSError or ValueError if persisted fixture data cannot establish release evidence.
#  */
class ReleaseJournalListingTestSupport:
    # Purpose: Keep recovery decisions bound to every journal visible in the fixture.
    # /**
    #  * @param self The file-backed journal fixture.
    #  * @returns The latest persisted plan, or None before journal creation.
    #  * @throws OSError or ValueError if persisted fixture data cannot be read.
    #  */
    def load(self):
        plan = json.loads(self.file.read_text()) if self.file.exists() else None
        self.record_listing(plan)
        return plan

    # Purpose: Prevent an older journal from making a newer journaled tag look orphaned.
    # /**
    #  * @param self The journal fixture retaining other batch identities.
    #  * @param plan The latest persisted plan, if one exists.
    #  * @returns None; updates the release-listing evidence used by recovery.
    #  * @throws KeyError if a persisted fixture omits its immutable batch identity.
    #  */
    def record_listing(self, plan):
        journaled = ({plan['batch_tag']} if plan else set()) | self.other_journals
        self.listing = ReleaseListing(release_count=len(journaled), journaled=frozenset(journaled))


# Purpose: Keep orphan-recovery scenarios on the coordinator's real Git and journal fixtures.
# /**
#  * @param None The inheriting test case supplies its runner, journal, repository and clock.
#  * @returns Fixture operations that preserve one test case's isolated release evidence.
#  * @throws AssertionError or fixture errors when a scenario cannot establish its preconditions.
#  */
class ReleaseRecoveryTestSupport:
    # Purpose: Reproduce the tag-without-journal window that recovery must handle safely.
    # /**
    #  * @param self The coordinator fixture with a real disposable Git remote.
    #  * @param run_id The dispatch identity whose journal creation is interrupted.
    #  * @returns None; leaves the immutable tag without its corresponding journal.
    #  * @throws AssertionError if the cut does not reach journal creation.
    #  */
    def crash_journal_creation(self, run_id='12345'):
        create = self.journal.create
        # Purpose: Keep the immutable intent durable while simulating journal transport failure.
        # /**
        #  * @param plan The frozen plan that would otherwise become a journal.
        #  * @returns Never returns.
        #  * @throws RuntimeError to interrupt the cut after its tag was pushed.
        #  */
        def crash(plan):
            raise RuntimeError('Journal creation interrupted')
        self.journal.create = crash
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            self.runner(run_id).run('cut')
        self.journal.create = create

    # Purpose: Ensure a later dispatch cannot silently reuse a completed batch's source.
    # /**
    #  * @param self The coordinator fixture after a completed release.
    #  * @returns None; advances the disposable main branch with unpublished Android work.
    #  * @throws OSError or CalledProcessError if the fixture cannot advance its Git remote.
    #  */
    def follow_up_change(self):
        self.fixture.git('reset', '--hard', 'origin/main')  # Discard the disposable prepared checkout.
        (self.root / 'packages/android/lib/plugin.dart').write_text('// next source change')
        self.fixture.commit('Further Android work')
        self.fixture.git('push', 'origin', 'main')

    # Purpose: Model a readable release listing before a later cut loses its journal.
    # /**
    #  * @param self The coordinator fixture starting with unpublished work.
    #  * @returns None; leaves a prior completed journal and a newer orphan intent.
    #  * @throws AssertionError if the initial release or injected interruption fails.
    #  */
    def next_orphan_after_complete_batch(self):
        self.assertEqual(self.cut(), 'complete')
        self.follow_up_change()
        self.crash_journal_creation('12346')

    # Purpose: Exercise distinct dispatch identities without bypassing coordinator recovery.
    # /**
    #  * @param self The coordinator fixture with durable journal and Git evidence.
    #  * @param operation The public coordinator operation to execute.
    #  * @param options The run identity and named coordinator flags for the scenario.
    #  * @returns The coordinator's externally observable operation result.
    #  * @throws Blocked or Deferred when the operation cannot safely progress.
    #  */
    def run_recovery_operation(self, operation, **options):
        runner = self.runner(options.pop('run_id', '12347'))
        return runner.run(operation, **options)

    # Purpose: Detect recovery writes to journals, remote tags, or validation history together.
    # /**
    #  * @param self The coordinator fixture with its real remote and validation trace.
    #  * @returns The persisted journal, remote tags, and completed validation evidence.
    #  * @throws OSError or CalledProcessError if the fixture evidence cannot be inspected.
    #  */
    def recovery_evidence(self):
        return {'journal': self.journal.load(),
                'tags': self.fixture.git('ls-remote', '--tags', 'origin'),
                'validation': self.trace.read_text() if self.trace.exists() else None}

    # Purpose: Preserve legacy intent evidence so recovery must refuse its missing SDK pin.
    # /**
    #  * @param self The coordinator fixture after a completed journaled release.
    #  * @returns None; publishes a pre-pin orphan on the disposable Git remote.
    #  * @throws CalledProcessError if the fixture cannot publish the annotated intent.
    #  */
    def publish_legacy_orphan_intent(self):
        source = self.fixture.git('rev-parse', 'origin/main')
        intent = {'schema': 1, 'source_sha': source, 'prepared_sha': None, 'status': 'planning',
                  'phase': 'planning', 'not_before': 0, 'error': None,
                  'package_paths': self.fixture.packages, 'packages': [], 'files': {},
                  'target': 'all', 'bump': 'patch', 'created_at': self.clock.now(),
                  'batch_tag': 'sz-release-batch-12346'}
        self.fixture.git('tag', '-a', 'sz-release-batch-12346', source, '-m', json.dumps(intent))
        self.fixture.git('push', 'origin', 'refs/tags/sz-release-batch-12346')

    # Purpose: Reproduce a stale latest journal without losing the candidate's existing journal.
    # /**
    #  * @param self The coordinator fixture with a prior journal and a newer orphan tag.
    #  * @returns None; exposes a planning journal plus proof the newer tag is journaled.
    #  * @throws OSError if the duplicate-journal fixture cannot be persisted.
    #  */
    def retain_duplicate_journal_evidence(self):
        stale = self.journal.load()
        stale['status'], stale['phase'] = 'planning', 'planning'
        self.journal.save(stale)
        self.journal.other_journals = {'sz-release-batch-12346'}
