// Built-in microSD slot. The card shares the external display's SPI bus, so it is
// only mounted for the moment a file is written and unmounted right after.
// Must not be called while the display is in the middle of a drawing transaction.
#pragma once
#include <FS.h>

#include <string>

namespace hal {

enum class SdResult { Ok, NoCard, Error };

// Writes `data` to `path` (e.g. "/cardputerOS/id_ed25519.pub"), creating the folders.
SdResult sdWriteFile(const char* path, const std::string& data);
// Appends `data` to `path`, creating the file and its folders if needed.
SdResult sdAppendFile(const char* path, const std::string& data);
// True if a card is inserted and readable, and `path` exists on it (path may be
// nullptr to check for a card only).
bool sdExists(const char* path);

// Keeps the card mounted while the session exists, for code that reads and writes
// files directly (via sdFs()). Sessions nest, and the one-shot functions above
// share the mount, so they do not unmount a card in use. Keep sessions short:
// the card can only be removed safely while none is open.
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
// The card's file system (valid inside an SdSession). Paths start at the card root.
fs::FS& sdFs();
// Creates every folder of `dir` (e.g. "/cardputerOS/tasks"); needs a session.
bool sdMakeDirs(const char* dir);

}  // namespace hal
