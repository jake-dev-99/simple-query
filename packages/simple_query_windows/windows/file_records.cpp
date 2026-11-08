#include "file_records.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace simple_query_windows {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Report whether a MIME type is an image, video or audio type.
// @param mime is the MIME type.
// @returns true for image/, video/ and audio/ types.
// @throws Nothing.
bool IsMediaMime(const std::string& mime) {
  return mime.rfind("image/", 0) == 0 || mime.rfind("video/", 0) == 0 ||
         mime.rfind("audio/", 0) == 0;
}

// Purpose: Name the media kind of a MIME type.
// @param mime is the MIME type.
// @returns "image", "video", "audio" or "other".
// @throws Nothing.
std::string MediaType(const std::string& mime) {
  if (mime.rfind("image/", 0) == 0) return "image";
  if (mime.rfind("video/", 0) == 0) return "video";
  if (mime.rfind("audio/", 0) == 0) return "audio";
  return "other";
}

// Purpose: Read an entry's last write time.
// @param entry is the directory entry.
// @returns The last write time in milliseconds on the file clock's epoch, or
// nothing when it cannot be read, for example because the file was removed.
// @throws Nothing.
std::optional<int64_t> ModifiedEpochMs(const std::filesystem::directory_entry& entry) {
  std::error_code ec;
  const auto written = entry.last_write_time(ec);
  if (ec) {
    return std::nullopt;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(written.time_since_epoch())
      .count();
}

}  // namespace

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string MimeFromPath(const std::filesystem::path& path) {
  const std::string ext = Lower(path.extension().string());
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".gif") return "image/gif";
  if (ext == ".heic") return "image/heic";
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".wav") return "audio/wav";
  if (ext == ".aac") return "audio/aac";
  if (ext == ".flac") return "audio/flac";
  if (ext == ".txt") return "text/plain";
  if (ext == ".json") return "application/json";
  return "application/octet-stream";
}

std::vector<EncodableMap> ListRecords(const ListRecordsOptions& options) {
  const std::filesystem::path& root = options.root;
  const bool media_only = options.media_only;
  std::vector<EncodableMap> rows;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    return rows;
  }

  for (auto it = std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    // A recursive walk of a large tree can outlast an observer's teardown.
    if (IsStopRequested(options.stop_requested)) {
      break;
    }
    if (ec) {
      ec.clear();
      continue;
    }

    const auto& entry = *it;
    const auto path = entry.path();
    const bool is_dir = entry.is_directory(ec);
    const std::string mime = MimeFromPath(path);

    if (media_only) {
      if (is_dir || !IsMediaMime(mime)) {
        continue;
      }
      const auto modified = ModifiedEpochMs(entry);
      if (!modified.has_value()) {
        continue;
      }
      EncodableMap row;
      row[EncodableValue("id")] = EncodableValue(path.string());
      row[EncodableValue("uriOrPath")] = EncodableValue(path.string());
      row[EncodableValue("mediaType")] = EncodableValue(MediaType(mime));
      row[EncodableValue("mimeType")] = EncodableValue(mime);
      if (entry.is_regular_file(ec)) {
        row[EncodableValue("size")] = EncodableValue(static_cast<int64_t>(entry.file_size(ec)));
      }
      row[EncodableValue("createdAt")] = EncodableValue(std::to_string(*modified));
      row[EncodableValue("modifiedAt")] = EncodableValue(std::to_string(*modified));
      rows.push_back(std::move(row));
      continue;
    }

    const auto modified = ModifiedEpochMs(entry);
    if (!modified.has_value()) {
      continue;
    }
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(path.string());
    row[EncodableValue("path")] = EncodableValue(path.string());
    row[EncodableValue("name")] = EncodableValue(path.filename().string());
    row[EncodableValue("isDirectory")] = EncodableValue(is_dir);
    if (entry.is_regular_file(ec)) {
      row[EncodableValue("size")] = EncodableValue(static_cast<int64_t>(entry.file_size(ec)));
    }
    row[EncodableValue("modifiedAt")] = EncodableValue(std::to_string(*modified));
    row[EncodableValue("mimeType")] = EncodableValue(mime);
    row[EncodableValue("type")] = EncodableValue(is_dir ? "directory" : "file");
    row[EncodableValue("extension")] = EncodableValue(path.extension().string());
    row[EncodableValue("modifiedEpochMs")] = EncodableValue(*modified);
    rows.push_back(std::move(row));
  }

  return rows;
}

}  // namespace simple_query_windows
