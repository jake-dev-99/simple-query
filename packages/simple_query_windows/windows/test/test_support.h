// Helpers shared by the native test files. Each test runs on the test
// runner's main thread, which plays the platform thread and pumps its own
// message queue.

#ifndef SIMPLE_QUERY_WINDOWS_TEST_TEST_SUPPORT_H_
#define SIMPLE_QUERY_WINDOWS_TEST_TEST_SUPPORT_H_

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "detached_threads.h"

namespace simple_query_windows {
namespace test {

// Long enough for a busy CI machine, short enough to fail fast.
inline constexpr std::chrono::milliseconds kDeliveryTimeout{5000};
// Teardown must finish well inside one long polling interval.
inline constexpr std::chrono::milliseconds kPromptTeardown{2000};

// Purpose: Run every message already waiting in this thread's queue.
// @returns Nothing.
// @throws Nothing.
inline void PumpQueuedMessages() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

// Purpose: Pump this thread's messages until a condition holds.
// @param done is the condition to wait for.
// @param timeout is how long to keep pumping.
// @returns true when done held before the timeout.
// @throws Nothing beyond what done throws.
inline bool PumpUntil(const std::function<bool()>& done,
                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    PumpQueuedMessages();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Purpose: Wait for a condition without running queued messages, so queued
// work stays queued.
// @param done is the condition to wait for.
// @param timeout is how long to wait.
// @returns true when done held before the timeout.
// @throws Nothing beyond what done throws.
inline bool WaitWithoutPumping(const std::function<bool()>& done,
                               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Purpose: Post to this thread until Windows refuses more posted messages,
// so the next post to this thread's windows fails.
// @returns true when a post was refused.
// @throws Nothing.
inline bool FillMessageQueue() {
  // Windows allows 10,000 posted messages per queue by default.
  constexpr int kMaxPosts = 100000;
  for (int post = 0; post < kMaxPosts; ++post) {
    if (!PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0)) {
      return true;
    }
  }
  return false;
}

// Purpose: Report whether text is an ISO 8601 UTC time with milliseconds,
// the form Dart's DateTime.parse reads.
// @param text is the text to check.
// @returns true for YYYY-MM-DDTHH:MM:SS.mmmZ.
// @throws Nothing.
inline bool IsIsoUtc(const std::string& text) {
  static const std::regex kIsoUtc(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
  return std::regex_match(text, kIsoUtc);
}

// Purpose: Measure how long an action takes.
// @param action is the action to time.
// @returns The elapsed time.
// @throws Nothing beyond what action throws.
inline std::chrono::steady_clock::duration TimeOf(const std::function<void()>& action) {
  const auto started = std::chrono::steady_clock::now();
  action();
  return std::chrono::steady_clock::now() - started;
}

// Purpose: Read the ids an observe event reports.
// @param event is the event map.
// @returns The ids, in the event's order.
// @throws std::bad_variant_access when the event has no id list.
inline std::vector<std::string> IdsOf(const flutter::EncodableMap& event) {
  std::vector<std::string> ids;
  for (const auto& id :
       std::get<flutter::EncodableList>(event.at(flutter::EncodableValue("ids")))) {
    ids.push_back(std::get<std::string>(id));
  }
  return ids;
}

// Purpose: Capture standard error until destroyed. Destroy it only after
// every thread that writes to it has been joined.
class StandardErrorCapture {
 public:
  // Purpose: Start sending standard error to this capture.
  // @returns Nothing.
  // @throws Nothing.
  StandardErrorCapture() : original_(std::cerr.rdbuf(captured_.rdbuf())) {}

  // Purpose: Send standard error back where it went before.
  // @returns Nothing.
  // @throws Nothing.
  ~StandardErrorCapture() { std::cerr.rdbuf(original_); }

  StandardErrorCapture(const StandardErrorCapture&) = delete;
  StandardErrorCapture& operator=(const StandardErrorCapture&) = delete;

  // Purpose: Split what was written so far into lines.
  // @returns The captured lines, without their line endings.
  // @throws Nothing.
  std::vector<std::string> Lines() const {
    std::vector<std::string> lines;
    std::istringstream text(captured_.str());
    for (std::string line; std::getline(text, line);) {
      lines.push_back(line);
    }
    return lines;
  }

 private:
  std::ostringstream captured_;
  std::streambuf* original_;
};

// Purpose: Wait, when destroyed, until every detached worker has finished.
// Workers outlive the objects that start them, so a test declares this
// after everything a worker reads, such as a temp directory or a snapshot
// source, and before what starts the workers; those objects are then
// destroyed only once no worker can touch them.
class DetachedThreadsDrain {
 public:
  // Purpose: Prepare to wait for detached workers.
  // @returns Nothing.
  // @throws Nothing.
  DetachedThreadsDrain() = default;

  // Purpose: Wait until no detached worker is running.
  // @returns Nothing.
  // @throws Nothing; a worker still running after the delivery timeout ends
  // the run, because destroying what it reads would crash it later in some
  // other test.
  ~DetachedThreadsDrain() {
    if (!WaitWithoutPumping([]() { return RunningDetachedThreads() == 0; },
                            kDeliveryTimeout)) {
      // Written straight to the process's standard error: a test may be
      // capturing std::cerr, and the run ends before gtest reports.
      std::fprintf(stderr, "a detached worker was still running after %lld ms\n",
                   static_cast<long long>(kDeliveryTimeout.count()));
      std::abort();
    }
  }

  DetachedThreadsDrain(const DetachedThreadsDrain&) = delete;
  DetachedThreadsDrain& operator=(const DetachedThreadsDrain&) = delete;
};

// Purpose: Own an empty directory for one test, removed afterwards.
class TempDirectory {
 public:
  // Purpose: Create a new empty directory under the system temp directory.
  // @returns Nothing.
  // @throws std::filesystem::filesystem_error when it cannot be created.
  TempDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("simple_query_windows_test_" +
               std::to_string(GetCurrentProcessId()) + "_" +
               std::to_string(next_index_++))) {
    std::filesystem::create_directories(path_);
  }

  // Purpose: Remove the directory and everything in it.
  // @returns Nothing.
  // @throws Nothing; a failed removal is reported as a test failure.
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) {
      ADD_FAILURE() << "could not remove " << path_.string() << ": "
                    << error.message();
    }
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  // Purpose: Give the directory's path.
  // @returns The directory's path.
  // @throws Nothing.
  const std::filesystem::path& path() const { return path_; }

  // Purpose: Create one small file in the directory.
  // @param name is the file name.
  // @returns The new file's path.
  // @throws Nothing; a failed write shows up as a missing change.
  std::filesystem::path WriteFile(const std::string& name) const {
    std::ofstream(path_ / name) << "changed";
    return path_ / name;
  }

 private:
  static inline int next_index_ = 0;
  std::filesystem::path path_;
};

}  // namespace test
}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_TEST_TEST_SUPPORT_H_
