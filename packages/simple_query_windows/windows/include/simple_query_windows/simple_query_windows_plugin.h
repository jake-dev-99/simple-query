#ifndef FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_
#define FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace simple_query_windows {

class SimpleQueryWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  SimpleQueryWindowsPlugin();
  ~SimpleQueryWindowsPlugin() override;

  SimpleQueryWindowsPlugin(const SimpleQueryWindowsPlugin&) = delete;
  SimpleQueryWindowsPlugin& operator=(const SimpleQueryWindowsPlugin&) = delete;
};

}  // namespace simple_query_windows

#endif  // FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_PLUGIN_H_
