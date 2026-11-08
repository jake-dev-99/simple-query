#ifndef SIMPLE_QUERY_WINDOWS_DOMAIN_SNAPSHOTS_H_
#define SIMPLE_QUERY_WINDOWS_DOMAIN_SNAPSHOTS_H_

#include <filesystem>
#include <string>

#include "polling_observers.h"
#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Name what one observer snapshot covers. Every field is borrowed,
// so a request must not outlive the call that builds it.
// @param root is the directory a files or media observer watches. A
// contacts or calendar snapshot ignores it.
// @param domain picks the source: files, media, contacts or calendar. Any
// other name throws std::invalid_argument.
// @param stop_requested is checked between records, so a long scan ends
// soon after the observer stops.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct SnapshotRequest {
  const std::filesystem::path& root;
  const std::string& domain;
  const StopRequested& stop_requested;
};

// Purpose: Take one observer snapshot of a domain's records.
// @param source names the domain, its root directory and the stop check.
// @returns Each record's id mapped to a fingerprint of its row.
// @throws ReadDeniedError when the user has not granted access,
// std::runtime_error when the read fails or stops before it finishes, and
// std::invalid_argument for a domain observe does not support.
Snapshot BuildSnapshotForDomain(const SnapshotRequest& source);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_DOMAIN_SNAPSHOTS_H_
