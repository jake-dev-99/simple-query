#ifndef SIMPLE_QUERY_WINDOWS_OBSERVE_EVENT_SENDER_H_
#define SIMPLE_QUERY_WINDOWS_OBSERVE_EVENT_SENDER_H_

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>

#include <functional>
#include <memory>
#include <string>

#include "native_query.g.h"
#include "observe_event.h"

namespace simple_query_windows {

// Purpose: Send observe events to one engine's Dart side, logging a run of
// rejected events once. A Dart side that stopped listening rejects every
// event after it, so one log line covers the run; an accepted event ends it,
// so the next rejection is logged again.
//
// Create, call and destroy this object on the platform thread.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class ObserveEventSender {
 public:
  // Purpose: Prepare to send through one engine's messenger.
  // @param messenger is the engine's messenger; it must outlive this object.
  // @returns Nothing.
  // @throws Nothing.
  explicit ObserveEventSender(flutter::BinaryMessenger* messenger);

  // Purpose: Send one observe event.
  // @param event is the event; its on_settled runs on the platform thread
  // once Dart accepts or rejects it.
  // @returns Nothing.
  // @throws Nothing.
  void Send(ObserveEvent event);

 private:
  NativeQueryFlutterApi flutter_api_;
  // Shared with pending replies, which can arrive after this object is gone.
  // Read and written only on the platform thread.
  std::shared_ptr<bool> rejected_event_reported_ = std::make_shared<bool>(false);
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_OBSERVE_EVENT_SENDER_H_
