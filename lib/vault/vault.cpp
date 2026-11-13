#include "vault.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace vault {

namespace {

// Sealed value: prefix + base64(nonce || ciphertext || tag)
const char kPrefix[] = "$cpv1$";
constexpr size_t kPrefixLen = sizeof kPrefix - 1;
// Record: "1:<iterations>:<salt>:<nonce>:<wrapped key>", binary fields in base64
const char kRecordVersion[] = "1";
const char kWrapAad[] = "cardputerOS vault key";

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void wipeKey(Key& k) {
    volatile uint8_t* p = k.data();
    for (size_t i = 0; i < k.size(); i++) p[i] = 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t e = s.find(sep, pos);
        out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) return out;
        pos = e + 1;
    }
}

std::string bytes(const uint8_t* p, size_t n) { return std::string((const char*)p, n); }

}  // namespace

void wipe(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); i++) p[i] = 0;
    s.clear();
}

// ---------------------------------------------------------------- base64

std::string base64Encode(const std::string& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t v = ((uint8_t)data[i] << 16) | ((uint8_t)data[i + 1] << 8) | (uint8_t)data[i + 2];
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < data.size()) {
        uint32_t v = (uint8_t)data[i] << 16;
        if (i + 1 < data.size()) v |= (uint8_t)data[i + 1] << 8;
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < data.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool base64Decode(const std::string& text, std::string& out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=') : 0;
        if (pad == 1 && text[i + 2] == '=') return false;  // "x=y=" is not valid
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = text[i + k];
            int d;
            if (c == '=' && k >= 4 - pad) d = 0;
            else if ((d = val(c)) < 0) return false;
            v = (v << 6) | (uint32_t)d;
        }
        out += (char)(v >> 16);
        if (pad < 2) out += (char)((v >> 8) & 0xFF);
        if (pad < 1) out += (char)(v & 0xFF);
    }
    return true;
}

uint32_t lockoutSeconds(uint32_t failures) {
    if (failures < kFreeAttempts) return 0;
    const uint32_t doublings = failures - kFreeAttempts;
    if (doublings >= 7) return kMaxLockoutSeconds;  // 30 << 7 > 3600
    const uint32_t s = 30u << doublings;
    return s < kMaxLockoutSeconds ? s : kMaxLockoutSeconds;
}

// ---------------------------------------------------------------- Vault

void Vault::lock() {
    wipeKey(key_);
    unlocked_ = false;
}

std::string Vault::create(const std::string& pass, uint32_t iterations) {
    lock();
    crypto_.random(key_.data(), key_.size());
    unlocked_ = true;
    std::string record = rewrap(pass, iterations);
    if (record.empty()) lock();
    return record;
}

std::string Vault::rewrap(const std::string& pass, uint32_t iterations) {
    if (!unlocked_ || iterations < kMinIterations || iterations > kMaxIterations) return "";
    uint8_t salt[kSaltLen], nonce[kNonceLen];
    crypto_.random(salt, sizeof salt);
    crypto_.random(nonce, sizeof nonce);
    Key kek;
    std::string keyBytes = bytes(key_.data(), key_.size()), wrapped;
    const bool ok = crypto_.deriveKey(pass, salt, sizeof salt, iterations, kek) &&
                    crypto_.seal(kek, nonce, kWrapAad, keyBytes, wrapped);
    wipeKey(kek);
    wipe(keyBytes);
    if (!ok) return "";
    return std::string(kRecordVersion) + ":" + std::to_string(iterations) + ":" + base64Encode(bytes(salt, sizeof salt)) +
           ":" + base64Encode(bytes(nonce, sizeof nonce)) + ":" + base64Encode(wrapped);
}

bool Vault::unlock(const std::string& record, const std::string& pass) {
    lock();
    const auto f = split(record, ':');
    if (f.size() != 5 || f[0] != kRecordVersion) return false;
    char* end = nullptr;
    const unsigned long iterations = strtoul(f[1].c_str(), &end, 10);
    if (f[1].empty() || *end || iterations < kMinIterations || iterations > kMaxIterations) return false;
    std::string salt, nonce, wrapped, keyBytes;
    if (!base64Decode(f[2], salt) || !base64Decode(f[3], nonce) || !base64Decode(f[4], wrapped)) return false;
    if (salt.size() != kSaltLen || nonce.size() != kNonceLen || wrapped.size() != kKeyLen + kTagLen) return false;
    Key kek;
    bool ok = crypto_.deriveKey(pass, (const uint8_t*)salt.data(), salt.size(), (uint32_t)iterations, kek) &&
              crypto_.open(kek, (const uint8_t*)nonce.data(), kWrapAad, wrapped, keyBytes) &&
              keyBytes.size() == kKeyLen;
    wipeKey(kek);
    if (ok) {
        memcpy(key_.data(), keyBytes.data(), kKeyLen);
        unlocked_ = true;
    }
    wipe(keyBytes);
    return ok;
}

bool Vault::isSealed(const std::string& value) { return value.compare(0, kPrefixLen, kPrefix) == 0; }

std::string Vault::seal(const std::string& name, const std::string& plain) {
    if (!unlocked_) return "";
    if (plain.empty()) return "";
    uint8_t nonce[kNonceLen];
    crypto_.random(nonce, sizeof nonce);
    std::string ct;
    if (!crypto_.seal(key_, nonce, name, plain, ct)) return "";
    return kPrefix + base64Encode(bytes(nonce, sizeof nonce) + ct);
}

bool Vault::open(const std::string& name, const std::string& sealed, std::string& plain) {
    plain.clear();
    if (!unlocked_) return false;
    if (sealed.empty()) return true;
    if (!isSealed(sealed)) return false;
    std::string raw;
    if (!base64Decode(sealed.substr(kPrefixLen), raw) || raw.size() < kNonceLen + kTagLen) return false;
    return crypto_.open(key_, (const uint8_t*)raw.data(), name, raw.substr(kNonceLen), plain);
}

}  // namespace vault
