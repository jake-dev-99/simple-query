#include "domain_snapshots.h"

#include <stdexcept>

#include "file_records.h"
#include "owned_message_error.h"
#include "rows_result.h"
#include "winrt_records.h"

namespace simple_query_windows {

// Purpose: Take one observer snapshot of a domain's records.
// @param source names the domain, its root directory and the stop check.
// @returns Each record's id mapped to a fingerprint of its row.
// @throws ReadDeniedError when the user has not granted access,
// std::runtime_error when the read fails or stops before it finishes, and
// std::invalid_argument for a domain observe does not support.
Snapshot BuildSnapshotForDomain(const SnapshotRequest& source) {
  const std::string& domain = source.domain;
  if (domain == "files" || domain == "media") {
    return FingerprintRecords(RowsOrThrow(ListRecords({.root = source.root,
                                                       .media_only = domain == "media",
                                                       .stop_requested = source.stop_requested})));
  }
  if (domain == "contacts" || domain == "calendar") {
    return FingerprintRecords(RowsOrThrow(domain == "contacts"
                                              ? ListContactRecords(source.stop_requested)
                                              : ListCalendarRecords(source.stop_requested)));
  }
  throw OwnedMessageError<std::invalid_argument>(
      "simple_query: windows observe has no snapshot for domain " + domain);
}

}  // namespace simple_query_windows
