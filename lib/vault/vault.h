// Encryption of stored secrets (passwords, private keys).
//
// A random 256-bit data key encrypts every secret with an AEAD cipher (AES-256-GCM
// on the device). The data key itself is stored wrapped (encrypted) with a key
// derived from the user's passphrase (PBKDF2-SHA256), so changing the passphrase
// only re-wraps the data key. Each secret is bound to its name (the settings key
// or file path) as associated data, so sealed values cannot be swapped around.
//
// Portable: the primitives come from a Crypto implementation (mbedTLS on the
// device, a stand-in in the host tests).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace vault {

constexpr size_t kKeyLen = 32, kSaltLen = 16, kNonceLen = 12, kTagLen = 16;
using Key = std::array<uint8_t, kKeyLen>;

class Crypto {
public:
    virtual ~Crypto() {}
    virtual void random(uint8_t* out, size_t n) = 0;
    // PBKDF2-HMAC-SHA256
    virtual bool deriveKey(const std::string& pass, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                           Key& out) = 0;
    // AEAD with a kNonceLen nonce: out = ciphertext || kTagLen tag.
    virtual bool seal(const Key& key, const uint8_t* nonce, const std::string& aad, const std::string& plain,
                      std::string& out) = 0;
    // Fails if the tag does not match (wrong key, tampered data or wrong aad).
    virtual bool open(const Key& key, const uint8_t* nonce, const std::string& aad, const std::string& sealed,
                      std::string& plain) = 0;
};

class Vault {
public:
    explicit Vault(Crypto& crypto) : crypto_(crypto) {}
    ~Vault() { lock(); }

    // Creates a new random data key protected by `pass` and unlocks the vault with it.
    // Returns the record to store ("" on failure).
    std::string create(const std::string& pass, uint32_t iterations);
    // Unwraps the data key from `record`. False: wrong passphrase or damaged record
    // (the vault is then locked).
    bool unlock(const std::string& record, const std::string& pass);
    // A record for a new passphrase around the current data key, so the secrets
    // stay readable. Requires the vault to be unlocked; "" on failure.
    std::string rewrap(const std::string& pass, uint32_t iterations);
    bool unlocked() const { return unlocked_; }
    void lock();  // forgets the data key

    // Encrypts a secret stored under `name`. Empty values stay empty. "" if locked.
    std::string seal(const std::string& name, const std::string& plain);
    // Decrypts a value made by seal() for the same name. False if locked, damaged
    // or not a sealed value.
    bool open(const std::string& name, const std::string& sealed, std::string& plain);
    // True if `value` looks like the output of seal().
    static bool isSealed(const std::string& value);

    static constexpr uint32_t kMinIterations = 1000, kMaxIterations = 10000000;

private:
    Crypto& crypto_;
    Key key_{};
    bool unlocked_ = false;
};

// Brute force protection: how long the unlock prompt waits before the next try,
// after `failures` wrong passphrases in a row. The first kFreeAttempts are free,
// then 30 s, doubling each time, up to kMaxLockoutSeconds.
constexpr uint32_t kFreeAttempts = 3, kMaxLockoutSeconds = 3600;
uint32_t lockoutSeconds(uint32_t failures);

// Record format details, exposed for tests.
std::string base64Encode(const std::string& data);
bool base64Decode(const std::string& text, std::string& out);
// Overwrites a string's characters before it is released.
void wipe(std::string& s);

}  // namespace vault
