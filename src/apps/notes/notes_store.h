// Notes kept on the internal flash (LittleFS, /notes) as Markdown text, with the
// bookkeeping needed to sync them with a WebDAV folder.
#pragma once
#include <ctime>
#include <string>
#include <vector>

struct Note {
    std::string id;     // local file name stem
    std::string href;   // path of the .md file on the server, empty if never uploaded
    std::string etag;   // server version our copy is based on
    std::string title;  // first line of the text without Markdown heading marks (cached for the list)
    time_t mtime = 0;
    bool dirty = false;    // changed locally since the last sync
    bool deleted = false;  // deleted locally, deletion not yet sent to the server
};

class NotesStore {
public:
    void load();
    // Visible notes, newest first. Pointers stay valid until the next mutating call.
    std::vector<Note*> list();
    std::vector<Note>& all() { return notes_; }
    Note* find(const std::string& id);
    int unsyncedCount() const;

    Note* create(const std::string& text);  // new local note (dirty)
    std::string text(const Note& n) const;
    bool setText(Note& n, const std::string& text, bool markDirty = true);
    // User deleted the note: keeps a tombstone if the server still has it.
    void removeByUser(const std::string& id);
    // Remove every trace of the note.
    void purge(const std::string& id);
    // Forget all server state (used when the sync account changes): every note
    // will be uploaded as new, tombstones are dropped.
    void detachAll();
    bool save();  // write the index

    // "# Shopping list" -> "Shopping list"
    static std::string titleOf(const std::string& text);

private:
    std::vector<Note> notes_;
};
