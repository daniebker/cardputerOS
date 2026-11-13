#include "journal_store.h"

#include <algorithm>

#include "../../os/storage.h"
#include "journal.h"

namespace {
const char* kDir = "/journal";
const char* kIndex = "/journal/index";
std::string path(const std::string& date) { return std::string(kDir) + "/" + date + ".md"; }
}  // namespace

// Index line: date <TAB> etag <TAB> flags
void JournalStore::load() {
    days_.clear();
    storage::makeDir(kDir);
    std::string text;
    if (!storage::readFile(kIndex, text)) return;
    for (auto& line : storage::splitLines(text)) {
        auto f = storage::splitFields(line);
        if (f.size() < 3 || !journal::validDate(f[0])) continue;
        JournalDay d;
        d.date = f[0];
        d.etag = f[1];
        d.dirty = f[2].find('D') != std::string::npos;
        days_.push_back(d);
    }
    std::sort(days_.begin(), days_.end(), [](const JournalDay& a, const JournalDay& b) { return a.date < b.date; });
}

bool JournalStore::save() {
    std::string text;
    for (auto& d : days_) {
        std::string etag = d.etag;
        for (char& c : etag)
            if (c == '\t' || c == '\n') c = ' ';
        text += d.date + "\t" + etag + "\t" + (d.dirty ? "D" : "-") + "\n";
    }
    return storage::writeFile(kIndex, text);
}

JournalDay* JournalStore::find(const std::string& date) {
    for (auto& d : days_)
        if (d.date == date) return &d;
    return nullptr;
}

int JournalStore::unsyncedCount() const {
    int n = 0;
    for (auto& d : days_)
        if (d.dirty) n++;
    return n;
}

std::string JournalStore::text(const std::string& date) const {
    std::string t;
    storage::readFile(path(date).c_str(), t);
    return t;
}

bool JournalStore::setText(const std::string& date, const std::string& text, bool markDirty) {
    if (!journal::validDate(date) || !storage::writeFile(path(date).c_str(), text)) return false;
    JournalDay* d = find(date);
    if (!d) {
        JournalDay nd;
        nd.date = date;
        days_.push_back(nd);
        std::sort(days_.begin(), days_.end(), [](const JournalDay& a, const JournalDay& b) { return a.date < b.date; });
        d = find(date);
    }
    if (markDirty) d->dirty = true;
    return true;
}

void JournalStore::remove(const std::string& date) {
    storage::remove(path(date).c_str());
    days_.erase(std::remove_if(days_.begin(), days_.end(), [&](const JournalDay& d) { return d.date == date; }), days_.end());
}

void JournalStore::detachAll() {
    for (auto& d : days_) {
        d.etag.clear();
        d.dirty = true;
    }
}
