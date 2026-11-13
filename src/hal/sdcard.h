// Built-in microSD slot. The card shares the external display's SPI bus, so it is
// only mounted for the moment a file is written and unmounted right after.
// Must not be called while the display is in the middle of a drawing transaction.
#pragma once
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

}  // namespace hal
