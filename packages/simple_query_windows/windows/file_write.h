#ifndef SIMPLE_QUERY_WINDOWS_FILE_WRITE_H_
#define SIMPLE_QUERY_WINDOWS_FILE_WRITE_H_

#include <filesystem>
#include <optional>
#include <string>

#include "file_mutations.h"

namespace simple_query_windows {
namespace file_mutation_detail {

// Purpose: Name a complete staged write and its publication policy.
// @param path is the destination.
// @param bytes are its new contents.
// @param exclusive forbids replacing any existing destination for insert.
// @param mutation supplies stop checks and write/flush effects.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FileWrite {
  const std::filesystem::path& path;
  const std::string& bytes;
  bool exclusive;
  const FileMutation& mutation;
};

// Purpose: Resolve and scope-own one staged write through publication and explicit cleanup.
// @param write names the destination, bytes and publication policy.
// @returns No failure or a stop/filesystem error, disclosing retained recovery.
// @throws std::exception after scope-owned staging cleanup or intentional recovery retention.
std::optional<FlutterError> WriteBytes(const FileWrite& write);

}  // namespace file_mutation_detail
}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_FILE_WRITE_H_
