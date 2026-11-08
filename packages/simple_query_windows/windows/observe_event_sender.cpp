#include "observe_event_sender.h"

#include "structured_log.h"

namespace simple_query_windows {

// Purpose: Prepare to send through one engine's messenger.
// @param messenger is the engine's messenger; it must outlive this object.
// @returns Nothing.
// @throws Nothing.
ObserveEventSender::ObserveEventSender(flutter::BinaryMessenger* messenger)
    : flutter_api_(messenger) {}

// Purpose: Send one observe event.
// @param observer_id is the observer the event belongs to.
// @param event is the change event map.
// @param on_settled runs on the platform thread once Dart accepts or rejects
// the event.
// @returns Nothing.
// @throws Nothing.
void ObserveEventSender::Send(const std::string& observer_id,
                              const flutter::EncodableMap& event,
                              std::function<void()> on_settled) {
  // Pigeon keeps the success and error handlers apart, and runs exactly one
  // of them, so each holds its own copy of on_settled. Both copy rather than
  // move: the order the two handlers are built in is unspecified.
  flutter_api_.OnObserveEvent(
      observer_id, event,
      [reported = rejected_event_reported_, on_settled]() {
        *reported = false;
        on_settled();
      },
      [observer_id, reported = rejected_event_reported_,
       on_settled](const FlutterError& error) {
        if (!*reported) {
          *reported = true;
          WriteLog({LogLevel::kWarning,
                    "observe_event_rejected",
                    {{"observer_id", observer_id},
                     {"code", error.code()},
                     {"message", error.message()},
                     {"next_log", "after an event is accepted"}}});
        }
        on_settled();
      });
}

}  // namespace simple_query_windows
