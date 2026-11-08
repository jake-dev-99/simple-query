#ifndef FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_
#define FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <filesystem>
#include <functional>
#include <memory>

namespace simple_query_windows {

class NativeQueryHostApiImpl;

// Purpose: Give the plugin its reads of the process environment, so tests
// can supply their own.
// @param current_directory returns the process working directory.
// @returns Nothing; this is a data type.
// @throws Nothing; callbacks document what they throw.
struct PluginEffects {
  // Returns the working directory, the root of a files or media request that
  // names none. Throws std::system_error when it cannot be read.
  std::function<std::filesystem::path()> current_directory = []() {
    return std::filesystem::current_path();
  };
};

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
  // @param effects supplies the environment reads; the default reads the
  // real process.
  // @returns Nothing.
  // @throws Nothing.
  explicit SimpleQueryWindowsPlugin(flutter::BinaryMessenger *messenger,
                                    PluginEffects effects = {});

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
