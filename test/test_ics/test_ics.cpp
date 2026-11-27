#include <unity.h>

#include <string>
#include <vector>

#include "ics.h"

using namespace ics;

void setUp() {}
void tearDown() {}

static int64_t utc(int y, int m, int d, int hh = 0, int mm = 0) { return daysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60; }

static Calendar parse(const std::string& text, size_t chunk = 7) {
    Calendar c;
    for (size_t i = 0; i < text.size(); i += chunk) c.feed(text.data() + i, std::min(chunk, text.size() - i));
    c.finish();
    return c;
}

static const char* kBerlin =
    "BEGIN:VTIMEZONE\r\n"
    "TZID:Europe/Berlin\r\n"
    "BEGIN:DAYLIGHT\r\n"
    "TZOFFSETFROM:+0100\r\nTZOFFSETTO:+0200\r\nTZNAME:CEST\r\n"
    "DTSTART:19700329T020000\r\nRRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\n"
    "END:DAYLIGHT\r\n"
    "BEGIN:STANDARD\r\n"
    "TZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\nTZNAME:CET\r\n"
    "DTSTART:19701025T030000\r\nRRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n";

void test_dates() {
    TEST_ASSERT_EQUAL(0, (int)daysFromCivil(1970, 1, 1));
    int y, m, d;
    civilFromDays(daysFromCivil(2024, 2, 29), y, m, d);
    TEST_ASSERT_EQUAL(2024, y);
    TEST_ASSERT_EQUAL(2, m);
    TEST_ASSERT_EQUAL(29, d);
    TEST_ASSERT_EQUAL(5, weekday(daysFromCivil(2026, 11, 27)));  // a Friday
    TEST_ASSERT_EQUAL(29, daysInMonth(2000, 2));
    TEST_ASSERT_EQUAL(28, daysInMonth(1900, 2));

    DateTime dt;
    TEST_ASSERT_TRUE(parseDateTime("20261127T093000Z", dt));
    TEST_ASSERT_TRUE(dt.utc);
    TEST_ASSERT_EQUAL(9, dt.hh);
    TEST_ASSERT_TRUE(parseDateTime("20261127", dt));
    TEST_ASSERT_TRUE(dt.dateOnly);
    TEST_ASSERT_FALSE(parseDateTime("20261332", dt));

    int64_t s;
    TEST_ASSERT_TRUE(parseDuration("PT1H30M", s));
    TEST_ASSERT_EQUAL(5400, (int)s);
    TEST_ASSERT_TRUE(parseDuration("P1W", s));
    TEST_ASSERT_EQUAL(7 * 86400, (int)s);
    TEST_ASSERT_TRUE(parseDuration("-P1DT2H", s));
    TEST_ASSERT_EQUAL(-(86400 + 7200), (int)s);
    TEST_ASSERT_FALSE(parseDuration("1H", s));

    int off;
    TEST_ASSERT_TRUE(parseOffset("-0530", off));
    TEST_ASSERT_EQUAL(-(5 * 3600 + 30 * 60), off);
    TEST_ASSERT_EQUAL_STRING("a, b; c\nd\\e", unescapeText("a\\, b\\; c\\nd\\\\e").c_str());
}

void test_parse_fields_folding_and_any_chunking() {
    const std::string text =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nX-WR-CALNAME:Work\r\n"
        "BEGIN:VEVENT\r\nUID:1@x\r\n"
        "SUMMARY:Team meeting\\, weekly\r\n"
        "DESCRIPTION:Line one\\nLine two that is \r\n  folded\r\n"
        "LOCATION;LANGUAGE=en:Room \"4\"\r\n"
        "DTSTART:20261127T090000Z\r\nDTEND:20261127T100000Z\r\n"
        "BEGIN:VALARM\r\nDESCRIPTION:alarm text\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\n"
        "END:VEVENT\r\nEND:VCALENDAR\r\n";
    for (size_t chunk : {1u, 2u, 5u, 64u, 10000u}) {
        Calendar c = parse(text, chunk);
        TEST_ASSERT_TRUE(c.sawCalendar());
        TEST_ASSERT_EQUAL_STRING("Work", c.name().c_str());
        TEST_ASSERT_EQUAL(1, (int)c.events().size());
        const Event& e = c.events()[0];
        TEST_ASSERT_EQUAL_STRING("Team meeting, weekly", e.summary.c_str());
        TEST_ASSERT_EQUAL_STRING("Line one\nLine two that is  folded", e.description.c_str());
        TEST_ASSERT_EQUAL_STRING("Room \"4\"", e.location.c_str());
        auto occ = c.expand(utc(2026, 11, 1), utc(2026, 12, 1), 0, 100);
        TEST_ASSERT_EQUAL(1, (int)occ.size());
        TEST_ASSERT_EQUAL((long long)utc(2026, 11, 27, 9), (long long)occ[0].start);
        TEST_ASSERT_EQUAL((long long)utc(2026, 11, 27, 10), (long long)occ[0].end);
    }
    // plain LF line endings and a missing final newline work too
    Calendar c = parse("BEGIN:VCALENDAR\nBEGIN:VEVENT\nSUMMARY:x\nDTSTART:20261127T090000Z\nEND:VEVENT\nEND:VCALENDAR");
    TEST_ASSERT_EQUAL(1, (int)c.events().size());
    TEST_ASSERT_FALSE(parse("<html>Not found</html>").sawCalendar());
}

void test_all_day_and_floating_use_local_offset() {
    const std::string text =
        "BEGIN:VCALENDAR\r\n"
        "BEGIN:VEVENT\r\nUID:a\r\nSUMMARY:Holiday\r\nDTSTART;VALUE=DATE:20261224\r\nDTEND;VALUE=DATE:20261227\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b\r\nSUMMARY:Floating\r\nDTSTART:20261224T120000\r\nDURATION:PT30M\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    Calendar c = parse(text);
    const int off = 3600;  // device at UTC+1
    auto occ = c.expand(utc(2026, 12, 1), utc(2027, 1, 1), off, 100);
    TEST_ASSERT_EQUAL(2, (int)occ.size());
    TEST_ASSERT_TRUE(occ[0].allDay);
    TEST_ASSERT_EQUAL((long long)(utc(2026, 12, 24) - off), (long long)occ[0].start);
    TEST_ASSERT_EQUAL((long long)(utc(2026, 12, 27) - off), (long long)occ[0].end);
    TEST_ASSERT_FALSE(occ[1].allDay);
    TEST_ASSERT_EQUAL((long long)(utc(2026, 12, 24, 12) - off), (long long)occ[1].start);
    TEST_ASSERT_EQUAL(1800, (int)(occ[1].end - occ[1].start));
}

void test_tzid_with_vtimezone_and_dst() {
    const std::string text = std::string("BEGIN:VCALENDAR\r\n") + kBerlin +
                             "BEGIN:VEVENT\r\nUID:w\r\nSUMMARY:Weekly\r\n"
                             "DTSTART;TZID=Europe/Berlin:20261019T090000\r\nDTEND;TZID=Europe/Berlin:20261019T100000\r\n"
                             "RRULE:FREQ=WEEKLY;COUNT=3\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    Calendar c = parse(text);
    auto occ = c.expand(utc(2026, 10, 1), utc(2026, 12, 1), 0, 100);
    TEST_ASSERT_EQUAL(3, (int)occ.size());
    // 09:00 summer time (UTC+2), then 09:00 winter time (UTC+1) after 25 October
    TEST_ASSERT_EQUAL((long long)utc(2026, 10, 19, 7), (long long)occ[0].start);
    TEST_ASSERT_EQUAL((long long)utc(2026, 10, 26, 8), (long long)occ[1].start);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 2, 8), (long long)occ[2].start);
    TEST_ASSERT_EQUAL(3600, (int)(occ[2].end - occ[2].start));

    // a zone that is not defined in the file: UTC names are known, others use the device's
    Calendar u = parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART;TZID=UTC:20261127T090000\r\nEND:VEVENT\r\n"
                       "BEGIN:VEVENT\r\nDTSTART;TZID=Mars/Olympus:20261127T090000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    occ = u.expand(utc(2026, 11, 1), utc(2026, 12, 1), 7200, 100);
    TEST_ASSERT_EQUAL(2, (int)occ.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 27, 7), (long long)occ[0].start);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 27, 9), (long long)occ[1].start);
}

static std::vector<int64_t> starts(const std::string& rrule, const char* dtstart, int64_t from, int64_t to,
                                   const std::string& extra = "") {
    const std::string text = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:r\r\nDTSTART:" + std::string(dtstart) +
                             "\r\nRRULE:" + rrule + "\r\n" + extra + "END:VEVENT\r\nEND:VCALENDAR\r\n";
    Calendar c = parse(text);
    std::vector<int64_t> out;
    for (auto& o : c.expand(from, to, 0, 1000)) out.push_back(o.start);
    return out;
}

void test_rules() {
    // daily, every 2 days, until (inclusive)
    auto s = starts("FREQ=DAILY;INTERVAL=2;UNTIL=20261107T090000Z", "20261101T090000Z", utc(2026, 1, 1), utc(2027, 1, 1));
    TEST_ASSERT_EQUAL(4, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 7, 9), (long long)s[3]);

    // weekly on Mon/Wed/Fri, count 5, starting on a Wednesday
    s = starts("FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=5", "20261104T080000Z", utc(2026, 1, 1), utc(2027, 1, 1));
    TEST_ASSERT_EQUAL(5, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 4, 8), (long long)s[0]);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 6, 8), (long long)s[1]);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 9, 8), (long long)s[2]);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 13, 8), (long long)s[4]);

    // every other week
    s = starts("FREQ=WEEKLY;INTERVAL=2", "20261102T080000Z", utc(2026, 11, 1), utc(2026, 12, 1));
    TEST_ASSERT_EQUAL(3, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 16, 8), (long long)s[1]);

    // monthly on the last Friday
    s = starts("FREQ=MONTHLY;BYDAY=-1FR", "20260130T100000Z", utc(2026, 11, 1), utc(2027, 1, 1));
    TEST_ASSERT_EQUAL(2, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 27, 10), (long long)s[0]);
    TEST_ASSERT_EQUAL((long long)utc(2026, 12, 25, 10), (long long)s[1]);

    // monthly on the 31st skips short months
    s = starts("FREQ=MONTHLY", "20260131T100000Z", utc(2026, 1, 1), utc(2026, 6, 1));
    TEST_ASSERT_EQUAL(3, (int)s.size());  // Jan, Mar, May

    // last working day of the month (BYSETPOS)
    s = starts("FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1", "20260130T170000Z", utc(2026, 10, 1), utc(2026, 11, 1));
    TEST_ASSERT_EQUAL(1, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 10, 30, 17), (long long)s[0]);

    // yearly birthday, started long ago, all-day
    s = starts("FREQ=YEARLY", "19920229", utc(2026, 1, 1), utc(2029, 1, 1));
    TEST_ASSERT_EQUAL(1, (int)s.size());  // 29 February only exists in 2028
    s = starts("FREQ=YEARLY;BYMONTH=11;BYDAY=4TH", "20201126T120000Z", utc(2026, 1, 1), utc(2027, 1, 1));
    TEST_ASSERT_EQUAL(1, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 26, 12), (long long)s[0]);  // Thanksgiving

    // EXDATE removes an instance
    s = starts("FREQ=DAILY;COUNT=3", "20261101T090000Z", utc(2026, 1, 1), utc(2027, 1, 1), "EXDATE:20261102T090000Z\r\n");
    TEST_ASSERT_EQUAL(2, (int)s.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 3, 9), (long long)s[1]);

    // unsupported frequency: shown once
    s = starts("FREQ=HOURLY", "20261101T090000Z", utc(2026, 1, 1), utc(2027, 1, 1));
    TEST_ASSERT_EQUAL(1, (int)s.size());
}

void test_overrides_and_cancelled() {
    const std::string text =
        "BEGIN:VCALENDAR\r\n"
        "BEGIN:VEVENT\r\nUID:s\r\nSUMMARY:Standup\r\nDTSTART:20261123T090000Z\r\nDURATION:PT15M\r\n"
        "RRULE:FREQ=DAILY;COUNT=5\r\nEND:VEVENT\r\n"
        // moved instance
        "BEGIN:VEVENT\r\nUID:s\r\nSUMMARY:Standup (late)\r\nRECURRENCE-ID:20261124T090000Z\r\n"
        "DTSTART:20261124T110000Z\r\nDTEND:20261124T111500Z\r\nEND:VEVENT\r\n"
        // cancelled instance
        "BEGIN:VEVENT\r\nUID:s\r\nRECURRENCE-ID:20261125T090000Z\r\nDTSTART:20261125T090000Z\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:c\r\nSUMMARY:Gone\r\nDTSTART:20261123T100000Z\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    Calendar c = parse(text);
    auto occ = c.expand(utc(2026, 11, 1), utc(2026, 12, 1), 0, 100);
    TEST_ASSERT_EQUAL(4, (int)occ.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 23, 9), (long long)occ[0].start);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 24, 11), (long long)occ[1].start);
    TEST_ASSERT_EQUAL_STRING("Standup (late)", c.events()[occ[1].event].summary.c_str());
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 26, 9), (long long)occ[2].start);
    TEST_ASSERT_EQUAL((long long)utc(2026, 11, 27, 9), (long long)occ[3].start);
}

void test_window_limits_and_memory_bounds() {
    std::string text = "BEGIN:VCALENDAR\r\n";
    for (int d = 1; d <= 28; d++) {
        char buf[200];
        snprintf(buf, sizeof buf, "BEGIN:VEVENT\r\nUID:%d\r\nSUMMARY:Day %d\r\nDTSTART:202602%02dT120000Z\r\nEND:VEVENT\r\n", d, d, d);
        text += buf;
    }
    text += "BEGIN:VEVENT\r\nUID:long\r\nSUMMARY:" + std::string(1000, 'x') + "\r\nDTSTART:20260220T120000Z\r\nEND:VEVENT\r\n";
    text += "END:VCALENDAR\r\n";
    Calendar c;
    c.keepAfter = utc(2026, 2, 20);
    c.maxText = 50;
    c.feed(text.data(), text.size());
    c.finish();
    // events that ended more than two days before keepAfter are dropped
    TEST_ASSERT_EQUAL(28 - 17 + 1, (int)c.events().size());
    TEST_ASSERT_EQUAL(50, (int)c.events().back().summary.size());
    // overlapping the window, sorted, cut to maxCount
    auto occ = c.expand(utc(2026, 2, 20), utc(2026, 3, 1), 0, 5);
    TEST_ASSERT_EQUAL(5, (int)occ.size());
    TEST_ASSERT_EQUAL((long long)utc(2026, 2, 20, 12), (long long)occ[0].start);

    Calendar small;
    small.maxEvents = 3;
    small.feed(text.data(), text.size());
    small.finish();
    TEST_ASSERT_EQUAL(3, (int)small.events().size());
    TEST_ASSERT_TRUE(small.truncated());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_dates);
    RUN_TEST(test_parse_fields_folding_and_any_chunking);
    RUN_TEST(test_all_day_and_floating_use_local_offset);
    RUN_TEST(test_tzid_with_vtimezone_and_dst);
    RUN_TEST(test_rules);
    RUN_TEST(test_overrides_and_cancelled);
    RUN_TEST(test_window_limits_and_memory_bounds);
    return UNITY_END();
}
