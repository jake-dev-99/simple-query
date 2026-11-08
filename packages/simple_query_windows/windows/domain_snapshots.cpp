#include "domain_snapshots.h"

#include <stdexcept>

#include "file_records.h"
#include "winrt_records.h"

namespace simple_query_windows {

// Purpose: Take one observer snapshot of a domain's records.
// @param source names the domain, its root directory and the stop check.
// @returns Each record's id mapped to a fingerprint of its row.
// @throws std::runtime_error when a contacts or calendar store cannot be
// read, std::system_error when a file path cannot be converted to the system
// code page, and std::invalid_argument for a domain observe does not support.
Snapshot BuildSnapshotForDomain(const SnapshotRequest& source) {
  const std::string& domain = source.domain;
  if (domain == "files" || domain == "media") {
    return FingerprintRecords(ListRecords({.root = source.root,
                                           .media_only = domain == "media",
                                           .stop_requested = source.stop_requested}));
  }
  if (domain == "contacts" || domain == "calendar") {
    return FingerprintRecords(RowsOrThrow(domain == "contacts"
                                              ? ListContactRecords(source.stop_requested)
                                              : ListCalendarRecords(source.stop_requested)));
  }
  throw std::invalid_argument("simple_query: windows observe has no snapshot for domain " +
                              domain);
}

}  // namespace simple_query_windows
