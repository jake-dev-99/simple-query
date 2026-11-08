#ifndef SIMPLE_QUERY_WINDOWS_DOMAIN_SNAPSHOTS_H_
#define SIMPLE_QUERY_WINDOWS_DOMAIN_SNAPSHOTS_H_

#include <filesystem>
#include <string>

#include "polling_observers.h"
#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Name what one observer snapshot covers.
struct SnapshotRequest {
  // The directory a files or media observer watches.
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
