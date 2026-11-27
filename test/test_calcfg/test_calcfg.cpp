#include <unity.h>

#include "calcfg.h"

void setUp() {}
void tearDown() {}

void test_parse() {
    const calcfg::File f = calcfg::parse(
        "\xEF\xBB\xBF# my calendars\r\n"
        "\r\n"
        "Work   https://calendar.google.com/calendar/ical/abc%40group/private-123/basic.ics\r\n"
        "Holidays: webcal://example.com/holidays.ics   trailing words\n"
        "  https://example.com/plain.ics?token=a=b \n"
        "\"Team: Ops\" = WEBCALS://ops.example/x.ics\n"
        "just some words\n"
        "   # indented comment\n");
    TEST_ASSERT_EQUAL(4, (int)f.calendars.size());
    TEST_ASSERT_EQUAL_STRING("Work", f.calendars[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("https://calendar.google.com/calendar/ical/abc%40group/private-123/basic.ics",
                             f.calendars[0].url.c_str());
    TEST_ASSERT_EQUAL_STRING("Holidays", f.calendars[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("webcal://example.com/holidays.ics", f.calendars[1].url.c_str());
    TEST_ASSERT_EQUAL_STRING("", f.calendars[2].name.c_str());
    TEST_ASSERT_EQUAL_STRING("https://example.com/plain.ics?token=a=b", f.calendars[2].url.c_str());
    TEST_ASSERT_EQUAL_STRING("Team: Ops", f.calendars[3].name.c_str());
    TEST_ASSERT_EQUAL_STRING("WEBCALS://ops.example/x.ics", f.calendars[3].url.c_str());
    TEST_ASSERT_EQUAL(1, (int)f.unreadable.size());
    TEST_ASSERT_EQUAL_STRING("just some words", f.unreadable[0].c_str());

    TEST_ASSERT_EQUAL(0, (int)calcfg::parse("").calendars.size());
    const std::string left = calcfg::leftover({"x"}, "Not imported:");
    TEST_ASSERT_EQUAL(1, (int)calcfg::parse(left).unreadable.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse);
    return UNITY_END();
}
