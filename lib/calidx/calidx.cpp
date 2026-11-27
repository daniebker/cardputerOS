#include "calidx.h"

#include <algorithm>
#include <cstring>

#include "ics.h"

namespace calidx {

namespace {
const char kFieldSep = '\x1F', kEnd = '\x1E';

void put64(uint8_t* p, int64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((uint64_t)v >> (8 * i));
}
int64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return (int64_t)v;
}

std::string clean(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c == kFieldSep || c == kEnd) c = ' ';
    return out;
}
}  // namespace

void encode(const Record& r, uint8_t out[kRecordSize]) {
    memset(out, 0, kRecordSize);
    put64(out, r.start);
    put64(out + 8, r.end);
    for (int i = 0; i < 4; i++) out[16 + i] = (uint8_t)(r.text >> (8 * i));
    out[20] = r.allDay ? 1 : 0;
}

Record decode(const uint8_t in[kRecordSize]) {
    Record r;
    r.start = get64(in);
    r.end = get64(in + 8);
    r.text = (uint32_t)in[16] | (uint32_t)in[17] << 8 | (uint32_t)in[18] << 16 | (uint32_t)in[19] << 24;
    r.allDay = in[20] != 0;
    return r;
}

std::string encodeText(const Text& t) {
    return clean(t.summary) + kFieldSep + clean(t.location) + kFieldSep + clean(t.description) + kEnd;
}

bool decodeText(const std::string& raw, Text& out, bool withDescription) {
    out = Text();
    const size_t end = raw.find(kEnd);
    const size_t a = raw.find(kFieldSep);
    if (a == std::string::npos || (end != std::string::npos && a > end)) return false;
    const size_t b = raw.find(kFieldSep, a + 1);
    if (b == std::string::npos || (end != std::string::npos && b > end)) return false;
    out.summary = raw.substr(0, a);
    out.location = raw.substr(a + 1, b - a - 1);
    if (withDescription) out.description = raw.substr(b + 1, end == std::string::npos ? std::string::npos : end - b - 1);
    return true;
}

size_t maxTextBytes(size_t maxText) { return 3 * maxText + 4; }

// ---------------------------------------------------------------- BatchWriter

BatchWriter::BatchWriter(Sink sink, size_t size) : sink_(std::move(sink)), size_(size ? size : 1) { buf_.reserve(size_); }

bool BatchWriter::write(const void* data, size_t len) {
    const char* p = (const char*)data;
    written_ += len;
    while (len > 0 && ok_) {
        const size_t take = std::min(len, size_ - buf_.size());
        buf_.append(p, take);
        p += take;
        len -= take;
        if (buf_.size() >= size_) flush();
    }
    return ok_;
}

bool BatchWriter::flush() {
    if (ok_ && !buf_.empty() && !sink_(buf_.data(), buf_.size())) ok_ = false;
    buf_.clear();
    return ok_;
}

// ---------------------------------------------------------------- build

BuildResult build(const Source& src, BatchWriter& occ, BatchWriter& txt, int64_t from, int64_t to, int localOffset,
                  uint32_t maxRecords, size_t maxText) {
    BuildResult res;
    char buf[1024];
    auto feedAll = [&](ics::Calendar& c) {
        if (!src.rewind()) return false;
        for (;;) {
            const int n = src.read(buf, sizeof buf);
            if (n <= 0) break;
            c.feed(buf, (size_t)n);
        }
        c.finish();
        return true;
    };

    // pass 1: time zones and replaced occurrences
    ics::Calendar pass1;
    pass1.collectOnly = true;
    pass1.maxText = maxText;
    if (!feedAll(pass1)) return res;
    res.isCalendar = pass1.sawCalendar();
    res.name = pass1.name();
    if (!res.isCalendar) return res;

    // pass 2: one event at a time
    ics::Calendar pass2;
    pass2.adoptZones(pass1);
    pass2.maxText = maxText;
    pass2.keepAfter = from;
    pass2.keepBefore = to;
    pass2.onEvent = [&](const ics::Event& e) {
        if (res.truncated) return;
        uint32_t textAt = UINT32_MAX;
        pass2.expandEvent(e, 0, from, to, localOffset, [&](const ics::Occurrence& o) {
            if (res.records >= maxRecords) {
                res.truncated = true;
                return false;
            }
            if (textAt == UINT32_MAX) {
                textAt = (uint32_t)txt.written();
                const std::string t = encodeText({e.summary, e.location, e.description});
                txt.write(t.data(), t.size());
            }
            Record r;
            r.start = o.start;
            r.end = o.end;
            r.allDay = o.allDay;
            r.text = textAt;
            uint8_t rec[kRecordSize];
            encode(r, rec);
            occ.write(rec, sizeof rec);
            res.records++;
            return occ.ok() && txt.ok();
        });
    };
    if (!feedAll(pass2)) return res;
    res.ok = occ.flush() && txt.flush();
    return res;
}

// ---------------------------------------------------------------- scan

void scan(const std::function<int(uint8_t*, size_t)>& read, int64_t from, int64_t to, size_t max,
          std::vector<Record>& out) {
    uint8_t buf[64 * kRecordSize];
    size_t have = 0;
    for (;;) {
        const int n = read(buf + have, sizeof buf - have);
        if (n <= 0) break;
        have += (size_t)n;
        size_t used = 0;
        for (; used + kRecordSize <= have; used += kRecordSize) {
            const Record r = decode(buf + used);
            const bool overlaps = r.start < to && (r.end > from || (r.end == r.start && r.start >= from));
            if (!overlaps) continue;
            if (out.size() >= max) return;
            out.push_back(r);
        }
        memmove(buf, buf + used, have - used);
        have -= used;
    }
}

}  // namespace calidx
