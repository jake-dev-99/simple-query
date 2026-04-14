#include "simple_query_windows_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "include/simple_query_windows/simple_query_windows_plugin.h"

void SimpleQueryWindowsPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  simple_query_windows::SimpleQueryWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
