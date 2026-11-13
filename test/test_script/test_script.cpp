#include <unity.h>

#include <fstream>
#include <map>
#include <sstream>

#include "lua.h"
#include "lauxlib.h"
#include "script.h"

using namespace script;

void setUp() {}
void tearDown() {}

// Records what a script does with the device.
class FakeHost : public Host {
public:
    uint32_t now = 1000;
    uint32_t autoAdvance = 0;  // ms added on each millis() call (simulates time passing)
    bool drawable = true, interrupt = false;
    std::vector<std::string> logs, ops;
    std::map<std::string, std::string> files;
    std::string save;
    bool hasSave = false;
    int saves = 0;
    // dialog answers
    bool confirmAnswer = true;
    std::string promptAnswer = "typed";
    bool promptOk = true;
    int chooseAnswer = 1;
    int dialogs = 0;

    uint32_t millis() override { return now += autoAdvance; }
    uint32_t random32() override { return 12345; }
    void log(const std::string& line) override { logs.push_back(line); }
    bool interrupted() override { return interrupt; }
    bool canDraw() override { return drawable; }
    int width() override { return 320; }
    int height() override { return 240; }
    void op(const std::string& s) { ops.push_back(s); }
    static std::string n(int v) { return std::to_string(v); }
    void fillRect(int x, int y, int w, int h, uint32_t c) override {
        op("fillRect " + n(x) + " " + n(y) + " " + n(w) + " " + n(h) + " " + n((int)c));
    }
    void drawRect(int, int, int, int, uint32_t) override { op("drawRect"); }
    void fillRoundRect(int, int, int, int, int, uint32_t) override { op("fillRoundRect"); }
    void drawRoundRect(int, int, int, int, int, uint32_t) override { op("drawRoundRect"); }
    void drawLine(int, int, int, int, uint32_t) override { op("line"); }
    void drawPixel(int x, int y, uint32_t) override { op("pixel " + n(x) + " " + n(y)); }
    void drawCircle(int, int, int, uint32_t) override { op("circle"); }
    void fillCircle(int, int, int, uint32_t) override { op("fillCircle"); }
    void drawTriangle(int, int, int, int, int, int, uint32_t) override { op("triangle"); }
    void fillTriangle(int, int, int, int, int, int, uint32_t) override { op("fillTriangle"); }
    void drawText(const std::string& t, int x, int y, Font f, int scale, Align a, uint32_t fg, bool hasBg,
                  uint32_t) override {
        op("text " + t + " " + n(x) + " " + n(y) + " f" + n((int)f) + " x" + n(scale) + " a" + n((int)a) + " " +
           n((int)fg) + (hasBg ? " bg" : ""));
    }
    int textWidth(const std::string& t, Font) override { return 6 * (int)t.size(); }
    int fontHeight(Font f) override { return 8 + 2 * (int)f; }
    int header(const std::string& t) override {
        op("header " + t);
        return 20;
    }
    int footer(const std::string& t) override {
        op("footer " + t);
        return 220;
    }
    void alert(const std::string& t, const std::string& m) override {
        dialogs++;
        op("alert " + t + ": " + m);
    }
    bool confirm(const std::string&, const std::string&, bool) override {
        dialogs++;
        return confirmAnswer;
    }
    bool prompt(const std::string&, std::string& v, bool) override {
        dialogs++;
        if (promptOk) v = promptAnswer;
        return promptOk;
    }
    int choose(const std::string&, const std::vector<std::string>& items, int initial) override {
        dialogs++;
        op("choose " + n((int)items.size()) + " " + n(initial));
        return chooseAnswer;
    }
    void busy(const std::string& m) override { op("busy " + m); }
    bool readFile(const std::string& p, std::string& out, size_t maxBytes, std::string& err) override {
        auto it = files.find(p);
        if (it == files.end()) {
            err = "not found";
            return false;
        }
        if (it->second.size() > maxBytes) {
            err = "too large";
            return false;
        }
        out = it->second;
        return true;
    }
    bool writeFile(const std::string& p, const std::string& d, bool append, std::string&) override {
        if (append) files[p] += d;
        else files[p] = d;
        return true;
    }
    bool listDir(const std::string& p, std::vector<FileEntry>& out, std::string&) override {
        for (auto& f : files) {
            if (p.empty() || f.first.compare(0, p.size() + 1, p + "/") == 0) {
                FileEntry e;
                e.name = p.empty() ? f.first : f.first.substr(p.size() + 1);
                e.size = (uint32_t)f.second.size();
                out.push_back(e);
            }
        }
        return true;
    }
    bool removeFile(const std::string& p, std::string& err) override {
        if (!files.erase(p)) {
            err = "not found";
            return false;
        }
        return true;
    }
    bool exists(const std::string& p) override { return files.count(p) > 0; }
    bool loadSave(std::string& out) override {
        out = save;
        return hasSave;
    }
    bool storeSave(const std::string& d) override {
        save = d;
        hasSave = true;
        saves++;
        return true;
    }
    int64_t unixTime() override { return 1763000000; }
    int battery() override { return 77; }
    std::string wifi() override { return "home"; }
    std::string osVersion() override { return "9.9.9"; }

    bool logged(const std::string& s) const {
        for (auto& l : logs)
            if (l.find(s) != std::string::npos) return true;
        return false;
    }
    bool did(const std::string& s) const {
        for (auto& o : ops)
            if (o == s) return true;
        return false;
    }
};

static Manifest manifest() {
    Manifest m;
    m.id = "test";
    m.name = "Test";
    return m;
}

static key::Event keyEv(uint16_t code, uint8_t mods = 0) {
    key::Event e;
    e.code = code;
    e.mods = mods;
    return e;
}

// ------------------------------------------------------------ manifest / paths

void test_manifest_parsing() {
    Manifest m;
    std::string err;
    TEST_ASSERT_TRUE(parseManifest("snake",
                                   "# a comment\n"
                                   "name = Snake Game\n"
                                   "description = \"Eat and grow\"\n"
                                   "color = #50fa7b\r\n"
                                   "icon = Tiles\n"
                                   "folder = Games\n"
                                   "unknown = ignored\n",
                                   m, err));
    TEST_ASSERT_EQUAL_STRING("snake", m.id.c_str());
    TEST_ASSERT_EQUAL_STRING("Snake Game", m.name.c_str());
    TEST_ASSERT_EQUAL_STRING("Eat and grow", m.description.c_str());
    TEST_ASSERT_EQUAL_HEX32(0x50FA7B, m.color);
    TEST_ASSERT_EQUAL_STRING("tiles", m.icon.c_str());
    TEST_ASSERT_EQUAL_STRING("Games", m.folder.c_str());

    TEST_ASSERT_TRUE(parseManifest("x", "", m, err));  // defaults
    TEST_ASSERT_EQUAL_STRING("x", m.name.c_str());
    TEST_ASSERT_TRUE(parseManifest("x", "color = pink", m, err));
    TEST_ASSERT_EQUAL_HEX32(0xFF79C6, m.color);
    TEST_ASSERT_FALSE(parseManifest("x", "color = blurple", m, err));
    TEST_ASSERT_FALSE(parseManifest("x", "just text", m, err));
    TEST_ASSERT_TRUE(err.find("line 1") != std::string::npos);
}

void test_ids_and_paths() {
    TEST_ASSERT_TRUE(validId("snake"));
    TEST_ASSERT_TRUE(validId("my-app_2.0"));
    TEST_ASSERT_FALSE(validId(""));
    TEST_ASSERT_FALSE(validId(".hidden"));
    TEST_ASSERT_FALSE(validId("a b"));
    TEST_ASSERT_FALSE(validId("a/b"));
    TEST_ASSERT_FALSE(validId(std::string(32, 'a')));

    std::string out;
    TEST_ASSERT_TRUE(cleanPath("data/level1.txt", out));
    TEST_ASSERT_EQUAL_STRING("data/level1.txt", out.c_str());
    TEST_ASSERT_TRUE(cleanPath("/./data//x.txt", out));
    TEST_ASSERT_EQUAL_STRING("data/x.txt", out.c_str());
    TEST_ASSERT_TRUE(cleanPath("", out));
    TEST_ASSERT_EQUAL_STRING("", out.c_str());
    TEST_ASSERT_FALSE(cleanPath("../other/main.lua", out));
    TEST_ASSERT_FALSE(cleanPath("data/../../x", out));
    TEST_ASSERT_FALSE(cleanPath("a\\b", out));
    TEST_ASSERT_FALSE(cleanPath("c:x", out));
    TEST_ASSERT_FALSE(cleanPath(std::string(200, 'a'), out));
}

// ------------------------------------------------------------ lifecycle

void test_lifecycle_and_drawing() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        local started = false
        function app.start() started = true end
        function app.draw()
            gfx.clear()
            gfx.fill_rect(10, 20, 30, 40, colors.red)
            gfx.fill_rect(1.7, 2.2, 3, 4, "#000010")
            gfx.text("hi", 160, 5, colors.text, {font = "large", align = "center", bg = colors.bg})
            print(gfx.text("big", 0, 0, nil, {size = 3}), gfx.text_width("abc", "mono", 2), gfx.font_height("small", 99))
            gfx.pixel(gfx.width() - 1, gfx.height() - 1)
            gfx.text(42, 1, 2)  -- optional arguments left out
            print(gfx.text_width("ab"))
        end
        return app
    )"));
    TEST_ASSERT_FALSE(r.failed());
    r.draw();
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_TRUE(h.did("fillRect 0 0 320 240 " + std::to_string(0x282A36)));
    TEST_ASSERT_TRUE(h.did("fillRect 10 20 30 40 " + std::to_string(0xFF5555)));
    TEST_ASSERT_TRUE(h.did("fillRect 1 2 3 4 16"));  // floats are floored
    TEST_ASSERT_TRUE(h.did("text hi 160 5 f2 x1 a1 " + std::to_string(0xF8F8F2) + " bg"));
    TEST_ASSERT_TRUE(h.did("text big 0 0 f1 x3 a0 " + std::to_string(0xF8F8F2)));
    TEST_ASSERT_EQUAL_STRING("54\t36\t64", h.logs[0].c_str());  // 6 px per char x3; x2; 8 px x8 (size capped)
    TEST_ASSERT_TRUE(h.did("pixel 319 239"));
    TEST_ASSERT_TRUE(h.did("text 42 1 2 f1 x1 a0 " + std::to_string(0xF8F8F2)));
    TEST_ASSERT_EQUAL_STRING("12", h.logs[1].c_str());
}

void test_no_drawing_in_background() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start("return { background = function(dt) gfx.clear() ui.alert('x') end }"));
    h.drawable = false;
    h.now += 100;
    r.tick(false);
    TEST_ASSERT_FALSE(r.failed());
    TEST_ASSERT_EQUAL(0, (int)h.ops.size());
    TEST_ASSERT_EQUAL(0, h.dialogs);
}

void test_keys() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.key(k)
            print(k.key, k.char, k.ctrl, k.shift, k.opt)
        end
        return app
    )"));
    r.key(keyEv('a'));
    r.key(keyEv('A', key::Shift));
    r.key(keyEv(key::Up));
    r.key(keyEv('c', key::Ctrl));
    r.key(keyEv(' '));
    r.key(keyEv(key::F1 + 2));
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_EQUAL(6, (int)h.logs.size());
    TEST_ASSERT_EQUAL_STRING("a\ta\tfalse\tfalse\tfalse", h.logs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("A\tA\tfalse\ttrue\tfalse", h.logs[1].c_str());
    TEST_ASSERT_EQUAL_STRING("up\tnil\tfalse\tfalse\tfalse", h.logs[2].c_str());
    TEST_ASSERT_EQUAL_STRING("c\tnil\ttrue\tfalse\tfalse", h.logs[3].c_str());
    TEST_ASSERT_EQUAL_STRING("space\t \tfalse\tfalse\tfalse", h.logs[4].c_str());
    TEST_ASSERT_EQUAL_STRING("f3\tnil\tfalse\tfalse\tfalse", h.logs[5].c_str());
}

void test_update_interval() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.start() sys.interval(100) end
        function app.update(dt) print("update", dt) end
        return app
    )"));
    r.tick(true);  // first tick only starts the clock
    h.now += 50;
    r.tick(true);
    TEST_ASSERT_EQUAL(0, (int)h.logs.size());
    h.now += 60;
    r.tick(true);
    TEST_ASSERT_EQUAL(1, (int)h.logs.size());
    TEST_ASSERT_EQUAL_STRING("update\t110", h.logs[0].c_str());
}

void test_sys_and_exit() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.key(k)
            print(sys.version, sys.app, sys.name, sys.battery(), sys.wifi(), sys.time())
            sys.status("score 5")
            sys.notice("done")
            sys.keep_awake(true)
            if k.key == "q" then sys.exit() end
        end
        return app
    )"));
    r.key(keyEv('x'));
    TEST_ASSERT_EQUAL_STRING("9.9.9\ttest\tTest\t77\thome\t1763000000", h.logs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("score 5", r.status().c_str());
    TEST_ASSERT_EQUAL_STRING("done", r.notice().c_str());
    TEST_ASSERT_TRUE(r.keepAwake());
    TEST_ASSERT_FALSE(r.exitRequested());
    r.key(keyEv('q'));
    TEST_ASSERT_TRUE(r.exitRequested());
}

// ------------------------------------------------------------ errors and limits

void test_runtime_error_stops_app() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        local function helper() local t = nil; return t.field end
        function app.key(k) helper() end
        return app
    )"));
    r.key(keyEv('a'));
    TEST_ASSERT_TRUE(r.failed());
    TEST_ASSERT_TRUE(r.error().find("main.lua:3:") != std::string::npos);
    TEST_ASSERT_TRUE(r.error().find("traceback") != std::string::npos);
    r.key(keyEv('a'));  // further calls are ignored
    r.draw();
    r.tick(true);
}

void test_syntax_error_and_bad_return() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_FALSE(r.start("local x = = 1"));
    TEST_ASSERT_TRUE(r.error().find("main.lua:1:") != std::string::npos);

    FakeHost h2;
    Runtime r2(h2, manifest());
    TEST_ASSERT_FALSE(r2.start("local app = {}"));
    TEST_ASSERT_TRUE(r2.error().find("return app") != std::string::npos);
}

void test_endless_loop_is_stopped() {
    FakeHost h;
    h.autoAdvance = 1;  // the clock moves on while the script runs
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.key(k)
            -- pcall must not be able to catch the time limit forever
            while true do pcall(function() while true do end end) end
        end
        return app
    )"));
    r.key(keyEv('a'));
    TEST_ASSERT_TRUE(r.failed());
    TEST_ASSERT_TRUE(r.error().find("ms without returning") != std::string::npos);
}

void test_coroutine_loop_is_stopped() {
    FakeHost h;
    h.autoAdvance = 1;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        return { key = function()
            while true do
                coroutine.resume(coroutine.create(function() while true do end end))
            end
        end }
    )"));
    r.key(keyEv('a'));
    TEST_ASSERT_TRUE(r.failed());
}

void test_memory_limit() {
    FakeHost h;
    Limits lim;
    lim.memory = 64 * 1024;
    Runtime r(h, manifest(), lim);
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        local keep = {}
        function app.key(k)
            if k.key == "s" then
                print(pcall(string.rep, "x", 1000000))
                return
            end
            for i = 1, 1e9 do keep[i] = {i, i, i} end
        end
        return app
    )"));
    // a failed allocation inside pcall is an ordinary error the script can handle
    r.key(keyEv('s'));
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_TRUE(h.logs.size() == 1 && h.logs[0].find("false") == 0);
    TEST_ASSERT_TRUE(r.memoryUsed() <= lim.memory);
    r.key(keyEv('a'));
    TEST_ASSERT_TRUE(r.failed());
    TEST_ASSERT_TRUE(r.error().find("memory") != std::string::npos);
}

void test_sandbox() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.start()
            print(io, debug, package, dofile, loadfile, os.execute, os.exit, os.remove, os.getenv)
            print(type(os.time()), type(os.date("%Y")), type(string.format), type(utf8.char))
            local bytecode = string.dump(function() return 1 end)
            print(load(bytecode))
            print(load("return 1 + 1")())
        end
        return app
    )"));
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_EQUAL_STRING("nil\tnil\tnil\tnil\tnil\tnil\tnil\tnil\tnil", h.logs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("number\tstring\tfunction\tfunction", h.logs[1].c_str());
    TEST_ASSERT_TRUE(h.logs[2].find("nil\t") == 0);  // binary chunks are refused
    TEST_ASSERT_EQUAL_STRING("2", h.logs[3].c_str());
}

void test_deep_nesting_is_an_error_not_a_crash() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"LUA(
        local app = {}
        local function deep(n) if n == 0 then return 0 end return select(2, pcall(deep, n - 1)) end
        local function plain(n) if n == 0 then return 0 end return 1 + plain(n - 1) end
        function app.key(k)
            print(plain(500))   -- Lua-to-Lua recursion uses Lua memory, not the C stack
            print(pcall(deep, 30))
            print(pcall(deep, 200))
            print(load("return " .. string.rep("(", 300) .. "1" .. string.rep(")", 300)))
        end
        return app
    )LUA"));
    r.key(keyEv('a'));
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_EQUAL_STRING("500", h.logs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("true\t0", h.logs[1].c_str());
    TEST_ASSERT_TRUE(h.logs[2].find("stack overflow") != std::string::npos);
    TEST_ASSERT_TRUE(h.logs[3].find("nil\t") == 0);  // too many syntax levels
}

// ------------------------------------------------------------ dialogs

void test_dialogs_and_redraw() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.draw() print("draw") end
        function app.key(k)
            if k.key == "c" then print(ui.confirm("Sure?", "Really")) end
            if k.key == "p" then print(ui.prompt("Name", "old")) end
            if k.key == "l" then print(ui.choose("Pick", {"a", "b", "c"}, 3)) end
            if k.key == "x" then print("no dialog") end
        end
        return app
    )"));
    r.key(keyEv('c'));
    TEST_ASSERT_EQUAL_STRING("true", h.logs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("draw", h.logs[1].c_str());  // repainted after the dialog
    r.key(keyEv('p'));
    TEST_ASSERT_EQUAL_STRING("typed", h.logs[2].c_str());
    h.promptOk = false;
    r.key(keyEv('p'));
    TEST_ASSERT_EQUAL_STRING("nil", h.logs[4].c_str());
    r.key(keyEv('l'));
    TEST_ASSERT_TRUE(h.did("choose 3 2"));
    TEST_ASSERT_EQUAL_STRING("2", h.logs[6].c_str());  // 1-based
    h.logs.clear();
    r.key(keyEv('x'));
    TEST_ASSERT_EQUAL(1, (int)h.logs.size());  // no repaint without a dialog
}

void test_dialog_time_not_counted() {
    FakeHost h;
    Runtime r(h, manifest());
    // the user takes 10 s to answer; the script's limit is 2 s
    class SlowHost : public FakeHost {
    public:
        bool confirm(const std::string&, const std::string&, bool) override {
            now += 10000;
            return true;
        }
    } slow;
    Runtime r2(slow, manifest());
    TEST_ASSERT_TRUE(r2.start(R"(
        return { key = function()
            ui.confirm("a", "b")
            for i = 1, 20000 do end   -- runs the hook a few times after the dialog
        end }
    )"));
    r2.key(keyEv('a'));
    TEST_ASSERT_FALSE_MESSAGE(r2.failed(), r2.error().c_str());
}

// ------------------------------------------------------------ files and modules

void test_fs_and_require() {
    FakeHost h;
    h.files["lib/util.lua"] = "local M = {} function M.twice(x) return 2 * x end return M";
    h.files["data/a.txt"] = "hello";
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local util = require("lib.util")
        local again = require("lib.util")
        local app = {}
        function app.start()
            print(util.twice(21), util == again)
            print(fs.read("data/a.txt"))
            print(fs.read("missing.txt"))
            print(fs.write("out/b.txt", "x"), fs.append("out/b.txt", "y"))
            print(fs.read("/out/b.txt"), fs.exists("out/b.txt"))
            print(#fs.list("out"), fs.list("out")[1].name)
            print(fs.remove("out/b.txt"), fs.exists("out/b.txt"))
            print(pcall(fs.read, "../secret"))
            print(pcall(require, "nothere"))
        end
        return app
    )"));
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_EQUAL_STRING("42\ttrue", h.logs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("hello", h.logs[1].c_str());
    TEST_ASSERT_EQUAL_STRING("nil\tnot found", h.logs[2].c_str());
    TEST_ASSERT_EQUAL_STRING("true\ttrue", h.logs[3].c_str());
    TEST_ASSERT_EQUAL_STRING("xy\ttrue", h.logs[4].c_str());
    TEST_ASSERT_EQUAL_STRING("1\tb.txt", h.logs[5].c_str());
    TEST_ASSERT_EQUAL_STRING("true\tfalse", h.logs[6].c_str());
    TEST_ASSERT_TRUE(h.logs[7].find("false\t") == 0 && h.logs[7].find("inside the app folder") != std::string::npos);
    TEST_ASSERT_TRUE(h.logs[8].find("false\t") == 0 && h.logs[8].find("nothere.lua") != std::string::npos);
}

void test_error_in_module_names_its_file() {
    FakeHost h;
    h.files["bad.lua"] = "local M = {}\nfunction M.go() error('boom') end\nreturn M";
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start("local bad = require('bad') return { key = function() bad.go() end }"));
    r.key(keyEv('a'));
    TEST_ASSERT_TRUE(r.failed());
    TEST_ASSERT_TRUE(r.error().find("bad.lua:2: boom") != std::string::npos);
}

// ------------------------------------------------------------ save data

void test_store_round_trip() {
    FakeHost h;
    {
        Runtime r(h, manifest());
        TEST_ASSERT_TRUE(r.start(R"(
            local app = {}
            function app.key(k)
                store.set("best", 1234)
                store.set("name", "Ann \"the\" \n\0 player")
                store.set("ratio", 0.5)
                store.set("whole", 3.0)
                store.set("on", true)
                store.set("board", {1, 2, {x = 3}, name = "b", [10] = false})
                store.set("gone", "x")
                store.set("gone", nil)
                print(pcall(store.set, "f", print))
                local t = {} t.self = t
                print(pcall(store.set, "cycle", t))
            end
            return app
        )"));
        r.key(keyEv('a'));
        TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
        TEST_ASSERT_TRUE(h.logs[0].find("false") == 0);
        TEST_ASSERT_TRUE(h.logs[1].find("contains itself") != std::string::npos);
        TEST_ASSERT_EQUAL(0, h.saves);  // not written on every set
        r.pause();
        TEST_ASSERT_EQUAL(1, h.saves);
        r.pause();
        TEST_ASSERT_EQUAL(1, h.saves);  // unchanged: not written again
    }
    h.logs.clear();
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        local app = {}
        function app.start()
            local b = store.get("board")
            print(store.get("best"), store.get("name"), store.get("ratio"), math.type(store.get("whole")), store.get("on"))
            print(b[1], b[2], b[3].x, b.name, b[10], store.get("gone"), store.get("missing", "dflt"))
        end
        return app
    )"));
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    const std::string expected("1234\tAnn \"the\" \n\0 player\t0.5\tfloat\ttrue", 39);
    TEST_ASSERT_TRUE(h.logs[0] == expected);  // embedded zero byte survives
    TEST_ASSERT_EQUAL_STRING("1\t2\t3\tb\tfalse\tnil\tdflt", h.logs[1].c_str());
}

void test_broken_save_is_ignored() {
    FakeHost h;
    h.hasSave = true;
    h.save = "os.exit() return {";  // not a table constructor, and no os.exit in its environment
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start("return { start = function() print(store.get('x', 'empty')) end }"));
    TEST_ASSERT_FALSE(r.failed());
    TEST_ASSERT_TRUE(h.logged("empty"));
    TEST_ASSERT_TRUE(h.logged("save data ignored"));
}

void test_save_flushed_periodically_and_on_failure() {
    FakeHost h;
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(R"(
        return {
            key = function(k)
                store.set("n", 1)
                if k.key == "e" then error("oops") end
            end,
        }
    )"));
    r.key(keyEv('a'));
    r.tick(true);
    TEST_ASSERT_EQUAL(0, h.saves);
    h.now += 11000;
    r.tick(true);
    TEST_ASSERT_EQUAL(1, h.saves);
    r.key(keyEv('e'));  // sets again, then fails: what was stored is still written
    TEST_ASSERT_TRUE(r.failed());
    TEST_ASSERT_EQUAL(2, h.saves);
}

// ------------------------------------------------------------ the example apps

static std::string readExample(const std::string& rel) {
    std::string here = __FILE__;  // .../test/test_script/test_script.cpp
    here = here.substr(0, here.rfind('/'));
    std::ifstream f(here + "/../../examples/apps/" + rel);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void runExample(const char* id, const std::vector<uint16_t>& keys) {
    const std::string src = readExample(std::string(id) + "/main.lua");
    TEST_ASSERT_TRUE_MESSAGE(!src.empty(), id);
    Manifest m;
    std::string err;
    TEST_ASSERT_TRUE_MESSAGE(parseManifest(id, readExample(std::string(id) + "/app.ini"), m, err), err.c_str());
    FakeHost h;
    h.chooseAnswer = 0;
    h.promptAnswer = "Tester";
    Runtime r(h, m);
    TEST_ASSERT_TRUE_MESSAGE(r.start(src), r.error().c_str());
    r.draw();
    for (int i = 0; i < 300; i++) {  // ten seconds of updates
        h.now += 33;
        r.tick(true);
        TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    }
    for (uint16_t k : keys) {
        r.key(keyEv(k));
        h.now += 33;
        r.tick(true);
        TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    }
    r.pause();
    h.drawable = false;
    for (int i = 0; i < 100; i++) {
        h.now += 1000;
        r.tick(false);
    }
    h.drawable = true;
    r.draw();
    r.stop();
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_TRUE(h.ops.size() > 5);  // it drew something
}

void test_example_hello() {
    runExample("hello", {'a', key::Up, 'n', 'x'});
    FakeHost h;  // q quits
    Runtime r(h, manifest());
    TEST_ASSERT_TRUE(r.start(readExample("hello/main.lua")));
    r.key(keyEv('q'));
    TEST_ASSERT_TRUE(r.exitRequested());
}

void test_example_snake() { runExample("snake", {key::Enter, key::Down, 'l', ' ', ' ', 'n', key::Enter, 'p'}); }

void test_example_clock() { runExample("clock", {'t', 'c', 't'}); }

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_manifest_parsing);
    RUN_TEST(test_ids_and_paths);
    RUN_TEST(test_lifecycle_and_drawing);
    RUN_TEST(test_no_drawing_in_background);
    RUN_TEST(test_keys);
    RUN_TEST(test_update_interval);
    RUN_TEST(test_sys_and_exit);
    RUN_TEST(test_runtime_error_stops_app);
    RUN_TEST(test_syntax_error_and_bad_return);
    RUN_TEST(test_endless_loop_is_stopped);
    RUN_TEST(test_coroutine_loop_is_stopped);
    RUN_TEST(test_memory_limit);
    RUN_TEST(test_sandbox);
    RUN_TEST(test_deep_nesting_is_an_error_not_a_crash);
    RUN_TEST(test_dialogs_and_redraw);
    RUN_TEST(test_dialog_time_not_counted);
    RUN_TEST(test_fs_and_require);
    RUN_TEST(test_error_in_module_names_its_file);
    RUN_TEST(test_store_round_trip);
    RUN_TEST(test_broken_save_is_ignored);
    RUN_TEST(test_save_flushed_periodically_and_on_failure);
    RUN_TEST(test_example_hello);
    RUN_TEST(test_example_snake);
    RUN_TEST(test_example_clock);
    return UNITY_END();
}
