#include "notes_store.h"

#include <Arduino.h>
#include <esp_random.h>

#include <algorithm>

#include "../../os/storage.h"

namespace {
const char* kDir = "/notes";
const char* kIndex = "/notes/index";
std::string textPath(const std::string& id) { return std::string(kDir) + "/" + id + ".txt"; }
std::string clean(std::string s) {  // index fields are tab separated
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}
}  // namespace

std::string NotesStore::titleOf(const std::string& text) {
    std::string t = text.substr(0, text.find('\n'));
    size_t i = 0;
    while (i < t.size() && t[i] == '#') i++;
    if (i > 0 && i <= 6 && (i == t.size() || t[i] == ' ')) t.erase(0, i);
    size_t a = t.find_first_not_of(' ');
    t = a == std::string::npos ? std::string() : t.substr(a);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.size() > 80) t.resize(80);
    return t;
}

// Index line: id, (unused), href, etag, flags, mtime, title
void NotesStore::load() {
    notes_.clear();
    storage::makeDir(kDir);
    std::string text;
    if (!storage::readFile(kIndex, text)) return;
    for (auto& line : storage::splitLines(text)) {
        auto f = storage::splitFields(line);
        if (f.size() < 7 || f[0].empty()) continue;
        Note n;
        n.id = f[0];
        n.href = f[2];
        n.etag = f[3];
        n.dirty = f[4].find('D') != std::string::npos;
        n.deleted = f[4].find('X') != std::string::npos;
        n.mtime = (time_t)strtoll(f[5].c_str(), nullptr, 10);
        n.title = f[6];
        notes_.push_back(n);
    }
}

bool NotesStore::save() {
    std::string text;
    for (auto& n : notes_) {
        text += n.id + "\t-\t" + clean(n.href) + "\t" + clean(n.etag) + "\t" + (n.dirty ? "D" : "") +
                (n.deleted ? "X" : "") + "-\t" + std::to_string((long long)n.mtime) + "\t" + clean(n.title) + "\n";
    }
    return storage::writeFile(kIndex, text);
}

std::vector<Note*> NotesStore::list() {
    std::vector<Note*> out;
    for (auto& n : notes_)
        if (!n.deleted) out.push_back(&n);
    std::stable_sort(out.begin(), out.end(), [](const Note* a, const Note* b) { return a->mtime > b->mtime; });
    return out;
}

Note* NotesStore::find(const std::string& id) {
    for (auto& n : notes_)
        if (n.id == id) return &n;
    return nullptr;
}

int NotesStore::unsyncedCount() const {
    int c = 0;
    for (auto& n : notes_)
        if (n.dirty || n.deleted) c++;
    return c;
}

Note* NotesStore::create(const std::string& text) {
    Note n;
    do {
        char buf[12];
        snprintf(buf, sizeof buf, "%08x", (unsigned)esp_random());
        n.id = buf;
    } while (find(n.id));
    notes_.push_back(n);
    Note* p = &notes_.back();
    setText(*p, text, true);
    return p;
}

std::string NotesStore::text(const Note& n) const {
    std::string t;
    storage::readFile(textPath(n.id).c_str(), t);
    return t;
}

bool NotesStore::setText(Note& n, const std::string& text, bool markDirty) {
    if (!storage::writeFile(textPath(n.id).c_str(), text)) return false;
    n.title = titleOf(text);
    time_t now = time(nullptr);
    n.mtime = now > 1700000000 ? now : n.mtime + 1;  // keep ordering sensible without a clock
    if (markDirty) n.dirty = true;
    return true;
}

void NotesStore::purge(const std::string& id) {
    storage::remove(textPath(id).c_str());
    storage::remove((std::string(kDir) + "/" + id + ".ics").c_str());  // left over from the CalDAV version
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const Note& n) { return n.id == id; }), notes_.end());
}

void NotesStore::removeByUser(const std::string& id) {
    Note* n = find(id);
    if (!n) return;
    if (n->href.empty()) {
        purge(id);
    } else {
        n->deleted = true;
        storage::remove(textPath(id).c_str());
    }
}

void NotesStore::detachAll() {
    std::vector<std::string> gone;
    for (auto& n : notes_) {
        if (n.deleted) {
            gone.push_back(n.id);
            continue;
        }
        n.href.clear();
        n.etag.clear();
        n.dirty = true;
        storage::remove((std::string(kDir) + "/" + n.id + ".ics").c_str());
    }
    for (auto& id : gone) purge(id);
}
