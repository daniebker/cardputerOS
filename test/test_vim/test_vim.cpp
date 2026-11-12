#include <unity.h>

#include <string>

#include "vim.h"
#include "vim_layout.h"

// Feed keys: \x1b = Esc, \r = Enter, \b = Backspace, \t = Tab, other control
// characters (e.g. \x12) = Ctrl + letter.
static Vim::Request feed(Vim& v, const std::string& keys) {
    Vim::Request last = Vim::Request::None;
    for (unsigned char c : keys) {
        key::Event e;
        if (c == 0x1b) e.code = key::Esc;
        else if (c == '\r') e.code = key::Enter;
        else if (c == '\b') e.code = key::Backspace;
        else if (c == '\t') e.code = key::Tab;
        else if (c < 0x20) {
            e.code = 'a' + c - 1;
            e.mods = key::Ctrl;
        } else e.code = c;
        Vim::Request r = v.onKey(e);
        if (r != Vim::Request::None) last = r;
    }
    return last;
}

static std::string run(const std::string& text, const std::string& keys, Vim* out = nullptr) {
    Vim v;
    v.setText(text);
    feed(v, keys);
    if (out) *out = v;
    return v.text();
}

#define EXPECT(text, keys, expected) TEST_ASSERT_EQUAL_STRING(expected, run(text, keys).c_str())

void setUp() {}
void tearDown() {}

void test_insert_basics() {
    EXPECT("", "ihello world\x1b", "hello world");
    EXPECT("abc", "Ax\x1b", "abcx");
    EXPECT("abc", "ax\x1b", "axbc");
    EXPECT("  abc", "Ix\x1b", "  xabc");
    EXPECT("one", "otwo\x1b", "one\ntwo");
    EXPECT("one", "Ozero\x1b", "zero\none");
    EXPECT("ab", "a\rc\x1b", "a\ncb");
    EXPECT("ab\ncd", "ji\b\x1b", "abcd");
    EXPECT("  indented", "A\rnext\x1b", "  indented\n  next");
    EXPECT("foo bar", "A\x17\x17x\x1b", "x");
}

void test_cursor_after_escape() {
    Vim v;
    run("abc", "A\x1b", &v);
    TEST_ASSERT_EQUAL(2, v.col());
    run("abc", "$", &v);
    TEST_ASSERT_EQUAL(2, v.col());
    run("abc\n\nxyz", "$jj", &v);
    TEST_ASSERT_EQUAL(2, v.row());
    TEST_ASSERT_EQUAL(2, v.col());  // $ sticks to the end of line
    run("abcdef\nab\nabcdef", "4ljj", &v);
    TEST_ASSERT_EQUAL(4, v.col());  // wanted column is remembered
}

void test_word_motions() {
    Vim v;
    run("foo bar-baz qux", "w", &v);
    TEST_ASSERT_EQUAL(4, v.col());
    run("foo bar-baz qux", "ww", &v);
    TEST_ASSERT_EQUAL(7, v.col());
    run("foo bar-baz qux", "W W", &v);  // space is a motion too
    TEST_ASSERT_EQUAL(12, v.col());
    run("foo bar-baz qux", "$b", &v);
    TEST_ASSERT_EQUAL(12, v.col());
    run("foo bar-baz qux", "$bb", &v);
    TEST_ASSERT_EQUAL(8, v.col());
    run("foo bar-baz qux", "e", &v);
    TEST_ASSERT_EQUAL(2, v.col());
    run("foo bar-baz qux", "ee", &v);
    TEST_ASSERT_EQUAL(6, v.col());
    run("foo\n\nbar", "ww", &v);
    TEST_ASSERT_EQUAL(2, v.row());
    run("foo\n  bar", "jb", &v);
    TEST_ASSERT_EQUAL(0, v.row());
    TEST_ASSERT_EQUAL(0, v.col());
}

void test_delete_and_change() {
    EXPECT("foo bar baz", "dw", "bar baz");
    EXPECT("foo bar baz", "2dw", "baz");
    EXPECT("foo bar baz", "d2w", "baz");
    EXPECT("foo bar baz", "wdw", "foo baz");
    EXPECT("foo bar\nbaz", "wdw", "foo \nbaz");  // dw does not join lines
    EXPECT("foo bar baz", "de", " bar baz");
    EXPECT("foo bar baz", "wD", "foo ");
    EXPECT("foo bar baz", "d$", "");
    EXPECT("foo bar baz", "$d0", "z");
    EXPECT("foo bar baz", "wcwXY\x1b", "foo XY baz");
    EXPECT("foo bar baz", "wcw\x1b", "foo  baz");
    EXPECT("a b c", "cwx\x1b", "x b c");
    EXPECT("foo bar baz", "wCend\x1b", "foo end");
    EXPECT("foo bar baz", "ccnew\x1b", "new");
    EXPECT("foo bar baz", "Snew\x1b", "new");
    EXPECT("foo", "xx", "o");
    EXPECT("foo", "$X", "fo");
    EXPECT("foo", "3x", "");
    EXPECT("foo", "sb\x1b", "boo");
    EXPECT("one\ntwo\nthree", "dd", "two\nthree");
    EXPECT("one\ntwo\nthree", "jdd", "one\nthree");
    EXPECT("one\ntwo\nthree", "2dd", "three");
    EXPECT("one\ntwo\nthree", "dj", "three");
    EXPECT("one\ntwo\nthree", "Gdk", "one");
    EXPECT("one\ntwo\nthree", "dG", "");
    EXPECT("one\ntwo\nthree", "jdgg", "three");
    EXPECT("one", "dd", "");
    EXPECT("foo(bar, baz)", "f(dt)", "foo)");
    EXPECT("foo(bar, baz)", "df,", " baz)");
    EXPECT("foo(bar, baz)", "$dFb", "foo(bar, )");
    EXPECT("a\n\nb\nc\n\nd", "jjd}", "a\n\n\nd");
    EXPECT("abc", "rx", "xbc");
    EXPECT("abc", "2rx", "xxc");
    EXPECT("abc", "l~~", "aBC");
}

void test_text_objects() {
    EXPECT("foo bar baz", "wldiw", "foo  baz");
    EXPECT("foo bar baz", "wldaw", "foo baz");
    EXPECT("foo bar baz", "wlciwX\x1b", "foo X baz");
    EXPECT("foo bar", "$daw", "foo");
    EXPECT("say \"hello world\" now", "fwci\"X\x1b", "say \"X\" now");
    EXPECT("say \"hello world\" now", "fwda\"", "say  now");
    EXPECT("say 'a' and 'b'", "$di'", "say 'a' and ''");
}

void test_yank_put() {
    EXPECT("one\ntwo", "yyp", "one\none\ntwo");
    EXPECT("one\ntwo", "yyjp", "one\ntwo\none");
    EXPECT("one\ntwo", "jyyP", "one\ntwo\ntwo");
    EXPECT("one\ntwo", "yy3p", "one\none\none\none\ntwo");
    EXPECT("one\ntwo\nthree", "yjGp", "one\ntwo\nthree\none\ntwo");
    EXPECT("foo bar", "ywwP", "foo foo bar");
    EXPECT("foo bar", "yw$p", "foo barfoo ");
    EXPECT("ab", "xp", "ba");
    EXPECT("one\ntwo", "ddp", "two\none");
    EXPECT("", "p", "");
    Vim v;
    run("one\ntwo", "yyjp", &v);
    TEST_ASSERT_EQUAL(2, v.row());
}

void test_visual() {
    EXPECT("foo bar baz", "wved", "foo  baz");
    EXPECT("foo bar baz", "vwwhy$p", "foo bar bazfoo bar ");
    EXPECT("one\ntwo\nthree", "Vjd", "three");
    EXPECT("one\ntwo\nthree", "jVyP", "one\ntwo\ntwo\nthree");
    EXPECT("one\ntwo\nthree", "lvjd", "oo\nthree");
    EXPECT("one\ntwo\nthree", "vjocX\x1b", "Xwo\nthree");
    EXPECT("one\ntwo", "Vj>", "  one\n  two");
    EXPECT("foo bar", "wviwcX\x1b", "foo X");
    EXPECT("one\ntwo\nthree", "VjJ", "one two\nthree");
    EXPECT("foo bar", "yiwwviwp", "foo foo");
    Vim v;
    run("abc", "v\x1b", &v);
    TEST_ASSERT_TRUE(v.mode() == Vim::Mode::Normal);
    Vim::Pos a, b;
    bool lw;
    run("abc\ndef", "lvj", &v);
    TEST_ASSERT_TRUE(v.selection(a, b, lw));
    TEST_ASSERT_EQUAL(0, a.r);
    TEST_ASSERT_EQUAL(1, a.c);
    TEST_ASSERT_EQUAL(1, b.r);
    TEST_ASSERT_FALSE(lw);
}

void test_undo_redo() {
    EXPECT("one\ntwo", "ddu", "one\ntwo");
    EXPECT("one\ntwo", "ddu\x12", "two");
    EXPECT("abc", "xxxuu", "bc");
    EXPECT("abc", "ihello \x1bu", "abc");
    EXPECT("abc", "ix\x1biy\x1buu\x12", "xabc");
    EXPECT("a\nb\nc\nd", "jddjp" "uu", "a\nb\nc\nd");
    EXPECT("a\nb\nc\nd", "Jjddu", "a b\nc\nd");
    EXPECT("abc", "u", "abc");
    EXPECT("abc", "x\x12", "bc");
    EXPECT("a b c", "dwdwuix\x1b\x12", "xb c");  // new change clears redo
    Vim v;
    run("one\ntwo\nthree", "jdd" "u", &v);
    TEST_ASSERT_EQUAL(1, v.row());
}

void test_dot_repeat() {
    EXPECT("a b c d", "dw..", "d");
    EXPECT("one\ntwo\nthree", "dd.", "three");
    EXPECT("foo foo foo", "cwbar\x1bw.w.", "bar bar bar");
    EXPECT("abc", "ix\x1b..", "xxxabc");
    EXPECT("a\nb\nc", "A!\x1bj.j.", "a!\nb!\nc!");
    EXPECT("abcdef", "x.u.", "cdef");
    EXPECT("abc", "lll.", "abc");
}

void test_join_and_shift() {
    EXPECT("one\n  two\nthree", "J", "one two\nthree");
    EXPECT("one\ntwo\nthree", "3J", "one two three");
    EXPECT("one\n\ntwo", "J", "one\ntwo");
    EXPECT("one", ">>", "  one");
    EXPECT("  one\n    two", "2<<", "one\n  two");
    EXPECT("one\ntwo\nthree", ">j", "  one\n  two\nthree");
}

void test_search() {
    Vim v;
    run("alpha beta\ngamma beta\nAlpha", "/beta\r", &v);
    TEST_ASSERT_EQUAL(0, v.row());
    TEST_ASSERT_EQUAL(6, v.col());
    run("alpha beta\ngamma beta\nAlpha", "/beta\rn", &v);
    TEST_ASSERT_EQUAL(1, v.row());
    run("alpha beta\ngamma beta\nAlpha", "/beta\rnn", &v);
    TEST_ASSERT_EQUAL(0, v.row());  // wrapped
    TEST_ASSERT_TRUE(v.message().find("BOTTOM") != std::string::npos);
    run("alpha beta\ngamma beta\nAlpha", "/beta\rnN", &v);
    TEST_ASSERT_EQUAL(0, v.row());
    run("alpha beta\ngamma beta\nAlpha", "/alpha\r", &v);
    TEST_ASSERT_EQUAL(2, v.row());  // smartcase: lower-case pattern ignores case
    run("alpha beta\ngamma beta\nAlpha", "G/Alpha\r", &v);
    TEST_ASSERT_EQUAL(2, v.row());  // only match is under the cursor: found again after wrapping
    run("alpha beta\ngamma beta\nAlpha", "?beta\r", &v);
    TEST_ASSERT_EQUAL(1, v.row());
    run("alpha beta\ngamma beta\nAlpha", "/nothere\r", &v);
    TEST_ASSERT_TRUE(v.message().find("not found") != std::string::npos);
    run("beta x beta", "*", &v);
    TEST_ASSERT_EQUAL(7, v.col());
}

void test_commands() {
    Vim v;
    v.setText("a\nb\nc");
    TEST_ASSERT_TRUE(feed(v, ":w\r") == Vim::Request::Write);
    TEST_ASSERT_TRUE(feed(v, ":q\r") == Vim::Request::Quit);
    TEST_ASSERT_TRUE(feed(v, "x:q\r") == Vim::Request::None);  // modified
    TEST_ASSERT_FALSE(v.message().empty());
    TEST_ASSERT_TRUE(feed(v, ":q!\r") == Vim::Request::ForceQuit);
    TEST_ASSERT_TRUE(feed(v, ":wq\r") == Vim::Request::WriteQuit);
    TEST_ASSERT_TRUE(feed(v, "ZZ") == Vim::Request::WriteQuit);
    TEST_ASSERT_TRUE(feed(v, ":3\r") == Vim::Request::None);
    TEST_ASSERT_EQUAL(2, v.row());
    feed(v, ":bogus\r");
    TEST_ASSERT_TRUE(v.message().find("Not an editor command") != std::string::npos);
    feed(v, ":w");
    TEST_ASSERT_EQUAL_STRING(":w", v.prompt().c_str());
    feed(v, "\x1b");
    TEST_ASSERT_TRUE(v.mode() == Vim::Mode::Normal);
    TEST_ASSERT_TRUE(v.modified());
    v.setModified(false);
    feed(v, "jk");
    TEST_ASSERT_FALSE(v.modified());
}

void test_counts_and_goto() {
    Vim v;
    run("1\n2\n3\n4\n5", "3G", &v);
    TEST_ASSERT_EQUAL(2, v.row());
    run("1\n2\n3\n4\n5", "Ggg", &v);
    TEST_ASSERT_EQUAL(0, v.row());
    run("1\n2\n3\n4\n5", "2j10k", &v);
    TEST_ASSERT_EQUAL(0, v.row());
    run("abcdefghijklmnop", "10l", &v);
    TEST_ASSERT_EQUAL(10, v.col());
    EXPECT("1\n2\n3\n4\n5", "2Gd3G", "1\n4\n5");
}

void test_utf8_roundtrip() {
    const std::string s = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 ok";
    EXPECT(s, "", s.c_str());
    EXPECT(s, "x", ("af\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 ok"));
    EXPECT("caf\xC3\xA9", "$x", "caf");  // deletes the whole character, not one byte
    EXPECT("a\r\nb\n", "", "a\nb");
}

void test_random_keys_do_not_crash() {
    Vim v;
    v.setText("The quick brown fox\n  jumps over\n\nthe \"lazy\" dog.\nlast line");
    const std::string alphabet = "hjklwbe0$^GgxXdcyp.uPJoOiaIAvV{}ft;,nN*/?:rsSDC~<>23 \"'q\x1b\x1b\x1b\r\b\t\x12\x04\x15\x17";
    uint32_t s = 99;
    for (int i = 0; i < 300000; i++) {
        s = s * 1664525u + 1013904223u;
        feed(v, std::string(1, alphabet[(s >> 16) % alphabet.size()]));
        TEST_ASSERT_TRUE(v.row() >= 0 && v.row() < (int)v.lines().size());
        TEST_ASSERT_TRUE(v.col() >= 0 && v.col() <= (int)v.lines()[v.row()].size());
        if (v.lines().size() > 400) v.setText("reset\ntext here");
    }
}


// Renders the layout as text, '|' marks the cursor.
static std::vector<std::string> screen(const Vim& v, VimLayout& lay, int cols, int rows) {
    std::vector<std::string> out;
    for (const VimRow& r : lay.update(v, cols, rows)) {
        if (r.line < 0) {
            out.push_back("~");
            continue;
        }
        std::string s = Vim::toUtf8(v.lines()[r.line].substr(r.start, r.len));
        if (r.cursor >= 0) s.insert(std::min<size_t>(r.cursor, s.size()), "|");
        out.push_back(s);
    }
    return out;
}

static void checkInvariants(const Vim& v, VimLayout& lay, int cols, int rows) {
    const auto& rs = lay.update(v, cols, rows);
    int cursorRows = 0;
    int prevLine = -1, prevEnd = 0;
    for (const VimRow& r : rs) {
        if (r.cursor >= 0) {
            cursorRows++;
            TEST_ASSERT_TRUE(r.cursor >= 0 && r.cursor < cols);
        }
        if (r.line < 0) continue;
        TEST_ASSERT_TRUE(r.len >= 0 && r.len <= cols);
        if (r.line == prevLine) TEST_ASSERT_EQUAL(prevEnd, r.start);  // continuation rows are contiguous
        else if (prevLine >= 0) {
            TEST_ASSERT_EQUAL(prevLine + 1, r.line);  // no line skipped
            TEST_ASSERT_EQUAL(0, r.start);
        }
        prevLine = r.line;
        prevEnd = r.start + r.len;
    }
    TEST_ASSERT_EQUAL(1, cursorRows);  // the cursor is always on screen
}

void test_layout_wrapping() {
    Vim v;
    VimLayout lay;
    v.setText("# Title\nthe quick brown fox jumps\nabcdefghijklmnopqrstuvwxy\nshort");
    auto s = screen(v, lay, 10, 8);
    TEST_ASSERT_EQUAL_STRING("|# Title", s[0].c_str());
    TEST_ASSERT_EQUAL_STRING("the quick ", s[1].c_str());  // word wrap
    TEST_ASSERT_EQUAL_STRING("brown fox ", s[2].c_str());
    TEST_ASSERT_EQUAL_STRING("jumps", s[3].c_str());
    TEST_ASSERT_EQUAL_STRING("abcdefghij", s[4].c_str());  // no space: hard break
    TEST_ASSERT_EQUAL_STRING("klmnopqrst", s[5].c_str());
    TEST_ASSERT_EQUAL_STRING("uvwxy", s[6].c_str());
    TEST_ASSERT_EQUAL_STRING("short", s[7].c_str());
    feed(v, "j$");
    s = screen(v, lay, 10, 8);
    TEST_ASSERT_EQUAL_STRING("jump|s", s[3].c_str());
    feed(v, "0w");  // cursor on "quick"
    s = screen(v, lay, 10, 8);
    TEST_ASSERT_EQUAL_STRING("the |quick ", s[1].c_str());
    feed(v, "ww");  // "fox"
    s = screen(v, lay, 10, 8);
    TEST_ASSERT_EQUAL_STRING("brown |fox ", s[2].c_str());
}

void test_layout_typing_across_wrap() {
    Vim v;
    VimLayout lay;
    v.setText("");
    feed(v, "i");
    for (int i = 0; i < 25; i++) {
        feed(v, std::string(1, (char)('a' + i)));
        checkInvariants(v, lay, 10, 4);
    }
    auto s = screen(v, lay, 10, 4);
    TEST_ASSERT_EQUAL_STRING("abcdefghij", s[0].c_str());  // one long word: hard breaks
    TEST_ASSERT_EQUAL_STRING("klmnopqrst", s[1].c_str());
    TEST_ASSERT_EQUAL_STRING("uvwxy|", s[2].c_str());
    // exactly at the wrap point the cursor moves to a fresh row
    v.setText("");
    feed(v, "i0123456789");
    s = screen(v, lay, 10, 4);
    TEST_ASSERT_EQUAL_STRING("0123456789", s[0].c_str());
    TEST_ASSERT_EQUAL_STRING("|", s[1].c_str());
    // typing words: a word that no longer fits moves to the next row as it is typed
    v.setText("");
    feed(v, "ihello wor");
    s = screen(v, lay, 10, 4);
    TEST_ASSERT_EQUAL_STRING("hello wor|", s[0].c_str());
    feed(v, "ld");
    s = screen(v, lay, 10, 4);
    TEST_ASSERT_EQUAL_STRING("hello ", s[0].c_str());
    TEST_ASSERT_EQUAL_STRING("world|", s[1].c_str());
}

void test_layout_scrolling() {
    Vim v;
    VimLayout lay;
    std::string text;
    for (int i = 0; i < 30; i++) text += "line " + std::to_string(i) + " " + std::string(i % 3 == 0 ? 25 : 3, 'x') + "\n";
    v.setText(text);
    for (int i = 0; i < 40; i++) {
        feed(v, "j");
        checkInvariants(v, lay, 10, 5);
    }
    for (int i = 0; i < 40; i++) {
        feed(v, "k");
        checkInvariants(v, lay, 10, 5);
    }
    feed(v, "G$");
    checkInvariants(v, lay, 10, 5);
    feed(v, "gg");
    checkInvariants(v, lay, 10, 5);
    // a line longer than the whole screen
    v.setText(std::string(100, 'z'));
    feed(v, "$");
    checkInvariants(v, lay, 10, 3);
    feed(v, "0");
    checkInvariants(v, lay, 10, 3);
}

void test_layout_random_editing() {
    Vim v;
    VimLayout lay;
    v.setText("# Notes\nsome text that is long enough to wrap around a few times on a small screen\n\n- item");
    const std::string alphabet = "hjklwbe0$GgxdoOiaAp.u \x1b\x1b\rabcdefghijklmnop";
    uint32_t s = 7;
    for (int i = 0; i < 50000; i++) {
        s = s * 1664525u + 1013904223u;
        feed(v, std::string(1, alphabet[(s >> 16) % alphabet.size()]));
        const int cols = 5 + (s >> 8) % 3;
        checkInvariants(v, lay, cols, 4);
        if (v.lines().size() > 200) v.setText("reset");
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_insert_basics);
    RUN_TEST(test_cursor_after_escape);
    RUN_TEST(test_word_motions);
    RUN_TEST(test_delete_and_change);
    RUN_TEST(test_text_objects);
    RUN_TEST(test_yank_put);
    RUN_TEST(test_visual);
    RUN_TEST(test_undo_redo);
    RUN_TEST(test_dot_repeat);
    RUN_TEST(test_join_and_shift);
    RUN_TEST(test_search);
    RUN_TEST(test_commands);
    RUN_TEST(test_counts_and_goto);
    RUN_TEST(test_utf8_roundtrip);
    RUN_TEST(test_random_keys_do_not_crash);
    RUN_TEST(test_layout_wrapping);
    RUN_TEST(test_layout_typing_across_wrap);
    RUN_TEST(test_layout_scrolling);
    RUN_TEST(test_layout_random_editing);
    return UNITY_END();
}
