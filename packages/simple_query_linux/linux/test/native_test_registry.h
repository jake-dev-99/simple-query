#pragma once

#include "eds_service_contract_test.h"
#include "observer_delivery_test.h"
#include "mutation_scope_test.h"

/** Purpose: Register every native production-boundary regression.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void RegisterNativeTests() {
  g_test_add_func("/simple_query/generated/async_lifecycle",
                  TestGeneratedAsyncLifecycle);
  g_test_add_func("/simple_query/generated/null_callback_cleanup",
                  TestGeneratedNullCallbackCleanup);
  g_test_add_func("/simple_query/host/dangling_symlink",
                  TestDanglingSymlinkReturnsUnavailable);
  g_test_add_func("/simple_query/host/observer_startup_rollback",
                  TestObserverThreadStartupRollback);
  g_test_add_func("/simple_query/host/unreadable_binary",
                  TestUnreadableBinaryReturnsUnavailable);
  g_test_add_func("/simple_query/host/binary_requires_readable_regular_file",
                  TestOpenBinaryRequiresReadableRegularFile);
  g_test_add_func("/simple_query/host/numeric_sort",
                  TestNumericSortUsesNumericOrder);
  g_test_add_func("/simple_query/host/delete_requires_root",
                  TestDeleteRequiresRootForFilesAndMedia);
  g_test_add_func("/simple_query/host/media_mutation_path",
                  TestMediaMutationUsesPortableRecordPath);
  g_test_add_func("/simple_query/host/mutations_require_root", TestAllMutationsRequireRoot);
  g_test_add_func("/simple_query/host/mutation_outside_targets", TestMutationRejectsOutsideTargets);
  g_test_add_func("/simple_query/host/mutation_outside_renames", TestMutationRejectsOutsideRenames);
  g_test_add_func("/simple_query/host/mutation_symlink_escapes", TestMutationRejectsSymlinkEscapes);
  g_test_add_func("/simple_query/host/mutation_relative_paths", TestMutationResolvesRelativePaths);
  g_test_add_func("/simple_query/host/batch_mutation_scope", TestBatchPreservesMutationScope);
  g_test_add_func("/simple_query/host/lightweight_capabilities",
                  TestCapabilitiesUseOneLightweightEdsProbe);
#if !defined(HAS_LIBEBOOK) && !defined(HAS_LIBECAL)
  g_test_add_func("/simple_query/observer/stop_does_not_block",
                  TestObserverStopDoesNotBlockOnDiscovery);
#endif
#ifdef HAS_LIBEBOOK
  g_test_add_func("/simple_query/eds/contact_revision",
                  TestContactProjectionUsesRevision);
#endif
#ifdef HAS_LIBECAL
  g_test_add_func("/simple_query/eds/calendar_modification",
                  TestCalendarProjectionUsesModificationTime);
#endif
  g_test_add_func("/simple_query/host/unreadable_root",
                  TestUnreadableRootReturnsUnavailable);
  g_test_add_func("/simple_query/host/observer_initial_snapshot_failure",
                  TestObserverInitialSnapshotFailureReturnsError);
  g_test_add_func("/simple_query/host/mutation_filesystem_errors",
                  TestMutationFilesystemErrorsReturnUnavailable);
  g_test_add_func("/simple_query/host/exception_boundary",
                  TestHostExceptionBoundaryReturnsUnavailable);
  g_test_add_func("/simple_query/host/filesystem_and_extension",
                  TestFilesystemAndExtensionBoundaries);
  g_test_add_func("/simple_query/observer/platform_context",
                  TestObserverDispatchesOnPlatformContext);
  g_test_add_func("/simple_query/observer/media_in_place_update",
                  TestMediaObserverDetectsInPlaceUpdate);
  g_test_add_func("/simple_query/observer/recovers_after_snapshot_failure",
                  TestObserverRecoversAfterSnapshotFailure);
  g_test_add_func("/simple_query/observer/stop_drops_queued",
                  TestObserverStopDropsQueuedDelivery);
  g_test_add_func("/simple_query/observer/reports_delivery_failures",
                  TestObserverReportsDeliveryFailures);
  g_test_add_func("/simple_query/observer/disposal_cancels_in_flight",
                  TestObserverDisposalCancelsInFlightDelivery);
}
