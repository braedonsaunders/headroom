// Linux side of the portable shell: GTK 3 + WebKitGTK 4.1 (the toolkit webview/webview uses on Linux).
#include "Shell.hpp"
#include "../../platform/Paths.hpp"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace rig::shell {

namespace {

// Runs a command without a shell (argv form), detached: xdg-open never blocks the UI.
void spawn(std::initializer_list<const char*> argv_in) {
    std::vector<gchar*> argv;
    for (const char* a : argv_in) argv.push_back(const_cast<gchar*>(a));
    argv.push_back(nullptr);
    GError* e = nullptr;
    if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &e)) {
        std::fprintf(stderr, "[shell] %s failed: %s\n", argv[0], e ? e->message : "?");
        if (e) g_error_free(e);
    }
}

class LinuxPlatform final : public Platform {
public:
    explicit LinuxPlatform(GtkWindow* w) : window_(w) {}

    void openUrl(const std::string& url) override { spawn({"xdg-open", url.c_str()}); }
    void openFolder(const std::string& path) override { spawn({"xdg-open", path.c_str()}); }
    void revealFile(const std::string& path) override {
        // org.freedesktop.FileManager1.ShowItems selects the file in Nautilus / Dolphin / Nemo; fall back to the folder.
        gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        bool done = false;
        if (uri) {
            GError* e = nullptr;
            GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
            if (bus) {
                const gchar* uris[] = {uri, nullptr};
                GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                          "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""),
                                                          nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &e);
                if (r) { g_variant_unref(r); done = true; }
                g_object_unref(bus);
            }
            if (e) g_error_free(e);
            g_free(uri);
        }
        if (!done) openFolder(std::filesystem::path(path).parent_path().string());
    }

    void pickPaths(const PickOptions& o, std::function<void(std::vector<std::string>)> done) override {
        // GTK 3's chooser can't pick files and folders in one dialog: folders-only when files aren't wanted,
        // otherwise files (folder imports go through "Select folder" below, offered as an extra button).
        const bool both = o.files && o.folders;
        GtkFileChooserAction act = o.files ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
        GtkWidget* dlg = gtk_file_chooser_dialog_new(o.title.c_str(), window_, act, "_Cancel", GTK_RESPONSE_CANCEL,
                                                     o.accept_label.empty() ? "_Open" : o.accept_label.c_str(), GTK_RESPONSE_ACCEPT, nullptr);
        if (both) gtk_dialog_add_button(GTK_DIALOG(dlg), "Use this _folder", 2);
        GtkFileChooser* fc = GTK_FILE_CHOOSER(dlg);
        gtk_file_chooser_set_select_multiple(fc, o.multiple);
        gtk_file_chooser_set_local_only(fc, TRUE);
        if (!o.start_dir.empty()) gtk_file_chooser_set_current_folder(fc, o.start_dir.c_str());
        if (o.files && !o.extensions.empty()) {
            GtkFileFilter* f = gtk_file_filter_new();
            gtk_file_filter_set_name(f, o.filter_name.empty() ? "Supported files" : o.filter_name.c_str());
            for (const auto& e : o.extensions) {
                std::string up = e;
                std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                gtk_file_filter_add_pattern(f, ("*." + e).c_str());
                gtk_file_filter_add_pattern(f, ("*." + up).c_str());
            }
            gtk_file_chooser_add_filter(fc, f);
        }
        const gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        std::vector<std::string> out;
        if (resp == GTK_RESPONSE_ACCEPT) {
            GSList* list = gtk_file_chooser_get_filenames(fc);
            for (GSList* l = list; l; l = l->next) { out.emplace_back(static_cast<char*>(l->data)); g_free(l->data); }
            g_slist_free(list);
        } else if (resp == 2) {
            if (gchar* cur = gtk_file_chooser_get_current_folder(fc)) { out.emplace_back(cur); g_free(cur); }
        }
        gtk_widget_destroy(dlg);
        done(std::move(out));
    }

private:
    GtkWindow* window_;
};

int g_lock_fd = -1;

} // namespace

std::unique_ptr<Platform> makePlatform(void* window, void*) {
    return std::make_unique<LinuxPlatform>(static_cast<GtkWindow*>(window));
}

bool acquireSingleInstance() {
    std::error_code ec;
    const std::string dir = platform::appDataDir();
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/.headroom-instance.lock";
    g_lock_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_lock_fd < 0) return true; // can't lock: don't refuse to start
    if (::flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(g_lock_fd);
        g_lock_fd = -1;
        return errno != EWOULDBLOCK;
    }
    return true; // held until the process exits
}

void configureBrowser(void* browser, bool debug) {
    auto* wv = WEBKIT_WEB_VIEW(browser);
    WebKitSettings* s = webkit_web_view_get_settings(wv);
    // The UI is loaded from file:// and reads its own assets with fetch(); the engine API is CORS-open.
    webkit_settings_set_allow_file_access_from_file_urls(s, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(s, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(s, debug);
    webkit_settings_set_enable_developer_extras(s, debug);
    webkit_settings_set_media_playback_requires_user_gesture(s, FALSE);
    webkit_settings_set_hardware_acceleration_policy(s, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
    GdkRGBA bg{0.08, 0.08, 0.09, 1.0}; // matches the macOS window background
    webkit_web_view_set_background_color(wv, &bg);
}

namespace {
struct HandlerCtx { std::function<void(const std::string&)> fn; };
void onScriptMessage(WebKitUserContentManager*, WebKitJavascriptResult* r, gpointer user) {
    JSCValue* v = webkit_javascript_result_get_js_value(r);
    gchar* j = jsc_value_to_json(v, 0);
    if (j) {
        static_cast<HandlerCtx*>(user)->fn(j);
        g_free(j);
    }
}
} // namespace

bool installNativeMessageHandler(void* browser, std::function<void(const std::string&)> on_json) {
    auto* wv = WEBKIT_WEB_VIEW(browser);
    WebKitUserContentManager* m = webkit_web_view_get_user_content_manager(wv);
    auto* ctx = new HandlerCtx{std::move(on_json)}; // lives as long as the window
    g_signal_connect(m, "script-message-received::headroom", G_CALLBACK(onScriptMessage), ctx);
    return webkit_user_content_manager_register_script_message_handler(m, "headroom");
}

namespace {
struct SnapCtx { std::string path; std::function<void(bool)> done; };
void onSnapshot(GObject* src, GAsyncResult* res, gpointer user) {
    auto* c = static_cast<SnapCtx*>(user);
    GError* e = nullptr;
    cairo_surface_t* surf = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src), res, &e);
    bool ok = false;
    if (surf) {
        ok = cairo_surface_write_to_png(surf, c->path.c_str()) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(surf);
    }
    if (e) { std::fprintf(stderr, "[shell] snapshot failed: %s\n", e->message); g_error_free(e); }
    c->done(ok);
    delete c;
}
} // namespace

bool snapshotPng(void* browser, const std::string& path, std::function<void(bool)> done) {
    webkit_web_view_get_snapshot(WEBKIT_WEB_VIEW(browser), WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
                                 onSnapshot, new SnapCtx{path, std::move(done)});
    return true;
}

std::string executableDir() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string(".") : p.parent_path().string();
}

namespace {
gboolean onQuitSignal(gpointer user) {
    (*static_cast<std::function<void()>*>(user))();
    return G_SOURCE_CONTINUE;
}
} // namespace

void installQuitHandler(std::function<void()> quit) {
    auto* fn = new std::function<void()>(std::move(quit)); // process lifetime
    g_unix_signal_add(SIGINT, onQuitSignal, fn);
    g_unix_signal_add(SIGTERM, onQuitSignal, fn);
}

} // namespace rig::shell
