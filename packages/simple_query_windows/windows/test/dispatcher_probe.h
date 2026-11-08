// The interface of the probe DLL, which holds its own copy of the
// platform-thread dispatcher so a test can see which module the dispatcher
// registers its window class for.

#ifndef SIMPLE_QUERY_WINDOWS_TEST_DISPATCHER_PROBE_H_
#define SIMPLE_QUERY_WINDOWS_TEST_DISPATCHER_PROBE_H_

namespace simple_query_windows {
namespace test {

// Creates a ready dispatcher inside the probe DLL; returns null when it is
// not ready.
using CreateProbeDispatcherFunction = void* (*)();
// Destroys a dispatcher the probe DLL created.
using DestroyProbeDispatcherFunction = void (*)(void*);

inline constexpr const char* kCreateProbeDispatcherExport = "CreateProbeDispatcher";
inline constexpr const char* kDestroyProbeDispatcherExport = "DestroyProbeDispatcher";

}  // namespace test
}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_TEST_DISPATCHER_PROBE_H_
