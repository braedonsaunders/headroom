#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <Security/Security.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "../audio/RealtimeAudioServer.hpp"
#include "../dsp/NamComponent.hpp"
#include "../plugins/PluginEditor.hpp"
#include "../plugins/PluginHost.hpp"
#include <memory>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static NSArray<NSURL *> *draggedFileURLs(id<NSDraggingInfo> info) {
    NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    return urls ?: @[];
}

// Practice backing tracks: audio files dropped on the window (or picked with importBackingTracks) go to the
// Practice view, which copies them into the library via /api/backing/import (assets/practice-view.js).
static BOOL btIsAudio(NSURL *u) {
    NSNumber *isDir = nil;
    [u getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (isDir.boolValue || u.pathExtension.length == 0) return NO;
    UTType *t = [UTType typeWithFilenameExtension:u.pathExtension];
    return t && [t conformsToType:UTTypeAudio];
}
static NSArray<NSString *> *btAudioPaths(NSArray<NSURL *> *urls) {
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) if (btIsAudio(u)) [paths addObject:u.path];
    return paths;
}
static NSString *btImportJS(NSArray<NSString *> *paths) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:paths options:0 error:nil];
    NSString *arr = json ? [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] : @"[]";
    return [NSString stringWithFormat:@"window.headroomBackingImport && window.headroomBackingImport(%@)", arr];
}


// ───────────── Guitar Tech: AI provider keys (macOS Keychain) and a streaming fetch proxy ─────────────
// Keys the user enters for the Guitar Tech (OpenRouter, Anthropic, any OpenAI-compatible endpoint) live in the
// login keychain as generic passwords under one service. The page never stores them itself.
static NSString *const kAiKeychainService = @"com.headroom.studio.ai";

static BOOL aiValidAccount(id a) {
    if (![a isKindOfClass:[NSString class]]) return NO;
    NSString *s = (NSString *)a;
    if (s.length == 0 || s.length > 64) return NO;
    NSCharacterSet *bad = [[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-"] invertedSet];
    return [s rangeOfCharacterFromSet:bad].location == NSNotFound;
}
static NSDictionary *aiSecretQuery(NSString *account) {
    return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
             (__bridge id)kSecAttrService: kAiKeychainService,
             (__bridge id)kSecAttrAccount: account};
}
static OSStatus aiSecretSet(NSString *account, NSString *value) {
    NSData *data = [value dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *q = aiSecretQuery(account);
    OSStatus st = SecItemUpdate((__bridge CFDictionaryRef)q, (__bridge CFDictionaryRef)@{(__bridge id)kSecValueData: data});
    if (st == errSecItemNotFound) {
        NSMutableDictionary *add = [q mutableCopy];
        add[(__bridge id)kSecValueData] = data;
        add[(__bridge id)kSecAttrLabel] = [NSString stringWithFormat:@"Headroom Guitar Tech key (%@)", account];
        st = SecItemAdd((__bridge CFDictionaryRef)add, NULL);
    }
    return st;
}
static NSString *aiSecretGet(NSString *account, OSStatus *status) {
    NSMutableDictionary *q = [aiSecretQuery(account) mutableCopy];
    q[(__bridge id)kSecReturnData] = @YES;
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef out = NULL;
    *status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &out);
    if (*status != errSecSuccess || !out) return nil;
    NSData *data = (__bridge_transfer NSData *)out;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}
static NSString *aiJSCall(NSString *fn, NSDictionary *payload) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    NSString *arg = json ? [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] : @"{}";
    return [NSString stringWithFormat:@"window.%@ && window.%@(%@)", fn, fn, arg];
}

// Streams an HTTP(S) request for the page when a provider can't be reached with fetch() (no CORS for a
// file:// page, e.g. Ollama or LM Studio on localhost). JS: postMessage({action:"aiFetch", id, url, method,
// headers, body}); replies arrive as window.headroomAiFetch({id, type:"head", status, headers}),
// {id, type:"data", b64}, then {id, type:"end", error?}. {action:"aiFetchCancel", id} aborts.
@interface HRAiFetchProxy : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, weak) WKWebView *webView;
@property (nonatomic, strong) NSURLSession *session;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NSString *> *reqIds;   // task id -> page request id
@end

@implementation HRAiFetchProxy
- (instancetype)initWithWebView:(WKWebView *)wv {
    if ((self = [super init])) {
        _webView = wv;
        _reqIds = [NSMutableDictionary dictionary];
        NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 120;
        cfg.timeoutIntervalForResource = 900;
        _session = [NSURLSession sessionWithConfiguration:cfg delegate:self delegateQueue:[NSOperationQueue mainQueue]];
    }
    return self;
}
- (void)post:(NSDictionary *)payload { [self.webView evaluateJavaScript:aiJSCall(@"headroomAiFetch", payload) completionHandler:nil]; }
- (void)start:(NSDictionary *)m {
    NSString *rid = [m[@"id"] isKindOfClass:[NSString class]] ? m[@"id"] : nil;
    if (!aiValidAccount(rid)) return;
    NSURL *url = [m[@"url"] isKindOfClass:[NSString class]] ? [NSURL URLWithString:m[@"url"]] : nil;
    NSString *scheme = url.scheme.lowercaseString;
    if (!url || !([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"])) {
        [self post:@{@"id": rid, @"type": @"end", @"error": @"Only http(s) URLs can be proxied"}];
        return;
    }
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = [m[@"method"] isKindOfClass:[NSString class]] ? m[@"method"] : @"GET";
    if ([m[@"headers"] isKindOfClass:[NSDictionary class]])
        [(NSDictionary *)m[@"headers"] enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL *stop) {
            if ([k isKindOfClass:[NSString class]] && [v isKindOfClass:[NSString class]]) [req setValue:v forHTTPHeaderField:k];
        }];
    if ([m[@"body"] isKindOfClass:[NSString class]]) req.HTTPBody = [(NSString *)m[@"body"] dataUsingEncoding:NSUTF8StringEncoding];
    NSURLSessionDataTask *task = [self.session dataTaskWithRequest:req];
    self.reqIds[@(task.taskIdentifier)] = rid;
    [task resume];
}
- (void)cancel:(NSString *)rid {
    [self.session getTasksWithCompletionHandler:^(NSArray *data, NSArray *up, NSArray *down) {
        for (NSURLSessionTask *t in data)
            if ([self.reqIds[@(t.taskIdentifier)] isEqualToString:rid]) [t cancel];
    }];
}
static BOOL aiPrivateHost(NSString *host) {
    NSString *h = host.lowercaseString ?: @"";
    if (h.length == 0 || [h isEqualToString:@"localhost"] || [h hasSuffix:@".localhost"] || [h hasSuffix:@".local"] || [h isEqualToString:@"::1"]) return YES;
    NSArray<NSString *> *p = [h componentsSeparatedByString:@"."];
    if (p.count == 4) {
        int a = p[0].intValue, b = p[1].intValue;
        if (a == 127 || a == 10 || a == 0 || (a == 169 && b == 254) || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31)) return YES;
    }
    return [h containsString:@":"] && ([h hasPrefix:@"fc"] || [h hasPrefix:@"fd"] || [h hasPrefix:@"fe80:"] || [h hasPrefix:@"::ffff:"]);
}
// Follow redirects, except from a public host onto the local machine or private network.
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response
        newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completionHandler {
    BOOL fromPublic = !aiPrivateHost(task.originalRequest.URL.host);
    completionHandler(fromPublic && aiPrivateHost(request.URL.host) ? nil : request);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    NSString *rid = self.reqIds[@(task.taskIdentifier)];
    NSHTTPURLResponse *h = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse *)response : nil;
    NSMutableDictionary *hdrs = [NSMutableDictionary dictionary];
    [h.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL *stop) {
        if ([k isKindOfClass:[NSString class]] && [v isKindOfClass:[NSString class]]) hdrs[[(NSString *)k lowercaseString]] = v;
    }];
    if (rid) [self post:@{@"id": rid, @"type": @"head", @"status": @(h ? h.statusCode : 200), @"headers": hdrs}];
    completionHandler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    NSString *rid = self.reqIds[@(task.taskIdentifier)];
    if (rid && data.length) [self post:@{@"id": rid, @"type": @"data", @"b64": [data base64EncodedStringWithOptions:0]}];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    NSString *rid = self.reqIds[@(task.taskIdentifier)];
    [self.reqIds removeObjectForKey:@(task.taskIdentifier)];
    if (!rid) return;
    if (error) [self post:@{@"id": rid, @"type": @"end", @"error": error.localizedDescription ?: @"Request failed"}];
    else [self post:@{@"id": rid, @"type": @"end"}];
}
@end

@interface HeadroomWebView : WKWebView
@property (nonatomic, assign) NSInteger dropDragSeq;
@property (nonatomic, assign) BOOL dropDragOK; // the drag holds audio files for Practice backing tracks
@end

@implementation HeadroomWebView
- (instancetype)initWithFrame:(NSRect)frame configuration:(WKWebViewConfiguration *)configuration {
    if ((self = [super initWithFrame:frame configuration:configuration])) _dropDragSeq = NSIntegerMin;
    return self;
}
- (BOOL)dropAccepts:(id<NSDraggingInfo>)info {
    if (info.draggingSequenceNumber != self.dropDragSeq) {
        self.dropDragSeq = info.draggingSequenceNumber;
        self.dropDragOK = btAudioPaths(draggedFileURLs(info)).count > 0; // audio files → Practice backing tracks
    }
    return self.dropDragOK;
}
- (void)dropOverlay:(BOOL)on {
    [self evaluateJavaScript:(on ? @"window.headroomBackingDrag && window.headroomBackingDrag(true)" : @"window.headroomBackingDrag && window.headroomBackingDrag(false)") completionHandler:nil];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    if ([self dropAccepts:info]) { [self dropOverlay:YES]; return NSDragOperationCopy; }
    return [WKWebView instancesRespondToSelector:_cmd] ? [super draggingEntered:info] : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    if ([self dropAccepts:info]) return NSDragOperationCopy;
    return [WKWebView instancesRespondToSelector:_cmd] ? [super draggingUpdated:info] : NSDragOperationNone;
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
    if (info && [self dropAccepts:info]) { [self dropOverlay:NO]; return; }
    if ([WKWebView instancesRespondToSelector:_cmd]) [super draggingExited:info];
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info {
    if ([self dropAccepts:info]) return YES;
    return [WKWebView instancesRespondToSelector:_cmd] ? [super prepareForDragOperation:info] : NO;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    if (![self dropAccepts:info]) return [WKWebView instancesRespondToSelector:_cmd] ? [super performDragOperation:info] : NO;
    [self dropOverlay:NO];
    [self evaluateJavaScript:btImportJS(btAudioPaths(draggedFileURLs(info))) completionHandler:nil]; // Practice backing tracks
    return YES;
}
- (void)concludeDragOperation:(id<NSDraggingInfo>)info {
    if (info && [self dropAccepts:info]) return;
    if ([WKWebView instancesRespondToSelector:_cmd]) [super concludeDragOperation:info];
}
@end

@interface StudioAppDelegate : NSObject <NSApplicationDelegate, WKNavigationDelegate, WKScriptMessageHandler, NSOpenSavePanelDelegate> {
    std::unique_ptr<rig::audio::RealtimeAudioServer> _audioServer;
}
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, copy) NSString *screenshotPath;
@property (nonatomic, copy) NSString *screenshotJS;   // --screenshot-js: run in the page before the snapshot (UI checks)
@property (nonatomic, strong) HRAiFetchProxy *aiProxy;
@end

@implementation StudioAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    // If launching an interactive session, gracefully ask any prior rig_studio instance to quit via Cocoa NSRunningApplication
    bool isScreenshot = (self.screenshotPath.length > 0);
    // A test instance (HEADROOM_PORT set) runs beside the main app and must never quit it.
    if (!isScreenshot && !getenv("HEADROOM_PORT")) {
        pid_t myPid = getpid();
        for (NSRunningApplication *app in [[NSWorkspace sharedWorkspace] runningApplications]) {
            if (app.processIdentifier != myPid &&
                ([app.localizedName isEqualToString:@"rig_studio"] || [app.localizedName isEqualToString:@"Headroom"] ||
                 [app.executableURL.lastPathComponent isEqualToString:@"rig_studio"] ||
                 [app.executableURL.lastPathComponent isEqualToString:@"Headroom"])) {
                [app terminate];
            }
        }
        usleep(350000);
    }

    NSString *htmlPath = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"ui/index.html"];
#ifdef RIG_APP_SOURCE_DIR
    NSString *devPage = [@RIG_APP_SOURCE_DIR stringByAppendingPathComponent:@"ui/index.html"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:devPage]) {
        htmlPath = devPage;
    } else if (![[NSFileManager defaultManager] fileExistsAtPath:htmlPath]) {
        NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        htmlPath = [[exeDir stringByAppendingPathComponent:@"../ui/index.html"] stringByStandardizingPath];
        if (![[NSFileManager defaultManager] fileExistsAtPath:htmlPath])
            htmlPath = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:@"app/ui/index.html"];
    }
#endif
    NSString *appDir = [htmlPath stringByDeletingLastPathComponent];
    appDir = [appDir stringByDeletingLastPathComponent]; // `.../app`
    NSString *repoRoot = [appDir stringByDeletingLastPathComponent]; // repository root

    rig::plugins::installAppEditorHooks(); // hosted plugin editors open as native windows
    _audioServer = std::make_unique<rig::audio::RealtimeAudioServer>();
    // HEADROOM_PORT / HEADROOM_NO_AUDIO let a second (test) instance run beside the main app without
    // fighting over the engine port or the audio device.
    bool enableAudio = !isScreenshot && !getenv("HEADROOM_NO_AUDIO");
    uint16_t boundPort = isScreenshot ? 17771 : 17770;
    if (const char* p = getenv("HEADROOM_PORT")) boundPort = static_cast<uint16_t>(atoi(p));
    if (!_audioServer->start([repoRoot UTF8String], boundPort, enableAudio)) {
        boundPort = (boundPort == 17770) ? 17771 : 17772;
        _audioServer->start([repoRoot UTF8String], boundPort, enableAudio);
    }

    NSRect frame = NSMakeRect(0, 0, 1600, 1000);
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable |
                              NSWindowStyleMaskFullSizeContentView;

    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Headroom";
    self.window.titlebarAppearsTransparent = YES;
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.backgroundColor = [NSColor colorWithSRGBRed:0.08 green:0.08 blue:0.09 alpha:1.0];
    [self.window center];

    WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
    // A side-by-side smoke with an isolated engine profile also needs isolated WebKit storage.
    if (getenv("HEADROOM_PORT") && getenv("HEADROOM_DATA_DIR"))
        config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    // Screenshot runs snapshot the app ~1 s after load, so they skip the boot splash.
    // Local developer tools (curl, rig scripts) read "<port> <token>" from <app support>/api-token, readable by this
    // user only (0600). A web page cannot read local files, so this does not reopen the cross-origin hole.
    {
        const std::string tokPath = rig::library::appSupportDir() + "/api-token";
        const int tfd = ::open(tokPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (tfd >= 0) {
            ::fchmod(tfd, 0600);
            const std::string line = std::to_string(boundPort) + " " + _audioServer->apiToken() + "\n";
            (void)::write(tfd, line.data(), line.size());
            ::close(tfd);
        }
    }
    NSString *portScriptSrc = [NSString stringWithFormat:@"window.RIG_PORT = %u; if(location.protocol==='file:') window.HEADROOM_API_TOKEN='%s';%@", boundPort, _audioServer->apiToken().c_str(), isScreenshot ? @" window.RIG_NO_SPLASH = true;" : @""];
    WKUserScript *portScript = [[WKUserScript alloc] initWithSource:portScriptSrc
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                   forMainFrameOnly:YES];
    [config.userContentController addUserScript:portScript];
    // Forward page errors to the app log (Console.app / stderr), since a release WKWebView has no inspector.
    WKUserScript *errScript = [[WKUserScript alloc] initWithSource:
        @"(()=>{const post=(m)=>{try{window.webkit.messageHandlers.headroom.postMessage({action:'jslog',msg:String(m).slice(0,2000)})}catch(_){}};"
         "addEventListener('error',(e)=>post('error: '+e.message+' @ '+(e.filename||'').split('/').pop()+':'+e.lineno+':'+e.colno));"
         "addEventListener('unhandledrejection',(e)=>post('unhandled rejection: '+((e.reason&&(e.reason.stack||e.reason.message))||e.reason)));"
         "const ce=console.error.bind(console);console.error=(...a)=>{post('console.error: '+a.map(String).join(' '));ce(...a)};})();"
                                                     injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                  forMainFrameOnly:YES];
    [config.userContentController addUserScript:errScript];
    // Native bridge: window.webkit.messageHandlers.headroom.postMessage({action: "importNam" | "revealNamFolder" | "revealRig" | "revealRigsFolder" | "openPluginEditor", instance, path})
    // Guitar Tech: "getSecret" | "setSecret" | "deleteSecret" ({id, account, value}) -> window.headroomSecretReply({id, ok, found, value?, error?});
    // "aiFetch" / "aiFetchCancel" -> window.headroomAiFetch(...) (HRAiFetchProxy). "openURL" ({url}) -> default browser (http/https only).
    // Practice: "importBackingTracks" (audio picker -> window.headroomBackingImport(paths)) | "revealBackingFolder".
    [config.userContentController addScriptMessageHandler:self name:@"headroom"];

    self.webView = [[HeadroomWebView alloc] initWithFrame:self.window.contentView.bounds configuration:config];
    [self.webView registerForDraggedTypes:[self.webView.registeredDraggedTypes arrayByAddingObject:NSPasteboardTypeFileURL]];
    self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.webView.navigationDelegate = self;
    [self.window.contentView addSubview:self.webView];

    NSURL *fileURL = [NSURL fileURLWithPath:htmlPath];
    NSURL *appRootURL = [NSURL fileURLWithPath:appDir];
    [self.webView loadFileURL:fileURL allowingReadAccessToURL:appRootURL];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    if (![message.body isKindOfClass:[NSDictionary class]]) return;
    NSString *action = ((NSDictionary *)message.body)[@"action"];
    if ([action isEqualToString:@"jslog"]) {
        NSLog(@"[ui] %@", ((NSDictionary *)message.body)[@"msg"]);
        return;
    }
    if ([action isEqualToString:@"getSecret"] || [action isEqualToString:@"setSecret"] || [action isEqualToString:@"deleteSecret"]) {
        NSDictionary *m = (NSDictionary *)message.body;
        id rid = m[@"id"];
        NSString *account = aiValidAccount(m[@"account"]) ? m[@"account"] : nil;
        NSString *value = [m[@"value"] isKindOfClass:[NSString class]] ? m[@"value"] : nil;
        // Keychain calls can block (an access prompt), so they run off the main thread.
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSMutableDictionary *reply = [NSMutableDictionary dictionaryWithObject:(rid ?: [NSNull null]) forKey:@"id"];
            OSStatus st = errSecParam;
            if (account && [action isEqualToString:@"getSecret"]) {
                NSString *v = aiSecretGet(account, &st);
                reply[@"found"] = @(v != nil);
                if (v) reply[@"value"] = v;
                if (st == errSecItemNotFound) st = errSecSuccess;
            } else if (account && [action isEqualToString:@"setSecret"] && value.length) {
                st = aiSecretSet(account, value);
            } else if (account && [action isEqualToString:@"deleteSecret"]) {
                st = SecItemDelete((__bridge CFDictionaryRef)aiSecretQuery(account));
                if (st == errSecItemNotFound) st = errSecSuccess;
            }
            reply[@"ok"] = @(st == errSecSuccess);
            if (st != errSecSuccess) {
                NSString *msg = (__bridge_transfer NSString *)SecCopyErrorMessageString(st, NULL);
                reply[@"error"] = msg ?: [NSString stringWithFormat:@"Keychain error %d", (int)st];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [self.webView evaluateJavaScript:aiJSCall(@"headroomSecretReply", reply) completionHandler:nil];
            });
        });
        return;
    }
    if ([action isEqualToString:@"aiFetch"]) {
        if (!self.aiProxy) self.aiProxy = [[HRAiFetchProxy alloc] initWithWebView:self.webView];
        [self.aiProxy start:(NSDictionary *)message.body];
        return;
    }
    if ([action isEqualToString:@"aiFetchCancel"]) {
        id rid = ((NSDictionary *)message.body)[@"id"];
        if (self.aiProxy && [rid isKindOfClass:[NSString class]]) [self.aiProxy cancel:rid];
        return;
    }
    if ([action isEqualToString:@"openURL"]) { // Guitar Tech citations/links: open in the default browser, never in the app's web view
        id u = ((NSDictionary *)message.body)[@"url"];
        NSURL *url = [u isKindOfClass:[NSString class]] ? [NSURL URLWithString:u] : nil;
        NSString *scheme = url.scheme.lowercaseString;
        if (url && url.host.length && ([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"]))
            [[NSWorkspace sharedWorkspace] openURL:url];
        return;
    }
    if ([action isEqualToString:@"importBackingTracks"]) { // Practice: audio files -> JS posts them to /api/backing/import
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = YES;
        panel.allowedContentTypes = @[UTTypeAudio];
        panel.message = @"Choose backing tracks to practise with";
        panel.prompt = @"Add";
        [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
            if (result != NSModalResponseOK) return;
            NSMutableArray<NSString *> *paths = [NSMutableArray array];
            for (NSURL *u in panel.URLs) [paths addObject:u.path];
            [self.webView evaluateJavaScript:btImportJS(paths) completionHandler:nil];
        }];
        return;
    }
    if ([action isEqualToString:@"revealBackingFolder"]) {
        NSString *dir = [NSString stringWithUTF8String:rig::audio::BackingTracks::defaultDir().c_str()];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir]];
        return;
    }
    if ([action isEqualToString:@"openPluginEditor"]) { // {action, instance}: a hosted plugin's own UI
        id inst = ((NSDictionary *)message.body)[@"instance"];
        if ([inst respondsToSelector:@selector(intValue)]) rig::plugins::showEditorOnMainThread([inst intValue]);
        return;
    }
    NSString *userRigsFolder = [NSString stringWithUTF8String:(rig::library::appSupportDir() + "/User Presets").c_str()];
    if ([action isEqualToString:@"revealRigsFolder"] || [action isEqualToString:@"revealUserPresetsFolder"]) {
        [[NSFileManager defaultManager] createDirectoryAtPath:userRigsFolder withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:userRigsFolder]];
        return;
    }
    if ([action isEqualToString:@"revealRig"] || [action isEqualToString:@"revealPreset"]) {
        id p = ((NSDictionary *)message.body)[@"path"];
        id nm = ((NSDictionary *)message.body)[@"name"];
        NSString *rigPath = [p isKindOfClass:[NSString class]] ? (NSString *)p : nil;
        if ((!rigPath || rigPath.length == 0) && [nm isKindOfClass:[NSString class]]) {
            std::string san = rig::library::sanitizeFileName([(NSString *)nm UTF8String]);
            rigPath = [userRigsFolder stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.rig", san.c_str()]];
        }
        if (rigPath && [[NSFileManager defaultManager] fileExistsAtPath:rigPath]) {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:rigPath]]];
        } else {
            [[NSFileManager defaultManager] createDirectoryAtPath:userRigsFolder withIntermediateDirectories:YES attributes:nil error:nil];
            [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:userRigsFolder]];
        }
        return;
    }
    NSString *folder = [NSString stringWithUTF8String:rig::namCaptureFolder().c_str()];
    if ([action isEqualToString:@"revealNamFolder"]) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:folder]];
        return;
    }
    if ([action isEqualToString:@"importNam"]) {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = YES;
        panel.message = @"Choose Neural Amp Modeler captures (.nam files or folders of them)";
        panel.prompt = @"Import";
        [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
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
            [self.webView evaluateJavaScript:js completionHandler:nil];
        }];
        return;
    }
    NSString *irFolder = [NSString stringWithUTF8String:rig::userIrFolder().c_str()];
    if ([action isEqualToString:@"revealIrFolder"]) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:irFolder]];
        return;
    }
    if ([action isEqualToString:@"importIr"]) {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = YES;
        panel.message = @"Choose speaker cabinet Impulse Responses (.wav files or folders)";
        panel.prompt = @"Import IRs";
        [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
            if (result != NSModalResponseOK) return;
            NSFileManager *fm = [NSFileManager defaultManager];
            NSInteger copied = 0;
            for (NSURL *url in panel.URLs) {
                BOOL isDir = NO;
                [fm fileExistsAtPath:url.path isDirectory:&isDir];
                NSArray<NSURL *> *files = isDir ? [[fm enumeratorAtURL:url includingPropertiesForKeys:nil options:0 errorHandler:nil] allObjects] : @[url];
                NSString *destDir = isDir ? [irFolder stringByAppendingPathComponent:url.lastPathComponent] : irFolder;
                [fm createDirectoryAtPath:destDir withIntermediateDirectories:YES attributes:nil error:nil];
                for (NSURL *f in files) {
                    if (![[f.pathExtension lowercaseString] isEqualToString:@"wav"]) continue;
                    NSString *dst = [destDir stringByAppendingPathComponent:f.lastPathComponent];
                    [fm removeItemAtPath:dst error:nil];
                    if ([fm copyItemAtPath:f.path toPath:dst error:nil]) ++copied;
                }
            }
            rig::rescanUserIrs();
            NSString *js = [NSString stringWithFormat:@"window.headroomIrImported && window.headroomIrImported(%ld)", (long)copied];
            [self.webView evaluateJavaScript:js completionHandler:nil];
        }];
    }
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    if (_audioServer) {
        _audioServer->stop();
        _audioServer.reset();
    }
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    if (self.screenshotPath.length > 0) {
        if (self.screenshotJS.length > 0) [self.webView evaluateJavaScript:self.screenshotJS completionHandler:nil];
        const double delay = self.screenshotJS.length > 0 ? 4.0 : 0.95;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            WKSnapshotConfiguration *snapCfg = [[WKSnapshotConfiguration alloc] init];
            snapCfg.rect = NSMakeRect(0, 0, 1600, 1000);
            [self.webView takeSnapshotWithConfiguration:snapCfg completionHandler:^(NSImage * _Nullable snapshotImage, NSError * _Nullable error) {
                if (snapshotImage && !error) {
                    CGImageRef cgImg = [snapshotImage CGImageForProposedRect:NULL context:nil hints:nil];
                    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cgImg];
                    NSData *pngData = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                    [pngData writeToFile:self.screenshotPath atomically:YES];
                }
                [NSApp terminate:nil];
            }];
        });
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Standard menus so Cmd+Q / Cmd+C / Cmd+V / Cmd+A work inside the web UI's text fields.
        NSMenu *bar = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [bar addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Headroom"];
        [appMenu addItemWithTitle:@"About Headroom" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Hide Headroom" action:@selector(hide:) keyEquivalent:@"h"];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Quit Headroom" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        NSMenuItem *editItem = [[NSMenuItem alloc] init];
        [bar addItem:editItem];
        NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
        [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
        [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
        [edit addItem:[NSMenuItem separatorItem]];
        [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        editItem.submenu = edit;
        NSMenuItem *winItem = [[NSMenuItem alloc] init];
        [bar addItem:winItem];
        NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
        [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [win addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        winItem.submenu = win;
        app.mainMenu = bar;

        StudioAppDelegate *delegate = [[StudioAppDelegate alloc] init];
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--screenshot-js") == 0 && i + 1 < argc) {
                delegate.screenshotJS = [NSString stringWithUTF8String:argv[i + 1]];
            }
            if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
                delegate.screenshotPath = [NSString stringWithUTF8String:argv[i + 1]];
            }
        }

        app.delegate = delegate;
        [app run];
    }
    return 0;
}
