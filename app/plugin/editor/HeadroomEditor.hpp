#pragma once
// Plugin editor: an NSView hosting a WKWebView that loads the bundled UI (Resources/ui/index.html) and talks
// to THIS instance's engine over its ephemeral loopback port (injected as window.RIG_PORT, with
// window.RIG_HOSTED = true so the page never falls back to the standalone app's 17770).
// Objective-C class names carry a per-format suffix (HEADROOM_OBJC_SUFFIX) so the AU and VST3 bundles can
// be loaded into the same host process without class-name collisions.
namespace headroom::plug {
class PluginCore;
constexpr double kEditorWidth = 1280.0, kEditorHeight = 820.0;
// Returns a +1 retained NSView* (as void*). Release with releaseEditorView().
void* createEditorView(PluginCore* core, double width, double height);
void releaseEditorView(void* view);
// Call before a core is destroyed: any view a host still holds stops referring to it.
void detachEditorViews(PluginCore* core);
} // namespace headroom::plug
