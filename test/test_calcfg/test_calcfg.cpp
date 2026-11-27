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

void test_names_and_encodings() {
    TEST_ASSERT_TRUE(calcfg::isImportName("calendars.txt"));
    TEST_ASSERT_TRUE(calcfg::isImportName("/cardputerOS/Calendars.TXT"));
    TEST_ASSERT_TRUE(calcfg::isImportName("calendars.txt.txt"));
    TEST_ASSERT_TRUE(calcfg::isImportName("calendar.txt"));
    TEST_ASSERT_FALSE(calcfg::isImportName("._calendars.txt"));
    TEST_ASSERT_FALSE(calcfg::isImportName("calendars.rtf"));
    TEST_ASSERT_FALSE(calcfg::isImportName("1.ics"));

    std::string problem;
    // UTF-16 LE with BOM (Notepad "Unicode"), and without
    const std::string text = "Work https://a.example/x.ics\r\n";
    std::string le = "\xFF\xFE", nobom;
    for (char c : text) {
        le += c;
        le += '\0';
        nobom += c;
        nobom += '\0';
    }
    TEST_ASSERT_EQUAL_STRING(text.c_str(), calcfg::decode(le, problem).c_str());
    TEST_ASSERT_TRUE(problem.empty());
    TEST_ASSERT_EQUAL_STRING(text.c_str(), calcfg::decode(nobom, problem).c_str());
    std::string be = "\xFE\xFF";
    for (char c : text) {
        be += '\0';
        be += c;
    }
    TEST_ASSERT_EQUAL_STRING(text.c_str(), calcfg::decode(be, problem).c_str());
    TEST_ASSERT_EQUAL_STRING(text.c_str(), calcfg::decode(text, problem).c_str());
    TEST_ASSERT_EQUAL_STRING("", calcfg::decode("{\\rtf1\\ansi https://a.example/x.ics}", problem).c_str());
    TEST_ASSERT_FALSE(problem.empty());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse);
    RUN_TEST(test_names_and_encodings);
    return UNITY_END();
}
