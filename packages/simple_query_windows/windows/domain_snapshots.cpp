#include "domain_snapshots.h"

#include <stdexcept>
#include <utility>

#include "file_records.h"
#include "owned_message_error.h"
#include "rows_result.h"
#include "winrt_records.h"

namespace simple_query_windows {

namespace {

// Purpose: Apply the subscription's filters before calculating its snapshot.
// @param rows are the complete records read for this poll.
// @param source optionally supplies borrowed, validated subscription filters.
// @returns Fingerprints of matching records only.
// @throws std::invalid_argument for a record that cannot be fingerprinted.
Snapshot FilteredSnapshot(std::vector<flutter::EncodableMap> rows,
                          const SnapshotRequest& source) {
  if (source.filters != nullptr) {
    rows = ApplyFilters(std::move(rows), *source.filters);
  }
  return FingerprintRecords(rows);
}

}  // namespace

// Purpose: Take one observer snapshot of a domain's records.
// @param source names the domain, its root directory and the stop check.
// @returns Each record's id mapped to a fingerprint of its row.
// @throws ReadDeniedError when the user has not granted access,
// std::runtime_error when the read fails or stops before it finishes, and
// std::invalid_argument for a domain observe does not support.
Snapshot BuildSnapshotForDomain(const SnapshotRequest& source) {
  const std::string& domain = source.domain;
  if (domain == "files" || domain == "media") {
    return FilteredSnapshot(RowsOrThrow(ListRecords({.root = source.root,
                                                       .media_only = domain == "media",
                                                       .stop_requested = source.stop_requested})), source);
  }
  if (domain == "contacts" || domain == "calendar") {
    return FilteredSnapshot(RowsOrThrow(domain == "contacts"
                                              ? ListContactRecords(source.stop_requested)
                                              : ListCalendarRecords(source.stop_requested)), source);
  }
  throw OwnedMessageError<std::invalid_argument>(
      "simple_query: windows observe has no snapshot for domain " + domain);
}

}  // namespace simple_query_windows
