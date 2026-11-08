#ifndef FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_
#define FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace simple_query_windows {

class NativeQueryHostApiImpl;

class SimpleQueryWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  explicit SimpleQueryWindowsPlugin(flutter::BinaryMessenger *messenger);
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
