#include <unity.h>

#include <string>

#include "journal.h"

using namespace journal;

void setUp() {}
void tearDown() {}

void test_add_entries() {
    std::string f = addEntry("", "2026-11-12", 9 * 60 + 5, "Woke up");
    TEST_ASSERT_EQUAL_STRING("# 2026-11-12\n\n- **09:05** Woke up\n", f.c_str());
    f = addEntry(f, "2026-11-12", 14 * 60 + 32, "Lunch\nwith  tabs\there ");
    TEST_ASSERT_EQUAL_STRING("# 2026-11-12\n\n- **09:05** Woke up\n- **14:32** Lunch with  tabs here\n", f.c_str());
    // an entry with an earlier time (clock was corrected) goes where it belongs
    f = addEntry(f, "2026-11-12", 10 * 60, "Mid-morning");
    TEST_ASSERT_EQUAL_STRING("# 2026-11-12\n\n- **09:05** Woke up\n- **10:00** Mid-morning\n- **14:32** Lunch with  tabs here\n", f.c_str());
    // same minute: after the existing one
    f = addEntry(f, "2026-11-12", 10 * 60, "Second at ten");
    auto e = entries(f);
    TEST_ASSERT_EQUAL(4, (int)e.size());
    TEST_ASSERT_EQUAL_STRING("Mid-morning", e[1].text.c_str());
    TEST_ASSERT_EQUAL_STRING("Second at ten", e[2].text.c_str());
    TEST_ASSERT_EQUAL(600, e[2].minutes);
    TEST_ASSERT_EQUAL(14 * 60 + 32, e[3].minutes);
}

void test_parse_keeps_foreign_content() {
    const std::string f =
        "# 2026-11-12\r\n\r\nA paragraph written elsewhere.\r\n\r\n"
        "- **08:00** first\r\n  continued on a second line\r\n- not an entry\r\n- **25:00** not a time either\r\n"
        "- **09:30** second\r\n\r\n## Section added in another app\r\n";
    Day d = parse(f);
    TEST_ASSERT_EQUAL_STRING("# 2026-11-12\n\nA paragraph written elsewhere.", d.preamble.c_str());
    TEST_ASSERT_EQUAL(2, (int)d.blocks.size());
    TEST_ASSERT_EQUAL_STRING("- **08:00** first\n  continued on a second line\n- not an entry\n- **25:00** not a time either",
                             d.blocks[0].c_str());
    TEST_ASSERT_EQUAL_STRING("- **09:30** second\n\n## Section added in another app", d.blocks[1].c_str());
    // formatting what was parsed and parsing again changes nothing more
    TEST_ASSERT_EQUAL_STRING(format(d).c_str(), format(parse(format(d))).c_str());
    auto e = entries(f);
    TEST_ASSERT_EQUAL_STRING("first\n  continued on a second line\n- not an entry\n- **25:00** not a time either", e[0].text.c_str());
    TEST_ASSERT_EQUAL(0, (int)entries("").size());
    TEST_ASSERT_EQUAL(0, (int)entries("just some text\n").size());
}

void test_remove_last() {
    std::string f = addEntry(addEntry("", "2026-01-01", 60, "a"), "2026-01-01", 120, "b");
    TEST_ASSERT_TRUE(removeLastEntry(f));
    TEST_ASSERT_EQUAL_STRING("# 2026-01-01\n\n- **01:00** a\n", f.c_str());
    TEST_ASSERT_TRUE(removeLastEntry(f));
    TEST_ASSERT_EQUAL_STRING("# 2026-01-01\n\n", f.c_str());
    TEST_ASSERT_FALSE(removeLastEntry(f));
}

void test_merge() {
    const std::string base = addEntry(addEntry("", "2026-11-12", 8 * 60, "shared"), "2026-11-12", 9 * 60, "also shared");
    const std::string local = addEntry(base, "2026-11-12", 12 * 60, "written on the device");
    const std::string remote = addEntry(addEntry(base, "2026-11-12", 10 * 60, "written on the phone"), "2026-11-12", 13 * 60, "phone again");
    const std::string m = merge(local, remote);
    TEST_ASSERT_EQUAL_STRING(
        "# 2026-11-12\n\n- **08:00** shared\n- **09:00** also shared\n- **10:00** written on the phone\n"
        "- **12:00** written on the device\n- **13:00** phone again\n",
        m.c_str());
    // merging is stable: doing it again, or the other way round, adds nothing
    TEST_ASSERT_EQUAL_STRING(m.c_str(), merge(m, remote).c_str());
    TEST_ASSERT_EQUAL_STRING(m.c_str(), merge(local, m).c_str());
    TEST_ASSERT_EQUAL_STRING(m.c_str(), merge(remote, local).c_str());
    // an entry edited on one side is kept in both versions rather than lost
    std::string edited = remote;
    edited.replace(edited.find("written on the phone"), 20, "edited on the phone!");
    const std::string m2 = merge(m, edited);
    TEST_ASSERT_TRUE(m2.find("written on the phone") != std::string::npos);
    TEST_ASSERT_TRUE(m2.find("edited on the phone!") != std::string::npos);
    // one side empty
    TEST_ASSERT_EQUAL_STRING(local.c_str(), merge(local, "").c_str());
    TEST_ASSERT_EQUAL_STRING(remote.c_str(), merge("", remote).c_str());
    // the remote title wins, the local one is used if the remote has none
    TEST_ASSERT_TRUE(merge("# mine\n\n- **01:00** x\n", "# theirs\n\n- **02:00** y\n").compare(0, 8, "# theirs") == 0);
    TEST_ASSERT_TRUE(merge("# mine\n\n- **01:00** x\n", "- **02:00** y\n").compare(0, 6, "# mine") == 0);
}

void test_dates() {
    TEST_ASSERT_TRUE(validDate("2026-11-12"));
    TEST_ASSERT_FALSE(validDate("2026-13-12"));
    TEST_ASSERT_FALSE(validDate("2026-11-00"));
    TEST_ASSERT_FALSE(validDate("2026-1-12"));
    TEST_ASSERT_FALSE(validDate("notes"));
    TEST_ASSERT_FALSE(validDate("2026-11-12.md"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_add_entries);
    RUN_TEST(test_parse_keeps_foreign_content);
    RUN_TEST(test_remove_last);
    RUN_TEST(test_merge);
    RUN_TEST(test_dates);
    return UNITY_END();
}
