#include <unity.h>

#include <cstring>

#include "vault.h"

using namespace vault;

// Stand-in primitives for the host: not secure, but they behave like the real
// ones (an AEAD that rejects a wrong key, wrong aad or changed data).
class FakeCrypto : public Crypto {
public:
    uint64_t counter = 1;
    int derives = 0;
    uint32_t lastIterations = 0;

    static uint64_t mix(uint64_t h, const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < n; i++) {
            h ^= p[i];
            h *= 0x100000001B3ull;
            h ^= h >> 29;
        }
        return h;
    }
    static uint64_t mix(uint64_t h, const std::string& s) { return mix(mix(h, s.data(), s.size()), "|", 1); }

    void random(uint8_t* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            counter = counter * 6364136223846793005ull + 1442695040888963407ull;
            out[i] = (uint8_t)(counter >> 56);
        }
    }
    bool deriveKey(const std::string& pass, const uint8_t* salt, size_t saltLen, uint32_t iterations, Key& out) override {
        derives++;
        lastIterations = iterations;
        uint64_t h = mix(mix(0xCBF29CE484222325ull, pass), salt, saltLen);
        h = mix(h, &iterations, sizeof iterations);
        for (size_t i = 0; i < out.size(); i++) {
            h = mix(h, &i, sizeof i);
            out[i] = (uint8_t)h;
        }
        return true;
    }
    std::string stream(const Key& key, const uint8_t* nonce, size_t n) {
        std::string s;
        uint64_t h = mix(mix(7, key.data(), key.size()), nonce, kNonceLen);
        for (size_t i = 0; i < n; i++) {
            h = mix(h, &i, sizeof i);
            s += (char)h;
        }
        return s;
    }
    std::string tag(const Key& key, const uint8_t* nonce, const std::string& aad, const std::string& ct) {
        uint64_t h = mix(mix(mix(mix(11, key.data(), key.size()), nonce, kNonceLen), aad), ct);
        std::string t;
        for (size_t i = 0; i < kTagLen; i++) {
            h = mix(h, &i, sizeof i);
            t += (char)h;
        }
        return t;
    }
    bool seal(const Key& key, const uint8_t* nonce, const std::string& aad, const std::string& plain,
              std::string& out) override {
        std::string ks = stream(key, nonce, plain.size());
        out = plain;
        for (size_t i = 0; i < out.size(); i++) out[i] ^= ks[i];
        out += tag(key, nonce, aad, out);
        return true;
    }
    bool open(const Key& key, const uint8_t* nonce, const std::string& aad, const std::string& sealed,
              std::string& plain) override {
        if (sealed.size() < kTagLen) return false;
        const std::string ct = sealed.substr(0, sealed.size() - kTagLen);
        if (tag(key, nonce, aad, ct) != sealed.substr(ct.size())) return false;
        std::string ks = stream(key, nonce, ct.size());
        plain = ct;
        for (size_t i = 0; i < plain.size(); i++) plain[i] ^= ks[i];
        return true;
    }
};

void setUp() {}
void tearDown() {}

void test_base64() {
    const char* cases[][2] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="},
                              {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (auto& c : cases) {
        TEST_ASSERT_EQUAL_STRING(c[1], base64Encode(c[0]).c_str());
        std::string out;
        TEST_ASSERT_TRUE(base64Decode(c[1], out));
        TEST_ASSERT_EQUAL_STRING(c[0], out.c_str());
    }
    std::string all;
    for (int i = 0; i < 256; i++) all += (char)i;
    std::string back;
    TEST_ASSERT_TRUE(base64Decode(base64Encode(all), back));
    TEST_ASSERT_TRUE(all == back);
    TEST_ASSERT_FALSE(base64Decode("Zm9", back));      // length
    TEST_ASSERT_FALSE(base64Decode("Zm*v", back));     // character
    TEST_ASSERT_FALSE(base64Decode("Zg==Zm9v", back)); // padding in the middle
    TEST_ASSERT_FALSE(base64Decode("Z=g=", back));
}

void test_create_unlock() {
    FakeCrypto c;
    Vault v(c);
    TEST_ASSERT_FALSE(v.unlocked());
    const std::string record = v.create("correct horse", 30000);
    TEST_ASSERT_FALSE(record.empty());
    TEST_ASSERT_TRUE(v.unlocked());
    TEST_ASSERT_EQUAL(0, (int)record.find("1:30000:"));
    const std::string sealed = v.seal("wifi", "home\tsecret\n");
    TEST_ASSERT_TRUE(Vault::isSealed(sealed));
    TEST_ASSERT_TRUE(sealed.find("secret") == std::string::npos);

    // a fresh vault (as after a reboot)
    Vault v2(c);
    std::string plain;
    TEST_ASSERT_FALSE(v2.open("wifi", sealed, plain));  // locked
    TEST_ASSERT_FALSE(v2.unlock(record, "wrong horse"));
    TEST_ASSERT_FALSE(v2.unlocked());
    TEST_ASSERT_TRUE(v2.unlock(record, "correct horse"));
    TEST_ASSERT_EQUAL(30000, (int)c.lastIterations);
    TEST_ASSERT_TRUE(v2.open("wifi", sealed, plain));
    TEST_ASSERT_EQUAL_STRING("home\tsecret\n", plain.c_str());

    v2.lock();
    TEST_ASSERT_FALSE(v2.open("wifi", sealed, plain));
    TEST_ASSERT_EQUAL_STRING("", v2.seal("wifi", "x").c_str());
}

void test_seal_binding_and_tamper() {
    FakeCrypto c;
    Vault v(c);
    v.create("pw-123456", 1000);
    const std::string a = v.seal("dav_pass", "hunter2");
    const std::string b = v.seal("dav_pass", "hunter2");
    TEST_ASSERT_TRUE(a != b);  // fresh nonce every time
    std::string plain;
    TEST_ASSERT_FALSE(v.open("jr_pass", a, plain));  // bound to its name
    std::string bad = a;
    bad[bad.size() - 3] = bad[bad.size() - 3] == 'A' ? 'B' : 'A';
    TEST_ASSERT_FALSE(v.open("dav_pass", bad, plain));
    TEST_ASSERT_FALSE(v.open("dav_pass", "hunter2", plain));  // plaintext is not a sealed value
    TEST_ASSERT_FALSE(v.open("dav_pass", "$cpv1$AAAA", plain));  // too short
    TEST_ASSERT_TRUE(v.open("dav_pass", b, plain));
    TEST_ASSERT_EQUAL_STRING("hunter2", plain.c_str());

    // empty values stay empty
    TEST_ASSERT_EQUAL_STRING("", v.seal("dav_pass", "").c_str());
    TEST_ASSERT_TRUE(v.open("dav_pass", "", plain));
    TEST_ASSERT_EQUAL_STRING("", plain.c_str());

    // binary content (private key files) survives
    std::string bin("\0\x01\xff-----BEGIN\n", 14);
    TEST_ASSERT_TRUE(v.open("/ssh/id", v.seal("/ssh/id", bin), plain));
    TEST_ASSERT_TRUE(bin == plain);
}

void test_rewrap_keeps_secrets() {
    FakeCrypto c;
    Vault v(c);
    const std::string oldRecord = v.create("old passphrase", 1000);
    const std::string sealed = v.seal("ssh_hosts", "box\tbox.lan\t22\tme\tpw\n");
    const std::string newRecord = v.rewrap("new passphrase", 2000);
    TEST_ASSERT_FALSE(newRecord.empty());
    TEST_ASSERT_TRUE(newRecord != oldRecord);

    Vault v2(c);
    TEST_ASSERT_FALSE(v2.unlock(newRecord, "old passphrase"));
    TEST_ASSERT_TRUE(v2.unlock(newRecord, "new passphrase"));
    std::string plain;
    TEST_ASSERT_TRUE(v2.open("ssh_hosts", sealed, plain));  // no re-encryption needed
    TEST_ASSERT_EQUAL_STRING("box\tbox.lan\t22\tme\tpw\n", plain.c_str());

    v2.lock();
    TEST_ASSERT_EQUAL_STRING("", v2.rewrap("x", 1000).c_str());  // needs the data key
}

void test_bad_records() {
    FakeCrypto c;
    Vault v(c);
    const std::string good = v.create("pw-123456", 1000);
    Vault v2(c);
    const int before = c.derives;
    const char* bad[] = {"", "1", "2:1000:a:b:c", "1:abc:AAAA:AAAA:AAAA", "1:10:AAAA:AAAA:AAAA",
                         "1:1000:AAAA:AAAA:AAAA", "1:1000:!!!!:AAAA:AAAA"};
    for (const char* r : bad) TEST_ASSERT_FALSE(v2.unlock(r, "pw-123456"));
    TEST_ASSERT_EQUAL(before, c.derives);  // rejected before the expensive key derivation
    // a changed wrapped key is detected
    std::string tampered = good;
    tampered[tampered.size() - 5] = tampered[tampered.size() - 5] == 'A' ? 'B' : 'A';
    TEST_ASSERT_FALSE(v2.unlock(tampered, "pw-123456"));
    TEST_ASSERT_TRUE(v2.unlock(good, "pw-123456"));
    // iteration bounds
    TEST_ASSERT_EQUAL_STRING("", v.rewrap("x", 10).c_str());
}

void test_lockout() {
    TEST_ASSERT_EQUAL(0, (int)lockoutSeconds(0));
    TEST_ASSERT_EQUAL(0, (int)lockoutSeconds(2));
    TEST_ASSERT_EQUAL(30, (int)lockoutSeconds(3));
    TEST_ASSERT_EQUAL(60, (int)lockoutSeconds(4));
    TEST_ASSERT_EQUAL(1920, (int)lockoutSeconds(9));
    TEST_ASSERT_EQUAL(3600, (int)lockoutSeconds(10));
    TEST_ASSERT_EQUAL(3600, (int)lockoutSeconds(40));
    TEST_ASSERT_EQUAL(3600, (int)lockoutSeconds(0xFFFFFFFFu));
}

void test_wipe() {
    std::string s = "secret";
    wipe(s);
    TEST_ASSERT_TRUE(s.empty());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_base64);
    RUN_TEST(test_create_unlock);
    RUN_TEST(test_seal_binding_and_tamper);
    RUN_TEST(test_rewrap_keeps_secrets);
    RUN_TEST(test_bad_records);
    RUN_TEST(test_lockout);
    RUN_TEST(test_wipe);
    return UNITY_END();
}
