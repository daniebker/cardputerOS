// Built-in microSD slot. The card shares the external display's SPI bus, so it is
// only mounted for the moment a file is written and unmounted right after.
// Must not be called while the display is in the middle of a drawing transaction.
#pragma once
#include <string>

namespace hal {

enum class SdResult { Ok, NoCard, Error };

// Writes `data` to `path` (e.g. "/cardputerOS/id_ed25519.pub"), creating the folder.
SdResult sdWriteFile(const char* path, const std::string& data);

}  // namespace hal
