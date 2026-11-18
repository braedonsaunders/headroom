// Windows / Linux implementations of Secrets.hpp (macOS: Keychain, in StudioApp.mm for now).
#include "Secrets.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#pragma comment(lib, "Advapi32.lib")
#elif defined(RIG_HAVE_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace rig::platform {

#if defined(_WIN32)
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring target(const std::string& account) { return wide(std::string(kSecretService) + "/" + account); }
std::string lastError(const char* what) { return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")"; }
} // namespace

bool secretsAvailable() { return true; }

SecretResult secretGet(const std::string& account) {
    SecretResult r;
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(account).c_str(), CRED_TYPE_GENERIC, 0, &c)) {
        if (GetLastError() == ERROR_NOT_FOUND) r.ok = true;
        else r.error = lastError("Credential Manager read failed");
        return r;
    }
    r.ok = r.found = true;
    r.value.assign(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c);
    return r;
}

SecretResult secretSet(const std::string& account, const std::string& value) {
    SecretResult r;
    std::wstring t = target(account), user = wide(account), label = wide("Headroom Guitar Tech key (" + account + ")");
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t.data();
    c.Comment = label.data();
    c.UserName = user.data();
    c.CredentialBlobSize = static_cast<DWORD>(value.size());
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (CredWriteW(&c, 0)) r.ok = true;
    else r.error = lastError("Credential Manager write failed");
    return r;
}

SecretResult secretDelete(const std::string& account) {
    SecretResult r;
    if (CredDeleteW(target(account).c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) r.ok = true;
    else r.error = lastError("Credential Manager delete failed");
    return r;
}

#elif defined(RIG_HAVE_LIBSECRET)
namespace {
const SecretSchema* schema() {
    static const SecretSchema s = {kSecretService, SECRET_SCHEMA_NONE,
                                   {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
                                   0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &s;
}
std::string take(GError* e, const char* what) {
    std::string m = std::string(what) + (e && e->message ? std::string(": ") + e->message : std::string());
    if (e) g_error_free(e);
    return m;
}
} // namespace

bool secretsAvailable() { return true; }

SecretResult secretGet(const std::string& account) {
    SecretResult r;
    GError* e = nullptr;
    gchar* pw = secret_password_lookup_sync(schema(), nullptr, &e, "account", account.c_str(), nullptr);
    if (e) { r.error = take(e, "Secret Service lookup failed"); return r; }
    r.ok = true;
    if (pw) { r.found = true; r.value = pw; secret_password_free(pw); }
    return r;
}

SecretResult secretSet(const std::string& account, const std::string& value) {
    SecretResult r;
    GError* e = nullptr;
    const std::string label = "Headroom Guitar Tech key (" + account + ")";
    if (secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(), nullptr, &e,
                                   "account", account.c_str(), nullptr)) r.ok = true;
    else r.error = take(e, "Secret Service store failed");
    return r;
}

SecretResult secretDelete(const std::string& account) {
    SecretResult r;
    GError* e = nullptr;
    secret_password_clear_sync(schema(), nullptr, &e, "account", account.c_str(), nullptr);
    if (e) r.error = take(e, "Secret Service delete failed");
    else r.ok = true;
    return r;
}

#else
namespace {
SecretResult unavailable() {
    return {false, false, {}, "No secret store on this system (install a Secret Service such as GNOME Keyring or KWallet)"};
}
} // namespace
bool secretsAvailable() { return false; }
SecretResult secretGet(const std::string&) { return unavailable(); }
SecretResult secretSet(const std::string&, const std::string&) { return unavailable(); }
SecretResult secretDelete(const std::string&) { return unavailable(); }
#endif

} // namespace rig::platform
