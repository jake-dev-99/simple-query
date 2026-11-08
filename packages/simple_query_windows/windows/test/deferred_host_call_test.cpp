#include "deferred_host_call.h"

#include <gtest/gtest.h>
#include <new>
#include <thread>

#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

// Purpose: A deferred allocation failure becomes an unavailable reply on the
// platform thread, without running the success finisher or ending the worker.
TEST(DeferredHostCallTest, RepliesUnavailableAfterABackgroundAllocationFailure) {
  bool replied = false;
  bool finished = false;
  std::thread::id work_thread;
  std::thread::id reply_thread;
  StandardErrorCapture log;
  DetachedThreadsDrain drain;
  BackgroundCalls calls(4);
  auto job = MakeBackgroundJob(BackgroundCall<ErrorOr<flutter::EncodableMap>,
                                             flutter::EncodableMap>{
      .work = [&](const StopRequested&) -> ErrorOr<flutter::EncodableMap> {
        work_thread = std::this_thread::get_id();
        throw std::bad_alloc();
      },
      .finish = [&](ErrorOr<flutter::EncodableMap> value) {
        finished = true;
        return value;
      },
      .result = [&](ErrorOr<flutter::EncodableMap> reply) {
        reply_thread = std::this_thread::get_id();
        replied = true;
        EXPECT_TRUE(reply.has_error());
        if (reply.has_error()) {
          EXPECT_EQ(reply.error().code(), "unavailable");
          EXPECT_NE(reply.error().message().find("simple_query:"), std::string::npos);
        }
      }});

  ASSERT_EQ(calls.Enqueue(std::move(job)), EnqueueResult::kQueued);
  ASSERT_TRUE(PumpUntil([&]() { return replied; }, kDeliveryTimeout));
  EXPECT_FALSE(finished);
  EXPECT_EQ(reply_thread, std::this_thread::get_id());
  EXPECT_NE(work_thread, reply_thread);
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
