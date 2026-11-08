#ifndef FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_
#define FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace simple_query_windows {

class NativeQueryHostApiImpl;

// Purpose: Serve simple_query's Windows host API for one Flutter engine.
// Each engine gets its own instance, so its calls and observe events stay on
// its own messenger.
class SimpleQueryWindowsPlugin : public flutter::Plugin {
 public:
  // Purpose: Create this engine's plugin and hand it to the registrar.
  // @param registrar is the engine's plugin registrar.
  // @returns Nothing.
  // @throws Nothing.
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  // Purpose: Start answering host API calls on one engine's messenger.
  // Construct it on that engine's platform thread.
  // @param messenger is the engine's messenger; it must outlive the plugin.
  // @returns Nothing.
  // @throws Nothing.
  explicit SimpleQueryWindowsPlugin(flutter::BinaryMessenger *messenger);

  // Purpose: Remove this engine's handlers, then stop every observer.
  // The registrar destroys plugins while the messenger is still alive.
  // @returns Nothing.
  // @throws Nothing.
  ~SimpleQueryWindowsPlugin() override;

  SimpleQueryWindowsPlugin(const SimpleQueryWindowsPlugin&) = delete;
  SimpleQueryWindowsPlugin& operator=(const SimpleQueryWindowsPlugin&) = delete;

 private:
  flutter::BinaryMessenger *messenger_;
  // Owned per engine; stops its observers before the messenger goes away.
  std::unique_ptr<NativeQueryHostApiImpl> host_api_;
};

}  // namespace simple_query_windows

#endif  // FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_
