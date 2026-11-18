// Plugin editor windows: the plugin's own Cocoa UI (AUv2 kAudioUnitProperty_CocoaUI, or the AUv3 view
// controller via kAudioUnitProperty_RequestViewController), falling back to CoreAudioKit's generic view.
// Everything here runs on the main thread. Each window holds a reference to its AudioUnit so removing
// the unit from the rack never frees a unit whose view is still on screen.
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUCocoaUIView.h>
#import <CoreAudioKit/CoreAudioKit.h>
#include "PluginEditor.hpp"
#include "PluginHost.hpp"
#include <memory>
#include <string>

@interface HRPluginEditorWindow : NSObject <NSWindowDelegate> {
@public
    std::shared_ptr<void> keepalive;
    int instanceId;
}
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSView *pluginView;
@property (nonatomic, strong) NSViewController *viewController;
@end

static NSMutableDictionary<NSNumber *, HRPluginEditorWindow *> *gEditors;

@implementation HRPluginEditorWindow
- (void)viewFrameChanged:(NSNotification *)note {
    const NSSize s = self.pluginView.frame.size;
    if (s.width > 10 && s.height > 10 && !NSEqualSizes(s, self.window.contentView.frame.size)) [self.window setContentSize:s];
}
- (void)windowWillClose:(NSNotification *)note {
    HRPluginEditorWindow *keep = self; // removing it from the table below must not free us mid-method
    [[NSNotificationCenter defaultCenter] removeObserver:keep];
    rig::plugins::setEditorOpen(keep->instanceId, false);
    keep.window.delegate = nil;
    keep.window.contentView = [[NSView alloc] initWithFrame:NSZeroRect];
    keep.pluginView = nil;
    keep.viewController = nil;
    [gEditors removeObjectForKey:@(keep->instanceId)];
    keep->keepalive.reset(); // last reference after a rack removal: the AudioUnit is disposed here
}
@end

static NSView *cocoaViewFor(AudioUnit unit) {
    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) != noErr ||
        size < sizeof(AudioUnitCocoaViewInfo))
        return nil;
    AudioUnitCocoaViewInfo *info = static_cast<AudioUnitCocoaViewInfo *>(malloc(size));
    NSView *view = nil;
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &size) == noErr) {
        const UInt32 nclasses = (size - sizeof(CFURLRef)) / sizeof(CFStringRef);
        NSURL *bundleURL = (__bridge_transfer NSURL *)info->mCocoaAUViewBundleLocation;
        NSString *className = nclasses > 0 ? (__bridge_transfer NSString *)info->mCocoaAUViewClass[0] : nil;
        for (UInt32 i = 1; i < nclasses; ++i)
            if (info->mCocoaAUViewClass[i]) CFRelease(info->mCocoaAUViewClass[i]);
        NSBundle *bundle = bundleURL ? [NSBundle bundleWithURL:bundleURL] : nil;
        Class factoryClass = (bundle && className) ? [bundle classNamed:className] : nil;
        if (factoryClass && [factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]) {
            id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
            view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(800, 500)];
        }
    }
    free(info);
    return view;
}

static void presentEditor(int instanceId, const rig::plugins::EditorTarget &t, NSView *view, NSViewController *vc) {
    if (gEditors[@(instanceId)]) return;
    AudioUnit unit = static_cast<AudioUnit>(t.audio_unit);
    if (!view) {
        AUGenericView *g = [[AUGenericView alloc] initWithAudioUnit:unit
                                                       displayFlags:AUViewTitleDisplayFlag | AUViewPropertiesDisplayFlag | AUViewParametersDisplayFlag];
        g.showsExpertParameters = YES;
        view = g;
    }
    NSSize size = view.frame.size;
    if ((size.width < 10 || size.height < 10) && vc) size = vc.preferredContentSize;
    if (size.width < 10 || size.height < 10) size = NSMakeSize(640, 400);
    [view setFrameSize:size];
    const BOOL resizable = (view.autoresizingMask & (NSViewWidthSizable | NSViewHeightSizable)) != 0 || vc != nil;
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (resizable) style |= NSWindowStyleMaskResizable;
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height) styleMask:style backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;
    w.title = [NSString stringWithFormat:@"%@ — Headroom", [NSString stringWithUTF8String:t.title.c_str()]];
    w.level = NSFloatingWindowLevel; // stays above the rack like a DAW plugin window
    w.hidesOnDeactivate = YES;
    if (resizable) view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    w.contentView = view;

    HRPluginEditorWindow *ed = [[HRPluginEditorWindow alloc] init];
    ed->keepalive = t.keepalive;
    ed->instanceId = instanceId;
    ed.window = w;
    ed.pluginView = view;
    ed.viewController = vc;
    w.delegate = ed;
    if (!resizable) {
        view.postsFrameChangedNotifications = YES;
        [[NSNotificationCenter defaultCenter] addObserver:ed selector:@selector(viewFrameChanged:) name:NSViewFrameDidChangeNotification object:view];
    }
    gEditors[@(instanceId)] = ed;
    rig::plugins::setEditorOpen(instanceId, true);
    [w center];
    const NSInteger n = static_cast<NSInteger>(gEditors.count) - 1;
    if (n > 0) [w setFrameOrigin:NSMakePoint(w.frame.origin.x + 24 * n, w.frame.origin.y - 24 * n)];
    [w makeKeyAndOrderFront:nil];
}

namespace rig::plugins {

void showEditorOnMainThread(int instanceId) {
    if (!gEditors) gEditors = [NSMutableDictionary dictionary];
    if (HRPluginEditorWindow *existing = gEditors[@(instanceId)]) {
        [existing.window makeKeyAndOrderFront:nil];
        return;
    }
    EditorTarget t;
    if (!editorTarget(instanceId, t) || !t.audio_unit) return;
    AudioUnit unit = static_cast<AudioUnit>(t.audio_unit);
    if (t.v3) {
        // AUv3: the extension hands back its view controller asynchronously.
        __block EditorTarget target = t;
        void (^handler)(AUViewControllerBase *) = ^(AUViewControllerBase *vc) {
            dispatch_async(dispatch_get_main_queue(), ^{
                presentEditor(instanceId, target, vc ? vc.view : nil, vc);
            });
        };
        if (AudioUnitSetProperty(unit, kAudioUnitProperty_RequestViewController, kAudioUnitScope_Global, 0, &handler, sizeof(handler)) == noErr) return;
    }
    presentEditor(instanceId, t, cocoaViewFor(unit), nil);
}

static bool openHook(int instanceId, std::string *) {
    dispatch_async(dispatch_get_main_queue(), ^{
        showEditorOnMainThread(instanceId);
    });
    return true;
}

static void closeHook(int instanceId) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (HRPluginEditorWindow *ed = gEditors[@(instanceId)]) [ed.window close];
    });
}

void installAppEditorHooks() {
    if (!gEditors) gEditors = [NSMutableDictionary dictionary];
    setEditorHooks(&openHook, &closeHook);
}

} // namespace rig::plugins
