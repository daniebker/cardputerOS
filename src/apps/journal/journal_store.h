// Journal days kept on the internal flash (LittleFS, /journal/YYYY-MM-DD.md) with
// the bookkeeping needed to sync them with a WebDAV folder.
#pragma once
#include <string>
#include <vector>

struct JournalDay {
    std::string date;  // "2026-11-12"
    std::string etag;  // server version our copy is based on ("" = never synced)
    bool dirty = false;
};

class JournalStore {
public:
    void load();
    bool save();  // writes the index

    // All days, oldest first.
    const std::vector<JournalDay>& days() const { return days_; }
    JournalDay* find(const std::string& date);
    int unsyncedCount() const;

    std::string text(const std::string& date) const;
    // Creates the day if needed.
    bool setText(const std::string& date, const std::string& text, bool markDirty);
    void remove(const std::string& date);
    // Forget all server state (the sync folder changed): everything is uploaded again.
    void detachAll();

private:
    std::vector<JournalDay> days_;
};
