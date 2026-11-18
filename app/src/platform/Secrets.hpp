#pragma once
// Per-user secret storage for Guitar Tech AI provider keys, one generic password per account under the
// service "com.headroom.studio.ai" (the same service the macOS Keychain code in StudioApp.mm uses).
//
//   macOS   Keychain (still implemented in StudioApp.mm; moving it here is a mechanical lift of
//           aiSecretGet/aiSecretSet/SecItemDelete into SecretsMac.mm behind these functions)
//   Windows Credential Manager (CredReadW / CredWriteW / CredDeleteW, CRED_TYPE_GENERIC,
//           target "com.headroom.studio.ai/<account>", CRED_PERSIST_LOCAL_MACHINE)
//   Linux   libsecret (Secret Service: GNOME Keyring, KWallet ≥ 5.97, KeePassXC) when built with
//           RIG_HAVE_LIBSECRET and a service is running; otherwise every call fails with a clear error.
//
// Calls can block (an unlock prompt): never call them on the UI or audio thread.
#include <string>

namespace rig::platform {

struct SecretResult {
    bool ok = false;
    bool found = false;
    std::string value;
    std::string error;
};

inline constexpr const char* kSecretService = "com.headroom.studio.ai";

bool secretsAvailable();
SecretResult secretGet(const std::string& account);
SecretResult secretSet(const std::string& account, const std::string& value);
SecretResult secretDelete(const std::string& account); // ok when it didn't exist

} // namespace rig::platform
