#ifndef SIMPLE_QUERY_WINDOWS_OBSERVE_EVENT_H_
#define SIMPLE_QUERY_WINDOWS_OBSERVE_EVENT_H_

#include <flutter/encodable_value.h>

#include <functional>
#include <string>

namespace simple_query_windows {

// Purpose: Carry one observe event from an observer to the sender that
// passes it to Dart.
// @param observer_id is the id Start returned; Dart routes the event by it.
// @param event holds domain, changeType, timestamp, ids and source, as
// BuildEvent writes them.
// @param on_settled must run on the platform thread once Dart accepts or
// rejects the event. Until then the observer sends nothing more.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct ObserveEvent {
  std::string observer_id;
  flutter::EncodableMap event;
  std::function<void()> on_settled;
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_OBSERVE_EVENT_H_
