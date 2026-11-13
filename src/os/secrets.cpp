#include "secrets.h"

#include <Arduino.h>
#include <WiFi.h>
#include <bootloader_random.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "storage.h"
#include "ui.h"
#include "vault.h"

namespace secrets {

namespace {

// Settings keys and files that hold secrets. Anything listed here that is still
// in plain text (from before encryption existed) is encrypted on unlock.
const char* const kKeys[] = {"wifi", "ssh_hosts", "dav_pass", "jr_pass"};
const char* const kFiles[] = {"/ssh/id_ed25519", "/ssh/id_ecdsa", "/ssh/id_rsa"};
const char* const kEraseAlso[] = {"/ssh/id_ed25519.pub"};  // useless without its private key
constexpr const char* kRecordKey = "vault";
constexpr const char* kFailKey = "vault_fail";  // wrong passphrases in a row
// PBKDF2 rounds: makes guessing the passphrase from a copy of the flash slow,
// while unlocking takes about a second or two.
constexpr uint32_t kIterations = 30000;

class EspCrypto : public vault::Crypto {
public:
    void random(uint8_t* out, size_t n) override {
        // The hardware RNG is only truly random while a radio is on; otherwise
        // switch on the internal noise source for the moment.
        const bool radioOff = WiFi.getMode() == WIFI_OFF && !btStarted();
        if (radioOff) bootloader_random_enable();
        esp_fill_random(out, n);
        if (radioOff) bootloader_random_disable();
    }

    bool deriveKey(const std::string& pass, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                   vault::Key& out) override {
        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        bool ok = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
                  mbedtls_pkcs5_pbkdf2_hmac(&md, (const unsigned char*)pass.data(), pass.size(), salt, saltLen,
                                            iterations, out.size(), out.data()) == 0;
        mbedtls_md_free(&md);
        return ok;
    }

    bool seal(const vault::Key& key, const uint8_t* nonce, const std::string& aad, const std::string& plain,
              std::string& out) override {
        out.assign(plain.size() + vault::kTagLen, '\0');
        uint8_t* buf = (uint8_t*)&out[0];
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.data(), 256) == 0 &&
                        mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain.size(), nonce, vault::kNonceLen,
                                                  (const uint8_t*)aad.data(), aad.size(),
                                                  (const uint8_t*)plain.data(), buf, vault::kTagLen,
                                                  buf + plain.size()) == 0;
        mbedtls_gcm_free(&gcm);
        if (!ok) out.clear();
        return ok;
    }

    bool open(const vault::Key& key, const uint8_t* nonce, const std::string& aad, const std::string& sealed,
              std::string& plain) override {
        if (sealed.size() < vault::kTagLen) return false;
        const size_t n = sealed.size() - vault::kTagLen;
        plain.assign(n, '\0');
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        const uint8_t* in = (const uint8_t*)sealed.data();
        const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.data(), 256) == 0 &&
                        mbedtls_gcm_auth_decrypt(&gcm, n, nonce, vault::kNonceLen, (const uint8_t*)aad.data(),
                                                 aad.size(), in + n, vault::kTagLen, in,
                                                 n ? (uint8_t*)&plain[0] : nullptr) == 0;
        mbedtls_gcm_free(&gcm);
        if (!ok) vault::wipe(plain);
        return ok;
    }
};

EspCrypto crypto;
vault::Vault theVault(crypto);

// When the current wait started (millis); a reboot starts it over.
uint32_t waitStart = 0;
bool waitStarted = false;

// Checks `pass` against the stored record with brute force accounting. On
// success the data key is in `v`.
bool tryPassphrase(vault::Vault& v, const std::string& pass) {
    if (waitSeconds() > 0) return false;
    const uint32_t failures = failedAttempts();
    storage::putInt(kFailKey, (int)(failures + 1));  // before the check: a power cut counts as a failure
    const bool ok = v.unlock(storage::getStr(kRecordKey), pass);
    if (ok) {
        storage::putInt(kFailKey, 0);
        waitStarted = false;
    } else {
        waitStart = millis();
        waitStarted = true;
    }
    return ok;
}

// Encrypts whatever secrets are still stored in plain text: everything right
// after the first setUp(), and anything left over if that was interrupted.
void sealLeftovers() {
    for (const char* k : kKeys) {
        std::string v = storage::getStr(k);
        if (!v.empty() && !vault::Vault::isSealed(v)) put(k, v);
        vault::wipe(v);
    }
    for (const char* f : kFiles) {
        std::string data;
        if (storage::readFile(f, data) && !data.empty() && !vault::Vault::isSealed(data)) writeFile(f, data);
        vault::wipe(data);
    }
}

}  // namespace

bool isSetUp() { return !storage::getStr(kRecordKey).empty(); }
bool unlocked() { return theVault.unlocked(); }

bool setUp(const std::string& pass) {
    if (pass.size() < kMinPassphrase) return false;
    const std::string record = theVault.create(pass, kIterations);
    if (record.empty()) return false;
    // the record first: if power fails while sealing, the rest is done on the next unlock
    storage::putStr(kRecordKey, record);
    sealLeftovers();
    return true;
}

uint32_t failedAttempts() {
    const int n = storage::getInt(kFailKey, 0);
    return n > 0 ? (uint32_t)n : 0;
}

uint32_t waitSeconds() {
    const uint32_t lockout = vault::lockoutSeconds(failedAttempts());
    if (lockout == 0) return 0;
    if (!waitStarted) {  // first look after boot: wait the full time
        waitStart = millis();
        waitStarted = true;
    }
    const uint32_t elapsed = (millis() - waitStart) / 1000;
    return elapsed >= lockout ? 0 : lockout - elapsed;
}

bool unlock(const std::string& pass) {
    if (!tryPassphrase(theVault, pass)) return false;
    sealLeftovers();
    return true;
}

bool changePassphrase(const std::string& oldPass, const std::string& newPass) {
    if (newPass.size() < kMinPassphrase) return false;
    // check the old passphrase on a separate vault, so a typo does not lock this one
    vault::Vault check(crypto);
    if (!tryPassphrase(check, oldPass)) return false;
    const std::string record = theVault.rewrap(newPass, kIterations);
    if (record.empty()) return false;
    storage::putStr(kRecordKey, record);
    return storage::getStr(kRecordKey) == record;
}

void erase() {
    theVault.lock();
    for (const char* k : kKeys) storage::putStr(k, "");
    for (const char* f : kFiles) storage::remove(f);
    for (const char* f : kEraseAlso) storage::remove(f);
    storage::putStr(kRecordKey, "");
    storage::putInt(kFailKey, 0);
    waitStarted = false;
}

std::string get(const char* key) {
    std::string raw = storage::getStr(key);
    if (raw.empty() || !theVault.unlocked()) return std::string();
    if (!vault::Vault::isSealed(raw)) return raw;  // not encrypted yet (sealLeftovers will)
    std::string plain;
    if (!theVault.open(key, raw, plain)) log_e("secret %s could not be decrypted", key);
    return plain;
}

void put(const char* key, const std::string& value) {
    if (value.empty()) {
        storage::putStr(key, "");
        return;
    }
    if (!theVault.unlocked()) return;
    const std::string sealed = theVault.seal(key, value);
    if (!sealed.empty()) storage::putStr(key, sealed);
}

bool readFile(const char* path, std::string& out) {
    out.clear();
    std::string raw;
    if (!theVault.unlocked() || !storage::readFile(path, raw)) return false;
    if (!vault::Vault::isSealed(raw)) {
        out.swap(raw);
        return true;
    }
    return theVault.open(path, raw, out);
}

bool writeFile(const char* path, const std::string& data) {
    if (!theVault.unlocked()) return false;
    const std::string sealed = theVault.seal(path, data);
    return !sealed.empty() && storage::writeFile(path, sealed);
}

bool askNewPassphrase(const char* title, std::string& out) {
    for (;;) {
        std::string a, b;
        if (!ui::prompt(title, a, true, "At least 6 characters. Longer is safer.")) return false;
        if (a.size() < kMinPassphrase) {
            ui::alert(title, "The passphrase needs at least " + std::to_string(kMinPassphrase) + " characters.");
            continue;
        }
        if (!ui::prompt("Repeat the passphrase", b, true)) {
            vault::wipe(a);
            return false;
        }
        const bool same = a == b;
        vault::wipe(b);
        if (!same) {
            vault::wipe(a);
            ui::alert(title, "The two entries are not the same. Please try again.");
            continue;
        }
        out.swap(a);
        return true;
    }
}

}  // namespace secrets
