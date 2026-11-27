#include <unity.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "calidx.h"
#include "ics.h"

void setUp() {}
void tearDown() {}

static int64_t utc(int y, int m, int d, int hh = 0) { return ics::daysFromCivil(y, m, d) * 86400 + hh * 3600; }

void test_records_and_text() {
    calidx::Record r;
    r.start = -5;
    r.end = utc(2026, 11, 27, 10);
    r.text = 0xA1B2C3D4u;
    r.allDay = true;
    uint8_t b[calidx::kRecordSize];
    calidx::encode(r, b);
    const calidx::Record d = calidx::decode(b);
    TEST_ASSERT_EQUAL(-5, (int)d.start);
    TEST_ASSERT_EQUAL((long long)r.end, (long long)d.end);
    TEST_ASSERT_TRUE(d.text == 0xA1B2C3D4u);
    TEST_ASSERT_TRUE(d.allDay);

    const std::string t = calidx::encodeText({"Meet\x1Fing", "Room 1", "Line\nTwo"}) + "next entry";
    calidx::Text out;
    TEST_ASSERT_TRUE(calidx::decodeText(t, out));
    TEST_ASSERT_EQUAL_STRING("Meet ing", out.summary.c_str());
    TEST_ASSERT_EQUAL_STRING("Room 1", out.location.c_str());
    TEST_ASSERT_EQUAL_STRING("Line\nTwo", out.description.c_str());
    TEST_ASSERT_TRUE(calidx::decodeText(t, out, false));
    TEST_ASSERT_EQUAL_STRING("", out.description.c_str());
    TEST_ASSERT_FALSE(calidx::decodeText("garbage", out));
}

void test_batch_writer() {
    std::vector<size_t> pieces;
    std::string all;
    calidx::BatchWriter w([&](const char* d, size_t n) {
        pieces.push_back(n);
        all.append(d, n);
        return true;
    }, 10);
    for (int i = 0; i < 7; i++) w.write("abc", 3);
    TEST_ASSERT_EQUAL(21, (int)w.written());
    TEST_ASSERT_EQUAL(2, (int)pieces.size());  // only full batches so far
    TEST_ASSERT_TRUE(w.flush());
    TEST_ASSERT_EQUAL(3, (int)pieces.size());
    TEST_ASSERT_EQUAL(10, (int)pieces[0]);
    TEST_ASSERT_EQUAL(1, (int)pieces[2]);
    TEST_ASSERT_EQUAL(21, (int)all.size());

    calidx::BatchWriter bad([](const char*, size_t) { return false; }, 4);
    bad.write("abcdef", 6);
    TEST_ASSERT_FALSE(bad.ok());
}

void test_build_and_scan() {
    const std::string ics =
        "BEGIN:VCALENDAR\r\nX-WR-CALNAME:Team\r\n"
        "BEGIN:VEVENT\r\nUID:w\r\nSUMMARY:Weekly\r\nLOCATION:Room\r\nDESCRIPTION:Agenda\\nitems\r\n"
        "DTSTART:20261102T090000Z\r\nDTEND:20261102T100000Z\r\nRRULE:FREQ=WEEKLY\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:o\r\nSUMMARY:Old\r\nDTSTART:20200101T090000Z\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:h\r\nSUMMARY:Holiday\r\nDTSTART;VALUE=DATE:20261124\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    size_t pos = 0;
    calidx::Source src;
    src.rewind = [&]() {
        pos = 0;
        return true;
    };
    src.read = [&](char* b, size_t max) {
        const size_t n = std::min<size_t>(std::min<size_t>(max, 17), ics.size() - pos);  // small pieces
        memcpy(b, ics.data() + pos, n);
        pos += n;
        return (int)n;
    };
    std::string occ, txt;
    calidx::BatchWriter wo([&](const char* d, size_t n) { occ.append(d, n); return true; }, 50);
    calidx::BatchWriter wt([&](const char* d, size_t n) { txt.append(d, n); return true; }, 50);
    const int64_t from = utc(2026, 11, 1), to = utc(2026, 12, 1);
    calidx::BuildResult r = calidx::build(src, wo, wt, from, to, 0, 1000, 300);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.isCalendar);
    TEST_ASSERT_FALSE(r.truncated);
    TEST_ASSERT_EQUAL_STRING("Team", r.name.c_str());
    TEST_ASSERT_EQUAL(6, (int)r.records);  // Mondays 2, 9, 16, 23, 30 November and the holiday
    TEST_ASSERT_EQUAL(6 * (int)calidx::kRecordSize, (int)occ.size());

    // a page of a week: 16..22 November (one Monday) plus the holiday on the 24th is outside
    size_t rp = 0;
    auto reader = [&](uint8_t* b, size_t max) {
        const size_t n = std::min<size_t>(std::min<size_t>(max, 30), occ.size() - rp);  // odd piece sizes
        memcpy(b, occ.data() + rp, n);
        rp += n;
        return (int)n;
    };
    std::vector<calidx::Record> page;
    calidx::scan(reader, utc(2026, 11, 16), utc(2026, 11, 23), 100, page);
    TEST_ASSERT_EQUAL(1, (int)page.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 16, 9), (long long)page[0].start);
    calidx::Text t;
    TEST_ASSERT_TRUE(calidx::decodeText(txt.substr(page[0].text), t));
    TEST_ASSERT_EQUAL_STRING("Weekly", t.summary.c_str());
    TEST_ASSERT_EQUAL_STRING("Room", t.location.c_str());
    TEST_ASSERT_EQUAL_STRING("Agenda\nitems", t.description.c_str());

    rp = 0;
    page.clear();
    calidx::scan(reader, utc(2026, 11, 24), utc(2026, 11, 25), 100, page);
    TEST_ASSERT_EQUAL(1, (int)page.size());
    TEST_ASSERT_TRUE(page[0].allDay);
    TEST_ASSERT_TRUE(calidx::decodeText(txt.substr(page[0].text), t));
    TEST_ASSERT_EQUAL_STRING("Holiday", t.summary.c_str());

    // the text of the weekly event is stored once
    TEST_ASSERT_EQUAL(1, (int)std::count(txt.begin(), txt.end(), 'W'));

    // record limit
    std::string occ2, txt2;
    calidx::BatchWriter wo2([&](const char* d, size_t n) { occ2.append(d, n); return true; });
    calidx::BatchWriter wt2([&](const char* d, size_t n) { txt2.append(d, n); return true; });
    r = calidx::build(src, wo2, wt2, from, to, 0, 2, 300);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.truncated);
    TEST_ASSERT_EQUAL(2, (int)r.records);

    // not a calendar
    const std::string html = "<html>nope</html>";
    calidx::Source s2;
    size_t p2 = 0;
    s2.rewind = [&]() { p2 = 0; return true; };
    s2.read = [&](char* b, size_t max) {
        const size_t n = std::min(max, html.size() - p2);
        memcpy(b, html.data() + p2, n);
        p2 += n;
        return (int)n;
    };
    r = calidx::build(s2, wo2, wt2, from, to, 0, 10, 300);
    TEST_ASSERT_FALSE(r.isCalendar);
    TEST_ASSERT_FALSE(r.ok);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_records_and_text);
    RUN_TEST(test_batch_writer);
    RUN_TEST(test_build_and_scan);
    return UNITY_END();
}
