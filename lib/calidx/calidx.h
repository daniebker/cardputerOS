// The agenda index of a calendar, kept on the SD card next to its .ics so that
// a calendar never has to be held in memory as a whole:
//
//   <n>.occ  one fixed-size record per occurrence in the index window: start,
//            end, all-day flag and where the event's text starts in <n>.txt
//   <n>.txt  the text of each event that occurs in the window, once:
//            summary 0x1F location 0x1F description 0x1E
//
// build() reads the .ics twice as a stream (time zones and replaced occurrences
// first, then one event at a time) and writes both files through BatchWriters,
// so its memory use does not depend on the size of the calendar. scan() reads
// the records of a page of days back in batches.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace calidx {

constexpr size_t kRecordSize = 24;

struct Record {
    int64_t start = 0, end = 0;  // UTC seconds
    uint32_t text = 0;           // offset in the .txt file
    bool allDay = false;
};
void encode(const Record& r, uint8_t out[kRecordSize]);
Record decode(const uint8_t in[kRecordSize]);

struct Text {
    std::string summary, location, description;
};
std::string encodeText(const Text& t);
// `raw`: the .txt file from the record's offset on (at least up to the end mark).
// `withDescription` false skips the description (for agenda rows).
bool decodeText(const std::string& raw, Text& out, bool withDescription = true);
// Bytes to read from a text offset to be sure to have the whole entry.
size_t maxTextBytes(size_t maxText);

// Collects small writes and passes them on to `sink` in pieces of `size` bytes
// (SD cards are much faster with few large writes than with many small ones).
class BatchWriter {
public:
    using Sink = std::function<bool(const char*, size_t)>;
    explicit BatchWriter(Sink sink, size_t size = 4096);
    bool write(const void* data, size_t len);
    bool flush();
    size_t written() const { return written_; }  // bytes accepted so far (the file offset)
    bool ok() const { return ok_; }

private:
    Sink sink_;
    std::string buf_;
    size_t size_, written_ = 0;
    bool ok_ = true;
};

struct Source {
    std::function<bool()> rewind;                 // back to the start of the .ics
    std::function<int(char*, size_t)> read;       // bytes read, <= 0 at the end
};

struct BuildResult {
    bool ok = false;          // both files written
    bool isCalendar = false;  // the source is an iCalendar file
    bool truncated = false;   // more than maxRecords occurrences: the later ones are missing
    uint32_t records = 0;
    std::string name;         // X-WR-CALNAME
};

// Occurrences overlapping [from, to) (UTC seconds), floating and all-day times
// in `localOffset`. Flushes both writers at the end.
BuildResult build(const Source& src, BatchWriter& occ, BatchWriter& txt, int64_t from, int64_t to, int localOffset,
                  uint32_t maxRecords, size_t maxText);

// Reads records in batches and keeps those overlapping [from, to) (at most `max`).
// `read` returns the bytes read (<= 0 at the end of the file).
void scan(const std::function<int(uint8_t*, size_t)>& read, int64_t from, int64_t to, size_t max,
          std::vector<Record>& out);

}  // namespace calidx
