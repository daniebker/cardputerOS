#include "calendar_store.h"

#include <algorithm>
#include <cstdlib>

#include "../../hal/sdcard.h"
#include "../../os/secrets.h"
#include "../../os/storage.h"
#include "../../os/theme.h"

namespace calstore {

const char* kDir = "/cardputerOS/calendar";

namespace dr = theme::dracula;
const uint32_t kColors[kColorCount] = {dr::cyan, dr::green, dr::orange, dr::pink, dr::purple, dr::yellow};
const char* const kColorNames[kColorCount] = {"Cyan", "Green", "Orange", "Pink", "Purple", "Yellow"};

namespace {
fs::FS& sd() { return hal::sdFs(); }

std::string key(const char* prefix, int id) { return std::string(prefix) + std::to_string(id); }

std::string metaPath(int id) { return std::string(kDir) + "/" + std::to_string(id) + ".meta"; }

std::string oneLine(const std::string& s) {
    std::string out;
    for (char c : s) out.push_back(c == '\n' || c == '\r' ? ' ' : c);
    return out;
}
}  // namespace

// NVS "cal_list": one "id<TAB>colour<TAB>name" line per calendar
std::vector<CalendarSub> loadSubs() {
    std::vector<CalendarSub> out;
    for (auto& line : storage::splitLines(storage::getStr("cal_list"))) {
        auto f = storage::splitFields(line);
        if (f.size() < 2) continue;
        CalendarSub s;
        s.id = atoi(f[0].c_str());
        if (s.id <= 0) continue;
        s.color = atoi(f[1].c_str()) % kColorCount;
        if (f.size() > 2) s.name = f[2];
        s.url = secrets::get(key("cal_url", s.id).c_str());
        s.user = storage::getStr(key("cal_user", s.id).c_str());
        s.pass = secrets::get(key("cal_pw", s.id).c_str());
        out.push_back(std::move(s));
    }
    return out;
}

void saveSubs(const std::vector<CalendarSub>& subs) {
    std::string text;
    for (auto& s : subs) {
        text += std::to_string(s.id) + "\t" + std::to_string(s.color) + "\t";
        for (char c : s.name) text.push_back(c == '\t' || c == '\n' ? ' ' : c);
        text += "\n";
        if (s.url != secrets::get(key("cal_url", s.id).c_str())) secrets::put(key("cal_url", s.id).c_str(), s.url);
        if (s.user != storage::getStr(key("cal_user", s.id).c_str())) storage::putStr(key("cal_user", s.id).c_str(), s.user);
        if (s.pass != secrets::get(key("cal_pw", s.id).c_str())) secrets::put(key("cal_pw", s.id).c_str(), s.pass);
    }
    storage::putStr("cal_list", text);
}

void forgetSecrets(int id) {
    secrets::put(key("cal_url", id).c_str(), "");
    secrets::put(key("cal_pw", id).c_str(), "");
    storage::putStr(key("cal_user", id).c_str(), "");
}

int newId(const std::vector<CalendarSub>& subs) {
    for (int id = 1;; id++) {
        bool used = false;
        for (auto& s : subs) used = used || s.id == id;
        if (!used) return id;
    }
}

std::string icsPath(int id) { return std::string(kDir) + "/" + std::to_string(id) + ".ics"; }
std::string tmpPath(int id) { return std::string(kDir) + "/" + std::to_string(id) + ".tmp"; }

void readMeta(CalendarSub& s) {
    s.etag.clear();
    s.lastModified.clear();
    s.error.clear();
    s.feedName.clear();
    s.checkedAt = s.fetchedAt = 0;
    s.cached = sd().exists(icsPath(s.id).c_str());
    const std::string path = metaPath(s.id);
    if (!s.cached || !sd().exists(path.c_str())) return;
    File f = sd().open(path.c_str(), "r");
    if (!f) return;
    std::string text;
    const size_t size = std::min<size_t>(f.size(), 4096);
    text.resize(size);
    text.resize(size ? f.read((uint8_t*)&text[0], size) : 0);
    f.close();
    for (auto& line : storage::splitLines(text)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "etag") s.etag = v;
        else if (k == "modified") s.lastModified = v;
        else if (k == "error") s.error = v;
        else if (k == "name") s.feedName = v;
        else if (k == "checked") s.checkedAt = atoll(v.c_str());
        else if (k == "fetched") s.fetchedAt = atoll(v.c_str());
    }
}

bool writeMeta(const CalendarSub& s) {
    if (!hal::sdMakeDirs(kDir)) return false;
    const std::string text = "etag=" + oneLine(s.etag) + "\nmodified=" + oneLine(s.lastModified) +
                             "\nchecked=" + std::to_string((long long)s.checkedAt) +
                             "\nfetched=" + std::to_string((long long)s.fetchedAt) + "\nname=" + oneLine(s.feedName) +
                             "\nerror=" + oneLine(s.error) + "\n";
    const std::string path = metaPath(s.id), tmp = path + ".tmp";
    File f = sd().open(tmp.c_str(), "w");
    if (!f) return false;
    const bool ok = f.write((const uint8_t*)text.data(), text.size()) == text.size();
    f.close();
    if (!ok) {
        sd().remove(tmp.c_str());
        return false;
    }
    if (sd().exists(path.c_str())) sd().remove(path.c_str());
    return sd().rename(tmp.c_str(), path.c_str());
}

bool commitDownload(int id) {
    const std::string tmp = tmpPath(id), path = icsPath(id);
    if (!sd().exists(tmp.c_str())) return false;
    if (sd().exists(path.c_str())) sd().remove(path.c_str());
    return sd().rename(tmp.c_str(), path.c_str());
}

void removeFiles(int id) {
    for (const std::string& p : {icsPath(id), tmpPath(id), metaPath(id), metaPath(id) + ".tmp"})
        if (sd().exists(p.c_str())) sd().remove(p.c_str());
}

}  // namespace calstore
