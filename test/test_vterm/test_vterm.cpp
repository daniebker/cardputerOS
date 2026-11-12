#include <unity.h>

#include <string>

#include "vterm.h"

static std::string rowText(const VTerm& t, int y) {
    std::string s;
    for (int x = 0; x < t.cols(); x++) s.push_back((char)t.cell(x, y).ch);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void setUp() {}
void tearDown() {}

void test_plain_text_and_wrap() {
    VTerm t(10, 4);
    t.write("hello\r\nworld");
    TEST_ASSERT_EQUAL_STRING("hello", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("world", rowText(t, 1).c_str());
    TEST_ASSERT_EQUAL(5, t.cursorX());
    TEST_ASSERT_EQUAL(1, t.cursorY());
    t.write("\r\n0123456789AB");
    TEST_ASSERT_EQUAL_STRING("0123456789", rowText(t, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("AB", rowText(t, 3).c_str());
}

void test_pending_wrap_does_not_scroll_early() {
    VTerm t(5, 2);
    t.write("abcde");  // fills the row, cursor stays on it
    TEST_ASSERT_EQUAL(0, t.cursorY());
    t.write("\r\nfghij");
    TEST_ASSERT_EQUAL_STRING("abcde", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("fghij", rowText(t, 1).c_str());
}

void test_scroll_and_scrollback() {
    VTerm t(8, 3, 10);
    t.write("1\r\n2\r\n3\r\n4\r\n5");
    TEST_ASSERT_EQUAL_STRING("3", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("5", rowText(t, 2).c_str());
    TEST_ASSERT_EQUAL(2, t.scrollbackSize());
    t.setViewOffset(2);
    TEST_ASSERT_EQUAL_STRING("1", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("2", rowText(t, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("3", rowText(t, 2).c_str());
    TEST_ASSERT_FALSE(t.cursorVisible());
    t.setViewOffset(99);
    TEST_ASSERT_EQUAL(2, t.viewOffset());
    t.setViewOffset(0);
    TEST_ASSERT_EQUAL_STRING("3", rowText(t, 0).c_str());
}

void test_cursor_movement_and_erase() {
    VTerm t(10, 5);
    t.write("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    t.write("\x1b[2;4H\x1b[K");
    TEST_ASSERT_EQUAL_STRING("bbb", rowText(t, 1).c_str());
    t.write("\x1b[1;1H\x1b[2J");
    TEST_ASSERT_EQUAL_STRING("", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("", rowText(t, 2).c_str());
    t.write("\x1b[3;3Hx\x1b[A\x1b[Dy\x1b[2Bz");
    TEST_ASSERT_EQUAL('x', t.cell(2, 2).ch);
    TEST_ASSERT_EQUAL('y', t.cell(2, 1).ch);
    TEST_ASSERT_EQUAL('z', t.cell(3, 3).ch);
}

void test_insert_delete_chars_and_lines() {
    VTerm t(10, 4);
    t.write("abcdef\x1b[1;3H\x1b[2P");
    TEST_ASSERT_EQUAL_STRING("abef", rowText(t, 0).c_str());
    t.write("\x1b[2@");
    TEST_ASSERT_EQUAL_STRING("ab  ef", rowText(t, 0).c_str());
    t.write("\x1b[2;1Hline2\x1b[3;1Hline3\x1b[1;1H\x1b[L");
    TEST_ASSERT_EQUAL_STRING("", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("ab  ef", rowText(t, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("line2", rowText(t, 2).c_str());
    t.write("\x1b[2M");
    TEST_ASSERT_EQUAL_STRING("line2", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("line3", rowText(t, 1).c_str());
}

void test_scroll_region() {
    VTerm t(6, 5, 10);
    t.write("A\r\nB\r\nC\r\nD\r\nE");
    t.write("\x1b[2;4r");        // region rows 2..4
    t.write("\x1b[4;1H\nX");     // LF at bottom of region scrolls only the region
    TEST_ASSERT_EQUAL_STRING("A", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("C", rowText(t, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("D", rowText(t, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("X", rowText(t, 3).c_str());
    TEST_ASSERT_EQUAL_STRING("E", rowText(t, 4).c_str());
    TEST_ASSERT_EQUAL(0, t.scrollbackSize());  // region scroll must not feed scrollback
    t.write("\x1b[2;1H\x1bM");   // reverse index at top of region
    TEST_ASSERT_EQUAL_STRING("", rowText(t, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("C", rowText(t, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("E", rowText(t, 4).c_str());
}

void test_sgr_colors() {
    VTerm t(20, 2);
    t.write("\x1b[1;31mR\x1b[0mN\x1b[38;5;200mP\x1b[48;2;255;0;0mT\x1b[7mI\x1b[m");
    TEST_ASSERT_EQUAL(1, t.cell(0, 0).fg);
    TEST_ASSERT_TRUE(t.cell(0, 0).attr & VTerm::Bold);
    TEST_ASSERT_FALSE(t.cell(0, 0).attr & VTerm::DefaultFg);
    TEST_ASSERT_TRUE(t.cell(1, 0).attr & VTerm::DefaultFg);
    TEST_ASSERT_EQUAL(200, t.cell(2, 0).fg);
    TEST_ASSERT_EQUAL(196, t.cell(3, 0).bg);
    TEST_ASSERT_TRUE(t.cell(4, 0).attr & VTerm::Inverse);
    t.write("\x1b[94;103mB");
    TEST_ASSERT_EQUAL(12, t.cell(5, 0).fg);
    TEST_ASSERT_EQUAL(11, t.cell(5, 0).bg);
}

void test_alt_screen() {
    VTerm t(10, 3, 5);
    t.write("main\x1b[?1049h");
    TEST_ASSERT_TRUE(t.altScreen());
    TEST_ASSERT_EQUAL_STRING("", rowText(t, 0).c_str());
    t.write("\x1b[Hvim\r\n\r\n\r\n\r\n");
    TEST_ASSERT_EQUAL(0, t.scrollbackSize());
    t.write("\x1b[?1049l");
    TEST_ASSERT_FALSE(t.altScreen());
    TEST_ASSERT_EQUAL_STRING("main", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL(4, t.cursorX());
}

void test_replies() {
    VTerm t(10, 5);
    std::string got;
    t.onReply([&](const char* d, size_t n) { got.append(d, n); });
    t.write("\x1b[3;4H\x1b[6n");
    TEST_ASSERT_EQUAL_STRING("\x1b[3;4R", got.c_str());
    got.clear();
    t.write("\x1b[c");
    TEST_ASSERT_EQUAL_STRING("\x1b[?1;2c", got.c_str());
}

void test_utf8_and_line_drawing() {
    VTerm t(10, 2);
    t.write("\xC3\xA9\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C\xE2\x82\xAC");  // é ─ │ ┌ €
    TEST_ASSERT_EQUAL('e', t.cell(0, 0).ch);
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmLeft | VTerm::ArmRight, t.cell(1, 0).ch);
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmUp | VTerm::ArmDown, t.cell(2, 0).ch);
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmDown | VTerm::ArmRight, t.cell(3, 0).ch);
    TEST_ASSERT_EQUAL('E', t.cell(4, 0).ch);
    t.write("\r\n\x1b(0lqk\x1b(Bq");  // DEC graphics
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmDown | VTerm::ArmRight, t.cell(0, 1).ch);
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmLeft | VTerm::ArmRight, t.cell(1, 1).ch);
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmDown | VTerm::ArmLeft, t.cell(2, 1).ch);
    TEST_ASSERT_EQUAL('q', t.cell(3, 1).ch);
}

void test_osc_title_and_ignored_strings() {
    VTerm t(10, 2);
    t.write("\x1b]0;my title\x07" "a\x1b]2;two\x1b\\b\x1bPignored\x1b\\c");
    TEST_ASSERT_EQUAL_STRING("two", t.title().c_str());
    TEST_ASSERT_EQUAL_STRING("abc", rowText(t, 0).c_str());
}

void test_split_sequences() {
    VTerm t(10, 2);
    t.write("\x1b");
    t.write("[3");
    t.write("1mX");
    t.write("\xE2\x94");
    t.write("\x80");
    TEST_ASSERT_EQUAL(1, t.cell(0, 0).fg);
    TEST_ASSERT_EQUAL(VTerm::BoxBase | VTerm::ArmLeft | VTerm::ArmRight, t.cell(1, 0).ch);
}

void test_resize() {
    VTerm t(10, 4, 10);
    t.write("one\r\ntwo\r\nthree\r\nfour");
    t.resize(6, 2);
    TEST_ASSERT_EQUAL(6, t.cols());
    TEST_ASSERT_EQUAL_STRING("three", rowText(t, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("four", rowText(t, 1).c_str());
    TEST_ASSERT_EQUAL(1, t.cursorY());
    TEST_ASSERT_EQUAL(2, t.scrollbackSize());
    t.setViewOffset(2);
    TEST_ASSERT_EQUAL_STRING("one", rowText(t, 0).c_str());
    t.resize(20, 6);
    t.write("\x1b[6;20Hz");
    TEST_ASSERT_EQUAL('z', t.cell(19, 5).ch);
}

void test_dirty_tracking() {
    VTerm t(10, 4);
    t.clearDirty();
    TEST_ASSERT_FALSE(t.anyDirty());
    t.write("\x1b[3;1Hx");
    TEST_ASSERT_TRUE(t.rowDirty(2));
    TEST_ASSERT_TRUE(t.rowDirty(0));  // cursor left this row
    TEST_ASSERT_FALSE(t.rowDirty(3));
}

void test_key_encoding() {
    VTerm t(10, 2);
    auto enc = [&](uint16_t code, uint8_t mods = 0) {
        key::Event e;
        e.code = code;
        e.mods = mods;
        return t.encodeKey(e);
    };
    TEST_ASSERT_EQUAL_STRING("a", enc('a').c_str());
    TEST_ASSERT_EQUAL_STRING("\r", enc(key::Enter).c_str());
    TEST_ASSERT_EQUAL_STRING("\x7f", enc(key::Backspace).c_str());
    TEST_ASSERT_EQUAL_STRING("\x03", enc('c', key::Ctrl).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1b" "b", enc('b', key::Alt).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1b[A", enc(key::Up).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1b[1;5C", enc(key::Right, key::Ctrl).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1b[3~", enc(key::Delete).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1bOP", enc(key::F1).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1b[15~", enc(key::F5).c_str());
    TEST_ASSERT_EQUAL_STRING("\x1b[Z", enc(key::Tab, key::Shift).c_str());
    TEST_ASSERT_EQUAL_STRING("", enc('q', key::Opt).c_str());
    std::string nul = enc(' ', key::Ctrl);
    TEST_ASSERT_EQUAL(1, (int)nul.size());
    TEST_ASSERT_EQUAL(0, nul[0]);
    t.write("\x1b[?1h");
    TEST_ASSERT_EQUAL_STRING("\x1bOA", enc(key::Up).c_str());
}

void test_garbage_does_not_crash() {
    VTerm t(7, 3, 4);
    uint32_t s = 12345;
    std::string buf;
    for (int i = 0; i < 200000; i++) {
        s = s * 1664525u + 1013904223u;
        uint8_t b = (uint8_t)(s >> 24);
        // bias towards escape-sequence bytes
        if ((s >> 8) % 5 == 0) b = "\x1b[];?0123456789ABCDHJKLMPSTXhlmr@\r\n"[(s >> 12) % 35];
        buf.push_back((char)b);
        if (buf.size() == 37) {
            t.write(buf);
            buf.clear();
            if ((s >> 4) % 97 == 0) t.resize(2 + (s >> 16) % 30, 2 + (s >> 20) % 12);
            if ((s >> 4) % 31 == 0) t.setViewOffset((s >> 16) % 6);
            for (int y = 0; y < t.rows(); y++)
                for (int x = 0; x < t.cols(); x++) (void)t.cell(x, y);
            TEST_ASSERT_TRUE(t.cursorX() >= 0 && t.cursorX() < t.cols());
            TEST_ASSERT_TRUE(t.cursorY() >= 0 && t.cursorY() < t.rows());
        }
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_text_and_wrap);
    RUN_TEST(test_pending_wrap_does_not_scroll_early);
    RUN_TEST(test_scroll_and_scrollback);
    RUN_TEST(test_cursor_movement_and_erase);
    RUN_TEST(test_insert_delete_chars_and_lines);
    RUN_TEST(test_scroll_region);
    RUN_TEST(test_sgr_colors);
    RUN_TEST(test_alt_screen);
    RUN_TEST(test_replies);
    RUN_TEST(test_utf8_and_line_drawing);
    RUN_TEST(test_osc_title_and_ignored_strings);
    RUN_TEST(test_split_sequences);
    RUN_TEST(test_resize);
    RUN_TEST(test_dirty_tracking);
    RUN_TEST(test_key_encoding);
    RUN_TEST(test_garbage_does_not_crash);
    return UNITY_END();
}
