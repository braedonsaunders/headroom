#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "HeadroomEditor.hpp"
#include "../core/HeadroomPluginCore.hpp"
#include "../../src/dsp/NamComponent.hpp"
#include <map>
#include <mutex>

#ifndef HEADROOM_OBJC_SUFFIX
#error "define HEADROOM_OBJC_SUFFIX per plugin format"
#endif
#define HR_CAT2(a, b) a##b
#define HR_CAT(a, b) HR_CAT2(a, b)
#define HeadroomEditorView HR_CAT(HeadroomEditorView_, HEADROOM_OBJC_SUFFIX)
#define HeadroomScriptProxy HR_CAT(HeadroomScriptProxy_, HEADROOM_OBJC_SUFFIX)

// WKUserContentController retains its handlers: a weak proxy avoids a view ↔ controller cycle.
@interface HeadroomScriptProxy : NSObject <WKScriptMessageHandler>
@property (nonatomic, weak) id<WKScriptMessageHandler> target;
@end
@implementation HeadroomScriptProxy
- (void)userContentController:(WKUserContentController *)c didReceiveScriptMessage:(WKScriptMessage *)m {
    [self.target userContentController:c didReceiveScriptMessage:m];
}
@end

@interface HeadroomEditorView : NSView <WKScriptMessageHandler, WKNavigationDelegate>
@property (nonatomic, strong) WKWebView *webView;
@end

@interface HeadroomEditorView ()
- (void)detachCore;
@end

// Live views per core (unretained), so a core that dies before its view can detach them.
static std::mutex g_views_m;
static std::multimap<headroom::plug::PluginCore *, void *> g_views;

@implementation HeadroomEditorView {
    headroom::plug::PluginCore *_core;
    __weak NSWindow *_observedWindow; // the window whose occlusion / miniaturise notifications we observe
    int _hostVisible;                 // last state sent to the page: -1 unknown, 0 hidden, 1 visible
    BOOL _windowClosing;              // the observed window posted NSWindowWillCloseNotification
}

- (instancetype)initWithCore:(headroom::plug::PluginCore *)core frame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    _core = core;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.wantsLayer = YES;
    self.layer.backgroundColor = [NSColor colorWithSRGBRed:0.08 green:0.08 blue:0.09 alpha:1.0].CGColor;

    if (core) {
        std::lock_guard<std::mutex> lk(g_views_m);
        g_views.emplace(core, (__bridge void *)self);
    }
    const uint16_t port = core ? core->attachEditor() : 0;
    WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
    NSString *boot = [NSString stringWithFormat:@"window.RIG_PORT = %u; window.RIG_HOSTED = true; if(location.protocol==='file:') window.HEADROOM_API_TOKEN='%s';", port, core ? core->apiToken().c_str() : ""];
    [config.userContentController addUserScript:[[WKUserScript alloc] initWithSource:boot
                                                                       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                                    forMainFrameOnly:YES]];
    HeadroomScriptProxy *proxy = [[HeadroomScriptProxy alloc] init];
    proxy.target = self;
    [config.userContentController addScriptMessageHandler:proxy name:@"headroom"];

    self.webView = [[WKWebView alloc] initWithFrame:self.bounds configuration:config];
    self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.webView.navigationDelegate = self; // re-sends the host visibility after each page load
    _hostVisible = -1;
    [self.webView setValue:@NO forKey:@"drawsBackground"];
    [self addSubview:self.webView];

    NSString *uiDir = core ? [NSString stringWithUTF8String:core->uiDirectory().c_str()] : @"";
    NSString *index = [uiDir stringByAppendingPathComponent:@"index.html"];
    if (port != 0 && [[NSFileManager defaultManager] fileExistsAtPath:index]) {
        [self.webView loadFileURL:[NSURL fileURLWithPath:index] allowingReadAccessToURL:[NSURL fileURLWithPath:uiDir isDirectory:YES]];
    } else {
        [self showProblem:port == 0 ? (core ? core->engineError() : std::string("no engine")) : "UI resources missing from the plugin bundle"];
    }
    return self;
}

- (void)showProblem:(const std::string &)why {
    NSString *settings = _core ? [NSString stringWithUTF8String:_core->settingsFilePath().c_str()] : @"";
    NSString *msg = [NSString stringWithUTF8String:why.c_str()] ?: @"";
    NSString *(^esc)(NSString *) = ^NSString *(NSString *s) {
        return [[[s stringByReplacingOccurrencesOfString:@"&" withString:@"&amp;"] stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"]
            stringByReplacingOccurrencesOfString:@">" withString:@"&gt;"];
    };
    NSString *html = [NSString stringWithFormat:
        @"<html><body style='background:#141417;color:#ddd;font:14px -apple-system;padding:40px'>"
         "<h2 style='font-weight:600'>Headroom can't start its engine</h2><p>%@</p>"
         "<p>The plugin passes audio through unchanged until this is fixed. Point it at the Headroom resources folder "
         "(containing <code>app/presets</code>) by editing <code>research_root=</code> in<br><code>%@</code>"
         "<br>(or set <code>HEADROOM_RESEARCH_ROOT</code>), then reload the plugin.</p></body></html>",
        esc(msg), esc(settings)];
    [self.webView loadHTMLString:html baseURL:nil];
}

- (BOOL)isFlipped { return YES; }

// ── Idle CPU: tell the page when the DAW stops showing the editor ──
// WebKit already reports document.visibilityState = "hidden" for a view outside a window, a hidden view or an
// occluded / minimised window, but hosts differ (some keep a closed editor's view alive, offscreen or hidden),
// so the page also gets an explicit HRActivity.setHostVisible(false|true) (app/ui/assets/activity.js), which
// stops every poll, animation and engine analysis while false. The view is not torn down on detach: a hidden
// page costs nothing, and re-attaching then shows instantly instead of reloading the whole UI.
- (BOOL)hr_computeVisible {
    NSWindow *w = self.window;
    return w && !_windowClosing && w.isVisible && !w.isMiniaturized && (w.occlusionState & NSWindowOcclusionStateVisible) && !self.isHiddenOrHasHiddenAncestor;
}
- (void)hr_updateHostVisibility:(BOOL)force {
    const int v = [self hr_computeVisible] ? 1 : 0;
    if (!force && v == _hostVisible) return;
    _hostVisible = v;
    NSString *js = [NSString stringWithFormat:@"window.HRActivity && window.HRActivity.setHostVisible && window.HRActivity.setHostVisible(%@)", v ? @"true" : @"false"];
    [self.webView evaluateJavaScript:js completionHandler:nil];
}
- (void)hr_windowChanged:(NSNotification *)n {
    NSWindow *w = self.window;
    if ([n.name isEqualToString:NSWindowWillCloseNotification]) _windowClosing = YES; // still "visible" until it is gone
    else if (w && w.isVisible && (w.occlusionState & NSWindowOcclusionStateVisible)) _windowClosing = NO; // reopened
    [self hr_updateHostVisibility:NO];
}
- (void)hr_observeWindow:(NSWindow *)w {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    NSArray<NSNotificationName> *names = @[NSWindowDidChangeOcclusionStateNotification, NSWindowDidMiniaturizeNotification,
                                           NSWindowDidDeminiaturizeNotification, NSWindowWillCloseNotification];
    NSWindow *old = _observedWindow;
    if (old == w) return;
    for (NSNotificationName nm in names) if (old) [nc removeObserver:self name:nm object:old];
    _observedWindow = w;
    _windowClosing = NO;
    for (NSNotificationName nm in names) if (w) [nc addObserver:self selector:@selector(hr_windowChanged:) name:nm object:w];
}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self hr_observeWindow:self.window];
    [self hr_updateHostVisibility:NO];
}
- (void)viewDidHide { [super viewDidHide]; [self hr_updateHostVisibility:NO]; }
- (void)viewDidUnhide { [super viewDidUnhide]; [self hr_updateHostVisibility:NO]; }
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation { [self hr_updateHostVisibility:YES]; }

- (void)detachCore { _core = nullptr; } // g_views_m held by the caller

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    if (![message.body isKindOfClass:[NSDictionary class]]) return;
    NSString *action = ((NSDictionary *)message.body)[@"action"];
    NSString *folder = [NSString stringWithUTF8String:rig::namCaptureFolder().c_str()];
    if ([action isEqualToString:@"revealNamFolder"]) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:folder]];
        return;
    }
    if (![action isEqualToString:@"importNam"]) return; // openPluginEditor: nested plugins are app-only
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = YES;
    panel.message = @"Choose Neural Amp Modeler captures (.nam files or folders of them)";
    panel.prompt = @"Import";
    __weak HeadroomEditorView *weakSelf = self;
    void (^done)(NSModalResponse) = ^(NSModalResponse result) {
        if (result != NSModalResponseOK) return;
        NSFileManager *fm = [NSFileManager defaultManager];
        NSInteger copied = 0;
        for (NSURL *url in panel.URLs) {
            BOOL isDir = NO;
            [fm fileExistsAtPath:url.path isDirectory:&isDir];
            NSArray<NSURL *> *files = isDir ? [[fm enumeratorAtURL:url includingPropertiesForKeys:nil options:0 errorHandler:nil] allObjects] : @[url];
            NSString *destDir = isDir ? [folder stringByAppendingPathComponent:url.lastPathComponent] : folder;
            [fm createDirectoryAtPath:destDir withIntermediateDirectories:YES attributes:nil error:nil];
            for (NSURL *f in files) {
                if (![[f.pathExtension lowercaseString] isEqualToString:@"nam"]) continue;
                NSString *dst = [destDir stringByAppendingPathComponent:f.lastPathComponent];
                [fm removeItemAtPath:dst error:nil];
                if ([fm copyItemAtPath:f.path toPath:dst error:nil]) ++copied;
            }
        }
        rig::rescanNamCaptures();
        NSString *js = [NSString stringWithFormat:@"window.headroomNamImported && window.headroomNamImported(%ld)", (long)copied];
        [weakSelf.webView evaluateJavaScript:js completionHandler:nil];
    };
    if (self.window) [panel beginSheetModalForWindow:self.window completionHandler:done];
    else done([panel runModal]);
}

- (void)dealloc {
    {
        std::lock_guard<std::mutex> lk(g_views_m);
        for (auto it = g_views.begin(); it != g_views.end();) it = it->second == (__bridge void *)self ? g_views.erase(it) : std::next(it);
    }
    [self hr_observeWindow:nil];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    // Full teardown: stop the page and drop the web view now, so its WebContent process has no page left (no
    // timers, no HTTP polling of this instance's port) even if the host releases us late.
    self.webView.navigationDelegate = nil;
    [self.webView.configuration.userContentController removeScriptMessageHandlerForName:@"headroom"];
    [self.webView stopLoading];
    [self.webView removeFromSuperview];
    _webView = nil;
}
@end

namespace headroom::plug {
void* createEditorView(PluginCore* core, double width, double height) {
    HeadroomEditorView* v = [[HeadroomEditorView alloc] initWithCore:core frame:NSMakeRect(0, 0, width, height)];
    return (__bridge_retained void*)v;
}
void releaseEditorView(void* view) {
    if (!view) return;
    HeadroomEditorView* v = (__bridge_transfer HeadroomEditorView*)view;
    [v removeFromSuperview];
    v = nil;
}
void detachEditorViews(PluginCore* core) {
    std::lock_guard<std::mutex> lk(g_views_m);
    auto range = g_views.equal_range(core);
    for (auto it = range.first; it != range.second; ++it) [(__bridge HeadroomEditorView*)it->second detachCore];
    g_views.erase(range.first, range.second);
}
} // namespace headroom::plug
