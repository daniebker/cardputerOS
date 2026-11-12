#include "storage.h"

#include <LittleFS.h>
#include <Preferences.h>

namespace storage {

namespace {
bool fsOk = false;
Preferences prefs;
}  // namespace

void begin() { prefs.begin("cpos", false); }

void mountFiles() {
    fsOk = LittleFS.begin(true);  // formats the partition on first boot (takes a few seconds)
    if (!fsOk) log_e("LittleFS mount failed");
}

bool mounted() { return fsOk; }

bool readFile(const char* path, std::string& out) {
    out.clear();
    if (!fsOk || !LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    out.resize(f.size());
    size_t n = f.read((uint8_t*)&out[0], out.size());
    out.resize(n);
    f.close();
    return true;
}

bool writeFile(const char* path, const std::string& data) {
    if (!fsOk) return false;
    // write to a temp file first so a power cut cannot leave a half-written file
    std::string tmp = std::string(path) + ".tmp";
    File f = LittleFS.open(tmp.c_str(), "w");
    if (!f) return false;
    size_t n = f.write((const uint8_t*)data.data(), data.size());
    f.close();
    if (n != data.size()) {
        LittleFS.remove(tmp.c_str());
        return false;
    }
    LittleFS.remove(path);
    return LittleFS.rename(tmp.c_str(), path);
}

bool exists(const char* path) { return fsOk && LittleFS.exists(path); }
bool remove(const char* path) { return fsOk && LittleFS.exists(path) && LittleFS.remove(path); }
bool makeDir(const char* path) { return fsOk && (LittleFS.exists(path) || LittleFS.mkdir(path)); }

std::string vfsPath(const char* path) { return std::string("/littlefs") + path; }

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
        pos = nl + 1;
    }
    return out;
}

std::vector<std::string> splitFields(const std::string& line, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t e = line.find(sep, pos);
        if (e == std::string::npos) {
            out.push_back(line.substr(pos));
            break;
        }
        out.push_back(line.substr(pos, e - pos));
        pos = e + 1;
    }
    return out;
}

int getInt(const char* key, int def) { return prefs.getInt(key, def); }
void putInt(const char* key, int value) { prefs.putInt(key, value); }
std::string getStr(const char* key, const char* def) {
    if (!prefs.isKey(key)) return def;
    return std::string(prefs.getString(key, def).c_str());
}
void putStr(const char* key, const std::string& value) { prefs.putString(key, value.c_str()); }

}  // namespace storage
