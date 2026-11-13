// Encrypted storage for secrets: Wi-Fi, SSH and WebDAV passwords and SSH private
// keys. They are encrypted with a key protected by the user's passphrase, which is
// asked for at boot (see lib/vault for the scheme). Until then they cannot be read.
//
// Use get()/put() instead of storage::getStr()/putStr() for secret settings, and
// readFile()/writeFile() instead of the storage:: ones for secret files.
#pragma once
#include <string>

namespace secrets {

constexpr size_t kMinPassphrase = 6;

bool isSetUp();   // a passphrase has been chosen
bool unlocked();  // the secrets can be read

// Chooses the first passphrase and encrypts all existing secrets with it.
bool setUp(const std::string& pass);
// False: wrong passphrase, or tried while waitSeconds() > 0.
bool unlock(const std::string& pass);

// Brute force protection: wrong passphrases in a row are counted (kept across
// reboots, counted before each check so cutting the power does not help), and
// after a few the next try has to wait. A reboot starts the wait over.
uint32_t failedAttempts();
// Seconds until the passphrase may be tried again (0 = now).
uint32_t waitSeconds();

// False: `oldPass` is wrong (counts as a failed attempt), tried while
// waitSeconds() > 0, or the new record could not be saved.
bool changePassphrase(const std::string& oldPass, const std::string& newPass);
// Forgotten passphrase: deletes all secrets (and the SSH key pair) and the
// passphrase. setUp() must be called again afterwards.
void erase();

// Secret settings (NVS keys). get() returns "" while locked; put() does nothing
// while locked (secrets are never stored in plain text).
std::string get(const char* key);
void put(const char* key, const std::string& value);

// Secret files on internal storage.
bool readFile(const char* path, std::string& out);
bool writeFile(const char* path, const std::string& data);

// Dialog: asks for a new passphrase twice. False if cancelled.
bool askNewPassphrase(const char* title, std::string& out);

}  // namespace secrets
