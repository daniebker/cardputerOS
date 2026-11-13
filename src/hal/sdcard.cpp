#include "sdcard.h"

#include <SD.h>
#include <SPI.h>

#include "board_config.h"

namespace hal {

namespace {

int mountCount = 0;  // nested mounts (SdSession and single accesses)
bool mountedOk = false;

bool mount() {
    if (mountCount++ > 0) return mountedOk;
    // The display driver already started the shared bus as the Arduino SPI object.
    mountedOk = SD.begin(SD_CS_PIN, SPI, 20000000, "/sd", 2);
    if (!mountedOk) {
        SD.end();
        digitalWrite(SD_CS_PIN, HIGH);
    }
    return mountedOk;
}

void unmount() {
    if (mountCount == 0 || --mountCount > 0) return;
    if (mountedOk) SD.end();  // unmount so the card can be removed at any time
    digitalWrite(SD_CS_PIN, HIGH);
    mountedOk = false;
}

// Mounts for the lifetime of the object.
struct Mount {
    bool ok;
    Mount() : ok(mount()) {}
    ~Mount() { unmount(); }
};

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
    Mount m;
    if (!m.ok) return SdResult::NoCard;
    if (!makeParents(path)) return SdResult::Error;
    File f = SD.open(path, mode, true);
    if (!f) return SdResult::Error;
    const bool ok = f.write((const uint8_t*)data.data(), data.size()) == data.size();
    f.close();
    return ok ? SdResult::Ok : SdResult::Error;
}

}  // namespace

SdSession::SdSession() : ok_(mount()) {}
SdSession::~SdSession() { unmount(); }

SdResult sdWriteFile(const char* path, const std::string& data) { return writeWithMode(path, data, FILE_WRITE); }

SdResult sdAppendFile(const char* path, const std::string& data) { return writeWithMode(path, data, FILE_APPEND); }

SdResult sdReadFile(const char* path, std::string& out, size_t maxBytes) {
    out.clear();
    Mount m;
    if (!m.ok) return SdResult::NoCard;
    if (!SD.exists(path)) return SdResult::NotFound;
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) return SdResult::NotFound;
    const size_t size = f.size();
    if (size > maxBytes) {
        f.close();
        return SdResult::Error;
    }
    out.resize(size);
    const size_t n = size ? f.read((uint8_t*)&out[0], size) : 0;
    f.close();
    if (n != size) {
        out.clear();
        return SdResult::Error;
    }
    return SdResult::Ok;
}

SdResult sdListDir(const char* path, std::vector<SdEntry>& out) {
    out.clear();
    Mount m;
    if (!m.ok) return SdResult::NoCard;
    File dir = SD.open(path);
    if (!dir) return SdResult::NotFound;
    if (!dir.isDirectory()) {
        dir.close();
        return SdResult::NotFound;
    }
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        SdEntry en;
        en.name = e.name();  // the name only, without the folder
        const size_t sl = en.name.rfind('/');
        if (sl != std::string::npos) en.name = en.name.substr(sl + 1);
        en.dir = e.isDirectory();
        en.size = en.dir ? 0 : (uint32_t)e.size();
        e.close();
        if (en.name.empty() || en.name == "." || en.name == "..") continue;
        out.push_back(en);
        if (out.size() >= 500) break;
    }
    dir.close();
    return SdResult::Ok;
}

SdResult sdRemove(const char* path) {
    Mount m;
    if (!m.ok) return SdResult::NoCard;
    if (!SD.exists(path)) return SdResult::NotFound;
    return SD.remove(path) ? SdResult::Ok : SdResult::Error;
}

bool sdExists(const char* path) {
    Mount m;
    return m.ok && (!path || SD.exists(path));
}

}  // namespace hal
