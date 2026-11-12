// The device's own SSH key pair (Ed25519), generated and kept on the device.
#pragma once
#include <string>

namespace sshkeys {

constexpr const char* kPrivatePath = "/ssh/id_ed25519";  // LittleFS path
constexpr const char* kPublicPath = "/ssh/id_ed25519.pub";

void libInit();  // libssh global setup, safe to call repeatedly

bool exists();
// Creates a new key pair, replacing any existing one. Returns false and sets err on failure.
bool generate(std::string& err);
// Same, writing to the given LittleFS paths (used by tests).
bool generateAt(const char* privPath, const char* pubPath, std::string& err);
// "ssh-ed25519 AAAA... cardputer" (empty if there is no key)
std::string publicKey();
// "SHA256:..." fingerprint of the public key (empty if there is no key)
std::string fingerprint();

}  // namespace sshkeys
