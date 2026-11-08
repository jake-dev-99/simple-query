#include "include/simple_query_windows/simple_query_windows_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "include/simple_query_windows/simple_query_windows_plugin.h"

// Purpose: Register the plugin with the registrar Flutter passes through the
// C API.
// @param registrar is the engine's registrar for this plugin.
// @returns Nothing.
// @throws Nothing.
void SimpleQueryWindowsPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  simple_query_windows::SimpleQueryWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
