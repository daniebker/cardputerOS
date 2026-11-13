#include "ssh_keys.h"

#include <libssh_esp32.h>

#include <libssh/libssh.h>

#include "../../os/secrets.h"
#include "../../os/storage.h"

namespace sshkeys {

namespace {
constexpr const char* kComment = "cardputer";

// Wraps an ssh_key so every exit path frees it.
struct Key {
    ssh_key k = nullptr;
    ~Key() {
        if (k) ssh_key_free(k);
    }
};
}  // namespace

void libInit() {
    static bool done = false;
    if (!done) {
        libssh_begin();
        done = true;
    }
}

bool exists() { return storage::exists(kPrivatePath) && storage::exists(kPublicPath); }

bool generate(std::string& err) { return generateAt(kPrivatePath, kPublicPath, err); }

bool generateAt(const char* privPath, const char* pubPath, std::string& err) {
    libInit();
    if (!storage::mounted() || !storage::makeDir("/ssh")) {
        err = "Internal storage is not available.";
        return false;
    }
    Key key;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key.k) != SSH_OK) {
        err = "Key generation failed.";
        return false;
    }
    char* priv = nullptr;
    if (ssh_pki_export_privkey_base64_format(key.k, nullptr, nullptr, nullptr, &priv, SSH_FILE_FORMAT_OPENSSH) !=
            SSH_OK ||
        !priv) {
        err = "Could not encode the private key.";
        return false;
    }
    std::string privText = priv;
    explicit_bzero(priv, strlen(priv));
    ssh_string_free_char(priv);
    if (privText.empty() || privText.back() != '\n') privText.push_back('\n');

    char* b64 = nullptr;
    if (ssh_pki_export_pubkey_base64(key.k, &b64) != SSH_OK || !b64) {
        err = "Could not encode the public key.";
        return false;
    }
    const std::string pubLine = std::string("ssh-ed25519 ") + b64 + " " + kComment + "\n";
    ssh_string_free_char(b64);

    // the private key is stored encrypted (see os/secrets.h)
    const bool ok = secrets::writeFile(privPath, privText) && storage::writeFile(pubPath, pubLine);
    privText.assign(privText.size(), '\0');
    if (!ok) {
        err = secrets::unlocked() ? "Could not write the key files (storage full?)." : "The secrets are locked.";
        return false;
    }

    // Read it back the way a connection will, so a broken file is caught now.
    Key check;
    std::string stored;
    const bool readable = secrets::readFile(privPath, stored) &&
                          ssh_pki_import_privkey_base64(stored.c_str(), nullptr, nullptr, nullptr, &check.k) == SSH_OK;
    stored.assign(stored.size(), '\0');
    if (!readable || ssh_key_cmp(key.k, check.k, SSH_KEY_CMP_PUBLIC) != 0) {
        storage::remove(privPath);
        storage::remove(pubPath);
        err = "The saved key could not be read back.";
        return false;
    }
    return true;
}

std::string publicKey() {
    std::string line;
    if (!storage::readFile(kPublicPath, line)) return std::string();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

std::string fingerprint() {
    libInit();
    const std::string line = publicKey();
    const size_t a = line.find(' ');
    if (a == std::string::npos) return std::string();
    const size_t b = line.find(' ', a + 1);
    const std::string b64 = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    Key key;
    if (ssh_pki_import_pubkey_base64(b64.c_str(), SSH_KEYTYPE_ED25519, &key.k) != SSH_OK) return std::string();
    unsigned char* hash = nullptr;
    size_t hlen = 0;
    std::string out;
    if (ssh_get_publickey_hash(key.k, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == SSH_OK) {
        char* s = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
        if (s) {
            out = s;
            ssh_string_free_char(s);
        }
        ssh_clean_pubkey_hash(&hash);
    }
    return out;
}

}  // namespace sshkeys
