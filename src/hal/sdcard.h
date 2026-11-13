// Built-in microSD slot. The card shares the external display's SPI bus, so it is
// only mounted while files are accessed and unmounted right after (it can be
// removed at any time). Must not be called while the display is in the middle of
// a drawing transaction (between startWrite and endWrite).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hal {

enum class SdResult { Ok, NoCard, NotFound, Error };

// Keeps the card mounted for its lifetime, so several accesses in a row do not
// mount it again each time. ok() is false if there is no card.
class SdSession {
public:
    SdSession();
    ~SdSession();
    SdSession(const SdSession&) = delete;
    SdSession& operator=(const SdSession&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_;
};

struct SdEntry {
    std::string name;
    bool dir = false;
    uint32_t size = 0;
};

// Writes `data` to `path` (e.g. "/cardputerOS/id_ed25519.pub"), creating the folders.
SdResult sdWriteFile(const char* path, const std::string& data);
// Appends `data` to `path`, creating the file and its folders if needed.
SdResult sdAppendFile(const char* path, const std::string& data);
// Reads a whole file; Error if it is larger than `maxBytes`.
SdResult sdReadFile(const char* path, std::string& out, size_t maxBytes);
// The entries of a folder (no "." / ".."), in the card's order.
SdResult sdListDir(const char* path, std::vector<SdEntry>& out);
SdResult sdRemove(const char* path);
// True if a card is inserted and readable, and `path` exists on it (path may be
// nullptr to check for a card only).
bool sdExists(const char* path);

}  // namespace hal
