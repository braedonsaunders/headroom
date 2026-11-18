#pragma once
// Native editor windows for hosted plugins (Headroom.app only; implemented in PluginEditor.mm).
namespace rig::plugins {
// Registers the open/close hooks used by PluginHost (call once on the main thread at launch).
void installAppEditorHooks();
// Main thread: shows (or brings to front) the editor of a hosted plugin instance.
void showEditorOnMainThread(int instance_id);
} // namespace rig::plugins
