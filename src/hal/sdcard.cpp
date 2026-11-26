#include "sdcard.h"

#include <SD.h>
#include <SPI.h>

#include "board_config.h"

namespace hal {

namespace {

int mounts = 0;  // open mounts (sessions and one-shot operations)

bool mount() {
    if (mounts > 0) {
        mounts++;
        return true;
    }
    // The display driver already started the shared bus as the Arduino SPI object.
    if (SD.begin(SD_CS_PIN, SPI, 20000000, "/sd", 2)) {
        mounts = 1;
        return true;
    }
    SD.end();
    digitalWrite(SD_CS_PIN, HIGH);
    return false;
}

void unmount() {
    if (mounts <= 0 || --mounts > 0) return;
    SD.end();  // unmount so the card can be removed at any time
    digitalWrite(SD_CS_PIN, HIGH);
}

// Creates every folder on the way to the file `path`.
bool makeParents(const char* path);
}  // namespace

SdSession::SdSession() : ok_(mount()) {}
SdSession::~SdSession() {
    if (ok_) unmount();
}

fs::FS& sdFs() { return SD; }

bool sdMakeDirs(const char* dir) { return makeParents((std::string(dir) + "/").c_str()); }

namespace {
// Creates every folder on the way to the file `path`.
bool makeParents(const char* path) {
    const std::string p(path);
    size_t pos = 1;
    while ((pos = p.find('/', pos)) != std::string::npos) {
        const std::string dir = p.substr(0, pos);
        if (!SD.exists(dir.c_str()) && !SD.mkdir(dir.c_str())) return false;
        pos++;
    }
    return true;
}

SdResult writeWithMode(const char* path, const std::string& data, const char* mode) {
    if (!mount()) return SdResult::NoCard;
    SdResult result = SdResult::Error;
    if (makeParents(path)) {
        File f = SD.open(path, mode, true);
        if (f) {
            const bool ok = f.write((const uint8_t*)data.data(), data.size()) == data.size();
            f.close();
            if (ok) result = SdResult::Ok;
        }
    }
    unmount();
    return result;
}

}  // namespace

SdResult sdWriteFile(const char* path, const std::string& data) { return writeWithMode(path, data, FILE_WRITE); }

SdResult sdAppendFile(const char* path, const std::string& data) { return writeWithMode(path, data, FILE_APPEND); }

bool sdExists(const char* path) {
    if (!mount()) return false;
    const bool ok = !path || SD.exists(path);
    unmount();
    return ok;
}

}  // namespace hal
