// Off macOS there are no hosted-plugin editor windows yet (no plugin hosting either; see PluginHostNull.cpp).
// VST3 hosting will bring IPlugView editors: an HWND child on Windows, an X11 window (XEmbed) on Linux.
#include "PluginEditor.hpp"

namespace rig::plugins {
void installAppEditorHooks() {}
void showEditorOnMainThread(int) {}
} // namespace rig::plugins
