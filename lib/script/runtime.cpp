// Lua app runtime: the sandboxed state, the lifecycle calls and the API bindings
// (gfx, ui, sys, fs, store, colors, require, print).
//
// Every use of the Lua API that can fail (most can, if only by running out of
// memory) happens inside lua_pcall: an error outside of it would end in Lua's
// panic handler, which aborts. Lua is built as C++ (lib/lua), so errors raised in
// the bindings unwind through C++ frames properly.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "script.h"

namespace script {

namespace {

constexpr int kHookInstructions = 1000;  // how often the time limit is checked
constexpr int kMaxDepth = 16;            // nesting of tables in save data

Runtime& rt(lua_State* L) { return **static_cast<Runtime**>(lua_getextraspace(L)); }

// ------------------------------------------------------------ argument helpers

// Coordinates may be any number (x / 2 is a float in Lua 5.4); they are floored.
int argInt(lua_State* L, int i) {
    const lua_Number n = luaL_checknumber(L, i);
    if (!std::isfinite(n)) return 0;
    if (n > 1e6) return 1000000;
    if (n < -1e6) return -1000000;
    return (int)std::floor(n);
}

int optInt(lua_State* L, int i, int def) { return lua_isnoneornil(L, i) ? def : argInt(L, i); }

// A colour: 0xRRGGBB, or a name such as "red" or "#FF5555".
uint32_t argColor(lua_State* L, int i, uint32_t def) {
    if (lua_isnoneornil(L, i)) return def;
    if (lua_type(L, i) == LUA_TSTRING) {
        uint32_t c;
        if (!parseColor(lua_tostring(L, i), c)) luaL_argerror(L, i, "unknown colour");
        return c;
    }
    return (uint32_t)luaL_checkinteger(L, i) & 0xFFFFFF;
}

Font parseFont(lua_State* L, int tableIdx) {
    lua_getfield(L, tableIdx, "font");
    Font f = Font::Normal;
    if (!lua_isnil(L, -1)) {
        static const char* const names[] = {"small", "normal", "large", "mono", "bold", nullptr};
        f = (Font)luaL_checkoption(L, -1, nullptr, names);
    }
    lua_pop(L, 1);
    return f;
}

Font argFont(lua_State* L, int i) {
    static const char* const names[] = {"small", "normal", "large", "mono", "bold", nullptr};
    return (Font)luaL_checkoption(L, i, "normal", names);
}

std::string argString(lua_State* L, int i) {
    size_t n;
    const char* s = luaL_checklstring(L, i, &n);
    return std::string(s, n);
}

std::string optString(lua_State* L, int i, const char* def) {
    if (lua_isnoneornil(L, i)) return def;
    return argString(L, i);
}

std::string argPath(lua_State* L, int i) {
    std::string clean;
    if (!cleanPath(argString(L, i), clean)) luaL_argerror(L, i, "invalid path (must stay inside the app folder)");
    return clean;
}

// Returns nil, message (the usual Lua convention for failures).
int failResult(lua_State* L, const std::string& error) {
    lua_pushnil(L);
    lua_pushlstring(L, error.data(), error.size());
    return 2;
}

// Time the user spends in a dialog does not count towards the callback's limit.
// (When the OS is waiting for the app to return, dialogs close at once; then the
// time does count, so a script that keeps opening dialogs still ends.)
class DialogScope {
public:
    explicit DialogScope(Runtime& r) : r_(r), t0_(r.host().millis()) { r.dialogShown(); }
    ~DialogScope() {
        if (!r_.host().interrupted()) r_.excludeTime(r_.host().millis() - t0_);
    }

private:
    Runtime& r_;
    uint32_t t0_;
};

// ------------------------------------------------------------ gfx

int gfxWidth(lua_State* L) {
    lua_pushinteger(L, rt(L).host().width());
    return 1;
}
int gfxHeight(lua_State* L) {
    lua_pushinteger(L, rt(L).host().height());
    return 1;
}
int gfxClear(lua_State* L) {
    const uint32_t c = argColor(L, 1, 0x282A36);
    Host& h = rt(L).host();
    if (h.canDraw()) h.fillRect(0, 0, h.width(), h.height(), c);
    return 0;
}
int gfxFillRect(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), w = argInt(L, 3), h = argInt(L, 4);
    const uint32_t c = argColor(L, 5, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().fillRect(x, y, w, h, c);
    return 0;
}
int gfxRect(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), w = argInt(L, 3), h = argInt(L, 4);
    const uint32_t c = argColor(L, 5, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().drawRect(x, y, w, h, c);
    return 0;
}
int gfxFillRoundRect(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), w = argInt(L, 3), h = argInt(L, 4), r = argInt(L, 5);
    const uint32_t c = argColor(L, 6, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().fillRoundRect(x, y, w, h, r, c);
    return 0;
}
int gfxRoundRect(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), w = argInt(L, 3), h = argInt(L, 4), r = argInt(L, 5);
    const uint32_t c = argColor(L, 6, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().drawRoundRect(x, y, w, h, r, c);
    return 0;
}
int gfxLine(lua_State* L) {
    const int x0 = argInt(L, 1), y0 = argInt(L, 2), x1 = argInt(L, 3), y1 = argInt(L, 4);
    const uint32_t c = argColor(L, 5, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().drawLine(x0, y0, x1, y1, c);
    return 0;
}
int gfxPixel(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2);
    const uint32_t c = argColor(L, 3, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().drawPixel(x, y, c);
    return 0;
}
int gfxCircle(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), r = argInt(L, 3);
    const uint32_t c = argColor(L, 4, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().drawCircle(x, y, r, c);
    return 0;
}
int gfxFillCircle(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), r = argInt(L, 3);
    const uint32_t c = argColor(L, 4, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().fillCircle(x, y, r, c);
    return 0;
}
int gfxTriangle(lua_State* L) {
    int p[6];
    for (int i = 0; i < 6; i++) p[i] = argInt(L, i + 1);
    const uint32_t c = argColor(L, 7, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().drawTriangle(p[0], p[1], p[2], p[3], p[4], p[5], c);
    return 0;
}
int gfxFillTriangle(lua_State* L) {
    int p[6];
    for (int i = 0; i < 6; i++) p[i] = argInt(L, i + 1);
    const uint32_t c = argColor(L, 7, 0xF8F8F2);
    if (rt(L).host().canDraw()) rt(L).host().fillTriangle(p[0], p[1], p[2], p[3], p[4], p[5], c);
    return 0;
}

int argScale(lua_State* L, int i) { return std::max(1, std::min(8, optInt(L, i, 1))); }

// gfx.text(text, x, y [, color [, {font=, size=, align=, bg=}]]) -> width in pixels
int gfxText(lua_State* L) {
    lua_settop(L, 5);  // missing arguments are nil (luaL_tolstring pushes onto the stack)
    size_t n;
    const char* s = luaL_tolstring(L, 1, &n);  // any value, like print
    const std::string text(s, n);
    const int x = argInt(L, 2), y = argInt(L, 3);
    const uint32_t fg = argColor(L, 4, 0xF8F8F2);
    Font font = Font::Normal;
    Align align = Align::Left;
    int scale = 1;
    bool hasBg = false;
    uint32_t bg = 0;
    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);
        font = parseFont(L, 5);
        lua_getfield(L, 5, "size");
        scale = argScale(L, lua_gettop(L));
        lua_pop(L, 1);
        lua_getfield(L, 5, "align");
        if (!lua_isnil(L, -1)) {
            static const char* const names[] = {"left", "center", "right", nullptr};
            align = (Align)luaL_checkoption(L, -1, nullptr, names);
        }
        lua_pop(L, 1);
        lua_getfield(L, 5, "bg");
        if (!lua_isnil(L, -1)) {
            hasBg = true;
            bg = argColor(L, lua_gettop(L), 0);
        }
        lua_pop(L, 1);
    }
    Host& h = rt(L).host();
    if (h.canDraw()) h.drawText(text, x, y, font, scale, align, fg, hasBg, bg);
    lua_pushinteger(L, h.textWidth(text, font) * scale);
    return 1;
}

// gfx.text_width(text [, font [, size]])
int gfxTextWidth(lua_State* L) {
    lua_settop(L, 3);
    size_t n;
    const char* s = luaL_tolstring(L, 1, &n);
    const std::string text(s, n);
    lua_pushinteger(L, rt(L).host().textWidth(text, argFont(L, 2)) * argScale(L, 3));
    return 1;
}

// gfx.font_height([font [, size]])
int gfxFontHeight(lua_State* L) {
    lua_pushinteger(L, rt(L).host().fontHeight(argFont(L, 1)) * argScale(L, 2));
    return 1;
}

int gfxRgb(lua_State* L) {
    auto ch = [&](int i) { return (uint32_t)std::max(0, std::min(255, argInt(L, i))); };
    lua_pushinteger(L, (lua_Integer)((ch(1) << 16) | (ch(2) << 8) | ch(3)));
    return 1;
}

const luaL_Reg kGfx[] = {
    {"width", gfxWidth},
    {"height", gfxHeight},
    {"clear", gfxClear},
    {"fill_rect", gfxFillRect},
    {"rect", gfxRect},
    {"fill_round_rect", gfxFillRoundRect},
    {"round_rect", gfxRoundRect},
    {"line", gfxLine},
    {"pixel", gfxPixel},
    {"circle", gfxCircle},
    {"fill_circle", gfxFillCircle},
    {"triangle", gfxTriangle},
    {"fill_triangle", gfxFillTriangle},
    {"text", gfxText},
    {"text_width", gfxTextWidth},
    {"font_height", gfxFontHeight},
    {"rgb", gfxRgb},
    {nullptr, nullptr},
};

// ------------------------------------------------------------ ui

int uiAlert(lua_State* L) {
    const std::string title = argString(L, 1), msg = optString(L, 2, "");
    Runtime& r = rt(L);
    if (!r.host().canDraw()) return 0;
    DialogScope d(r);
    r.host().alert(title, msg);
    return 0;
}

int uiConfirm(lua_State* L) {
    const std::string title = argString(L, 1), msg = optString(L, 2, "");
    const bool def = lua_toboolean(L, 3);
    Runtime& r = rt(L);
    bool yes = false;
    if (r.host().canDraw()) {
        DialogScope d(r);
        yes = r.host().confirm(title, msg, def);
    }
    lua_pushboolean(L, yes);
    return 1;
}

int uiPrompt(lua_State* L) {
    const std::string title = argString(L, 1);
    std::string value = optString(L, 2, "");
    const bool secret = lua_toboolean(L, 3);
    Runtime& r = rt(L);
    bool ok = false;
    if (r.host().canDraw()) {
        DialogScope d(r);
        ok = r.host().prompt(title, value, secret);
    }
    if (!ok) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

int uiChoose(lua_State* L) {
    const std::string title = argString(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> items;
    const lua_Integer n = luaL_len(L, 2);
    if (n > 500) luaL_argerror(L, 2, "too many items (500 at most)");
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, 2, i);
        size_t len;
        const char* s = luaL_tolstring(L, -1, &len);
        items.emplace_back(s, len);
        lua_pop(L, 2);
    }
    const int initial = optInt(L, 3, 1) - 1;
    Runtime& r = rt(L);
    int pick = -1;
    if (r.host().canDraw() && !items.empty()) {
        DialogScope d(r);
        pick = r.host().choose(title, items, std::max(0, std::min((int)items.size() - 1, initial)));
    }
    if (pick < 0) lua_pushnil(L);
    else lua_pushinteger(L, pick + 1);
    return 1;
}

int uiBusy(lua_State* L) {
    const std::string msg = argString(L, 1);
    Host& h = rt(L).host();
    if (h.canDraw()) h.busy(msg);
    return 0;
}

int uiHeader(lua_State* L) {
    const std::string title = argString(L, 1);
    Host& h = rt(L).host();
    lua_pushinteger(L, h.canDraw() ? h.header(title) : 0);
    return 1;
}

int uiFooter(lua_State* L) {
    const std::string hint = argString(L, 1);
    Host& h = rt(L).host();
    lua_pushinteger(L, h.canDraw() ? h.footer(hint) : h.height());
    return 1;
}

const luaL_Reg kUi[] = {
    {"alert", uiAlert},   {"confirm", uiConfirm}, {"prompt", uiPrompt}, {"choose", uiChoose},
    {"busy", uiBusy},     {"header", uiHeader},   {"footer", uiFooter}, {nullptr, nullptr},
};

// ------------------------------------------------------------ sys

int sysMillis(lua_State* L) {
    lua_pushinteger(L, rt(L).host().millis());
    return 1;
}
int sysTime(lua_State* L) {
    const int64_t t = rt(L).host().unixTime();
    if (t <= 0) return 0;
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}
int sysBattery(lua_State* L) {
    const int b = rt(L).host().battery();
    if (b < 0) return 0;
    lua_pushinteger(L, b);
    return 1;
}
int sysWifi(lua_State* L) {
    const std::string s = rt(L).host().wifi();
    if (s.empty()) return 0;
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
int sysExit(lua_State* L) {
    rt(L).requestExit();
    return 0;
}
int sysStatus(lua_State* L) {
    rt(L).setStatus(optString(L, 1, "").substr(0, 80));
    return 0;
}
int sysNotice(lua_State* L) {
    rt(L).setNotice(optString(L, 1, "").substr(0, 80));
    return 0;
}
int sysKeepAwake(lua_State* L) {
    rt(L).setKeepAwake(lua_isnone(L, 1) || lua_toboolean(L, 1));
    return 0;
}
// sys.interval(ms): how often app.update / app.background run (0 = never).
int sysInterval(lua_State* L) {
    if (!lua_isnone(L, 1)) {
        const int ms = argInt(L, 1);
        rt(L).setInterval(ms <= 0 ? 0 : (uint32_t)std::max(10, std::min(60000, ms)));
    }
    lua_pushinteger(L, rt(L).interval());
    return 1;
}
int sysMemory(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)rt(L).memoryUsed());
    lua_pushinteger(L, (lua_Integer)rt(L).memoryLimit());
    return 2;
}

const luaL_Reg kSys[] = {
    {"millis", sysMillis},          {"time", sysTime},   {"battery", sysBattery},   {"wifi", sysWifi},
    {"exit", sysExit},              {"status", sysStatus}, {"notice", sysNotice},   {"interval", sysInterval},
    {"keep_awake", sysKeepAwake}, {"memory", sysMemory}, {nullptr, nullptr},
};

// ------------------------------------------------------------ fs (the app's folder on the SD card)

int fsRead(lua_State* L) {
    const std::string path = argPath(L, 1);
    Runtime& r = rt(L);
    std::string data, err;
    if (!r.host().readFile(path, data, r.limits().maxFile, err)) return failResult(L, err);
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int fsWriteMode(lua_State* L, bool append) {
    const std::string path = argPath(L, 1);
    size_t n;
    const char* d = luaL_checklstring(L, 2, &n);
    std::string err;
    if (path.empty()) return failResult(L, "no file name");
    if (!rt(L).host().writeFile(path, std::string(d, n), append, err)) return failResult(L, err);
    lua_pushboolean(L, 1);
    return 1;
}
int fsWrite(lua_State* L) { return fsWriteMode(L, false); }
int fsAppend(lua_State* L) { return fsWriteMode(L, true); }

int fsList(lua_State* L) {
    std::string path;
    if (!lua_isnoneornil(L, 1)) path = argPath(L, 1);
    std::vector<FileEntry> entries;
    std::string err;
    if (!rt(L).host().listDir(path, entries, err)) return failResult(L, err);
    lua_createtable(L, (int)entries.size(), 0);
    int i = 1;
    for (const FileEntry& e : entries) {
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, e.name.data(), e.name.size());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, e.dir);
        lua_setfield(L, -2, "dir");
        lua_pushinteger(L, e.size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int fsExists(lua_State* L) {
    const std::string path = argPath(L, 1);
    lua_pushboolean(L, rt(L).host().exists(path));
    return 1;
}

int fsRemove(lua_State* L) {
    const std::string path = argPath(L, 1);
    std::string err;
    if (path.empty() || !rt(L).host().removeFile(path, err)) return failResult(L, err.empty() ? "no file name" : err);
    lua_pushboolean(L, 1);
    return 1;
}

const luaL_Reg kFs[] = {
    {"read", fsRead}, {"write", fsWrite},   {"append", fsAppend}, {"list", fsList},
    {"exists", fsExists}, {"remove", fsRemove}, {nullptr, nullptr},
};

// ------------------------------------------------------------ store (save data)

constexpr const char* kSaveKey = "script.save";

void pushSaveTable(lua_State* L) { lua_getfield(L, LUA_REGISTRYINDEX, kSaveKey); }

int storeGet(lua_State* L) {
    luaL_checkstring(L, 1);
    lua_settop(L, 2);  // 2: the default (nil if not given)
    pushSaveTable(L);
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) lua_pushvalue(L, 2);
    return 1;
}

int storeSet(lua_State* L) {
    luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    const int t = lua_type(L, 2);
    if (t != LUA_TNIL && t != LUA_TBOOLEAN && t != LUA_TNUMBER && t != LUA_TSTRING && t != LUA_TTABLE)
        luaL_argerror(L, 2, "only strings, numbers, booleans and tables can be stored");
    if (t == LUA_TTABLE) {  // check now, so the error points at this line
        std::string out, err;
        if (!serialize(L, 2, out, err)) luaL_argerror(L, 2, err.c_str());
    }
    pushSaveTable(L);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    rt(L).markSaveDirty();
    return 0;
}

int storeSave(lua_State* L) {
    Runtime& r = rt(L);
    r.markSaveDirty();
    pushSaveTable(L);
    std::string out, err;
    if (!serialize(L, lua_gettop(L), out, err)) return failResult(L, err);
    if (out.size() > r.limits().maxFile) return failResult(L, "save data too large");
    if (!r.host().storeSave(out)) return failResult(L, "could not write the save data");
    r.saveWritten();
    lua_pushboolean(L, 1);
    return 1;
}

const luaL_Reg kStore[] = {{"get", storeGet}, {"set", storeSet}, {"save", storeSave}, {nullptr, nullptr}};

// ------------------------------------------------------------ base library replacements

int luaPrint(lua_State* L) {
    const int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    rt(L).host().log(line);
    return 0;
}

// load() that only accepts source code: precompiled chunks are not checked by Lua
// and can crash the interpreter.
int luaSafeLoad(lua_State* L) {
    if (lua_gettop(L) < 3) lua_settop(L, 3);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

constexpr const char* kLoadedKey = "script.loaded";

// require("name") loads name.lua (dots are folders: "lib.util" -> lib/util.lua)
// from the app folder, once.
int luaRequire(lua_State* L) {
    const std::string name = argString(L, 1);
    if (name.empty() || name.size() > 64) luaL_argerror(L, 1, "invalid module name");
    for (char c : name)
        if (!isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') luaL_argerror(L, 1, "invalid module name");
    lua_getfield(L, LUA_REGISTRYINDEX, kLoadedKey);
    const int loaded = lua_gettop(L);
    lua_getfield(L, loaded, name.c_str());
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    std::string path = name;
    for (char& c : path)
        if (c == '.') c = '/';
    path += ".lua";
    std::string clean;
    if (!cleanPath(path, clean)) luaL_argerror(L, 1, "invalid module name");
    Runtime& r = rt(L);
    std::string src, err;
    if (!r.host().readFile(clean, src, r.limits().maxFile, err))
        return luaL_error(L, "module '%s' not found: %s: %s", name.c_str(), clean.c_str(), err.c_str());
    const std::string chunk = "@" + clean;
    if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
    lua_pushstring(L, name.c_str());
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, loaded, name.c_str());
    return 1;
}

// Error handler: adds a stack traceback to the message.
int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

void setField(lua_State* L, const char* table, const char* key) {  // table.key = nil
    lua_getglobal(L, table);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, key);
    }
    lua_pop(L, 1);
}

struct Color {
    const char* name;
    uint32_t rgb;
};
const Color kThemeColors[] = {
    // UI roles (same as the built-in apps)
    {"bg", 0x282A36}, {"panel", 0x21222C}, {"text", 0xF8F8F2}, {"dim", 0x6272A4}, {"accent", 0xBD93F9},
    {"select", 0x44475A}, {"ok", 0x50FA7B}, {"warn", 0xFFB86C}, {"error", 0xFF5555},
    // Dracula palette
    {"background", 0x282A36}, {"current_line", 0x44475A}, {"foreground", 0xF8F8F2}, {"comment", 0x6272A4},
    {"cyan", 0x8BE9FD}, {"green", 0x50FA7B}, {"orange", 0xFFB86C}, {"pink", 0xFF79C6}, {"purple", 0xBD93F9},
    {"red", 0xFF5555}, {"yellow", 0xF1FA8C}, {"darker", 0x21222C}, {"darkest", 0x191A21},
    {"black", 0x000000}, {"white", 0xFFFFFF},
};

void newModule(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

// Opens the libraries and the API (runs protected).
int openAll(lua_State* L) {
    Runtime& r = rt(L);
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    lua_settop(L, 0);

    // remove what reaches outside the sandbox
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    for (const char* k : {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"}) setField(L, "os", k);
    lua_getglobal(L, "load");
    lua_pushcclosure(L, luaSafeLoad, 1);
    lua_setglobal(L, "load");
    lua_pushcfunction(L, luaPrint);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, luaRequire);
    lua_setglobal(L, "require");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kLoadedKey);

    // seed math.random from the hardware (Lua's own seed is the same on every boot)
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, (lua_Integer)r.host().random32());
    lua_pushinteger(L, (lua_Integer)r.host().random32());
    lua_call(L, 2, 0);
    lua_pop(L, 1);

    newModule(L, "gfx", kGfx);
    newModule(L, "ui", kUi);
    newModule(L, "fs", kFs);
    newModule(L, "store", kStore);
    newModule(L, "sys", kSys);
    lua_getglobal(L, "sys");
    const std::string version = r.host().osVersion();
    lua_pushlstring(L, version.data(), version.size());
    lua_setfield(L, -2, "version");
    lua_pushstring(L, r.manifest().id.c_str());
    lua_setfield(L, -2, "app");
    lua_pushstring(L, r.manifest().name.c_str());
    lua_setfield(L, -2, "name");
    lua_pop(L, 1);
    lua_createtable(L, 0, (int)(sizeof kThemeColors / sizeof kThemeColors[0]));
    for (const Color& c : kThemeColors) {
        lua_pushinteger(L, c.rgb);
        lua_setfield(L, -2, c.name);
    }
    lua_setglobal(L, "colors");

    // the save data starts empty; Runtime::loadSave fills it
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kSaveKey);
    lua_gc(L, LUA_GCINC, 120, 0, 0);  // collect sooner than the default: memory is short
    return 0;
}

// ------------------------------------------------------------ serialisation

void quote(std::string& out, const char* s, size_t n) {
    out += '"';
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\%03u", c);
            out += buf;
        } else {
            out += (char)c;
        }
    }
    out += '"';
}

void number(lua_State* L, int idx, std::string& out) {
    char buf[40];
    if (lua_isinteger(L, idx)) {
        snprintf(buf, sizeof buf, LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, idx));
        out += buf;
        return;
    }
    const lua_Number v = lua_tonumber(L, idx);
    if (std::isnan(v)) out += "(0/0)";
    else if (std::isinf(v)) out += v > 0 ? "1e9999" : "-1e9999";
    else {
        snprintf(buf, sizeof buf, "%.17g", (double)v);
        out += buf;
        if (!strpbrk(buf, ".eEn")) out += ".0";  // stays a float when read back
    }
}

bool serializeValue(lua_State* L, int idx, std::string& out, std::string& error, int depth,
                    std::set<const void*>& seen) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return true;
        case LUA_TNUMBER: number(L, idx, out); return true;
        case LUA_TSTRING: {
            size_t n;
            const char* s = lua_tolstring(L, idx, &n);
            quote(out, s, n);
            return true;
        }
        case LUA_TTABLE: break;
        default: error = std::string("cannot store a ") + luaL_typename(L, idx); return false;
    }
    if (depth >= kMaxDepth) {
        error = "tables nested too deeply";
        return false;
    }
    const void* p = lua_topointer(L, idx);
    if (seen.count(p)) {
        error = "a table contains itself";
        return false;
    }
    seen.insert(p);
    if (!lua_checkstack(L, 3)) {
        error = "out of stack";
        return false;
    }
    idx = lua_absindex(L, idx);
    out += '{';
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const int kt = lua_type(L, -2);
        if (kt == LUA_TSTRING) {
            size_t n;
            const char* s = lua_tolstring(L, -2, &n);
            out += '[';
            quote(out, s, n);
            out += "]=";
        } else if (kt == LUA_TNUMBER) {
            out += '[';
            number(L, -2, out);
            out += "]=";
        } else if (kt == LUA_TBOOLEAN) {
            out += lua_toboolean(L, -2) ? "[true]=" : "[false]=";
        } else {
            error = std::string("cannot store a table key of type ") + luaL_typename(L, -2);
            lua_pop(L, 2);
            return false;
        }
        if (!serializeValue(L, -1, out, error, depth + 1, seen)) {
            lua_pop(L, 2);
            return false;
        }
        out += ',';
        lua_pop(L, 1);
    }
    out += '}';
    seen.erase(p);
    return true;
}

}  // namespace

bool serialize(lua_State* L, int index, std::string& out, std::string& error) {
    std::set<const void*> seen;
    out = "return ";
    return serializeValue(L, index, out, error, 0, seen);
}


// ------------------------------------------------------------ Runtime

namespace {

// Bodies run by Runtime::run (protected). Argument 1 is the light userdata given to run().

int bodyLoadSave(lua_State* L) {
    const std::string* text = static_cast<const std::string*>(lua_touserdata(L, 1));
    if (luaL_loadbufferx(L, text->data(), text->size(), "=save data", "t") != LUA_OK) return lua_error(L);
    lua_newtable(L);  // empty environment: the data is only a table constructor
    lua_setupvalue(L, -2, 1);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "save data is not a table");
    lua_setfield(L, LUA_REGISTRYINDEX, kSaveKey);
    return 0;
}

int bodyMain(lua_State* L) {
    const std::string* src = static_cast<const std::string*>(lua_touserdata(L, 1));
    if (luaL_loadbufferx(L, src->data(), src->size(), "@main.lua", "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1))
        return luaL_error(L, "main.lua must end with 'return app' (a table of functions), but returned %s",
                          luaL_typename(L, -1));
    rt(L).setAppRef(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

struct CallArgs {
    const char* name;
    const std::function<int(lua_State*)>* args;
};

int bodyCall(lua_State* L) {
    const CallArgs* c = static_cast<const CallArgs*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, rt(L).appRef());
    lua_pushstring(L, c->name);
    lua_rawget(L, 1);
    if (!lua_isfunction(L, -1)) return 0;
    const int n = (c->args && *c->args) ? (*c->args)(L) : 0;
    lua_call(L, n, 0);
    return 0;
}

}  // namespace

Runtime::Runtime(Host& host, const Manifest& manifest, const Limits& limits)
    : host_(host), manifest_(manifest), limits_(limits) {}

Runtime::~Runtime() {
    if (L_) lua_close(L_);
}

void* Runtime::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Runtime* r = static_cast<Runtime*>(ud);
    if (!ptr) osize = 0;  // osize is then the type of object being created
    if (nsize == 0) {
        free(ptr);
        r->memUsed_ -= osize;
        return nullptr;
    }
    if (nsize > osize && r->memUsed_ - osize + nsize > r->limits_.memory) return nullptr;  // Lua: "not enough memory"
    void* p = realloc(ptr, nsize);
    if (p) r->memUsed_ = r->memUsed_ - osize + nsize;
    return p;
}

bool Runtime::deadlinePassed() {
    if (!killed_ && host_.millis() - callStart_ > budget_) killed_ = true;
    return killed_;
}

void Runtime::excludeTime(uint32_t ms) { callStart_ += ms; }

void Runtime::hook(lua_State* L, lua_Debug*) {
    Runtime& r = rt(L);
    if (r.depth_ > 0 && r.deadlinePassed()) {
        // From now on every instruction fails, so a pcall cannot keep the loop going.
        lua_sethook(L, hook, LUA_MASKCOUNT, 1);
        luaL_error(L, "stopped: ran for more than %d ms without returning (endless loop?)", (int)r.budget_);
    }
}

bool Runtime::run(int (*body)(lua_State*), void* ud, uint32_t budgetMs, bool fatal) {
    if (!L_ || failed_ || depth_ > 0) return false;
    const int top = lua_gettop(L_);
    // none of these allocate, so nothing can fail outside the protected call
    lua_pushcfunction(L_, traceback);
    lua_pushcfunction(L_, body);
    lua_pushlightuserdata(L_, ud);
    depth_++;
    callStart_ = host_.millis();
    budget_ = budgetMs;
    const int rc = lua_pcall(L_, 1, 0, top + 1);
    depth_--;
    if (rc == LUA_OK) {
        lua_settop(L_, top);
        return true;
    }
    std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "unknown error";
    if (rc == LUA_ERRMEM)
        msg = "not enough memory (this app may use " + std::to_string(limits_.memory / 1024) + " KB)";
    lua_settop(L_, top);
    if (fatal) {
        fail(msg);
    } else {
        host_.log(msg);
        killed_ = false;
        lua_sethook(L_, hook, LUA_MASKCOUNT, kHookInstructions);
    }
    return false;
}

bool Runtime::hasFunction(const char* name) {
    if (!L_ || failed_ || appRef_ < 0) return false;
    // lua_getfield-free lookup that cannot fail: the names are already interned
    // strings once main.lua defined them, and a missing name is just not found.
    lua_rawgeti(L_, LUA_REGISTRYINDEX, appRef_);
    const int t = lua_gettop(L_);
    lua_pushnil(L_);
    bool found = false;
    while (!found && lua_next(L_, t)) {
        if (lua_type(L_, -2) == LUA_TSTRING && strcmp(lua_tostring(L_, -2), name) == 0) found = lua_isfunction(L_, -1);
        lua_pop(L_, 1);
        if (found) lua_pop(L_, 1);
    }
    lua_settop(L_, t - 1);
    return found;
}

bool Runtime::callApp(const char* name, const std::function<int(lua_State*)>& args, uint32_t budgetMs) {
    if (!hasFunction(name)) return !failed_;
    CallArgs c{name, &args};
    return run(bodyCall, &c, budgetMs, true);
}

void Runtime::fail(const std::string& message) {
    if (failed_) return;
    error_ = message;
    host_.log("error: " + message);
    if (L_) {
        // keep what the app stored so far (no Lua code runs for this)
        killed_ = false;
        lua_sethook(L_, nullptr, 0, 0);
        if (saveDirty_ && depth_ == 0) writeSave();
        lua_close(L_);  // free the memory right away
        L_ = nullptr;
    }
    failed_ = true;
}

bool Runtime::start(const std::string& source) {
    L_ = lua_newstate(alloc, this);
    if (!L_) {
        failed_ = true;
        error_ = "not enough memory to start the app";
        return false;
    }
    *static_cast<Runtime**>(lua_getextraspace(L_)) = this;
    lua_atpanic(L_, [](lua_State* L) -> int {
        rt(L).host().log(std::string("PANIC: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        return 0;  // Lua aborts after this; it should never get here
    });
    lua_sethook(L_, hook, LUA_MASKCOUNT, kHookInstructions);
    if (!run(openAll, nullptr, limits_.startMs, true)) return false;

    std::string save;
    if (host_.loadSave(save) && !save.empty()) {
        // a broken save file must not stop the app: it starts with empty data
        if (save.size() > limits_.maxFile) host_.log("save data too large, ignored");
        else if (!run(bodyLoadSave, &save, limits_.startMs, false)) host_.log("save data ignored");
    }
    std::string src = source;
    if (!run(bodyMain, &src, limits_.startMs, true)) return false;
    lastSave_ = host_.millis();
    if (!callApp("start", nullptr, limits_.startMs)) return false;
    dialogShown_ = false;  // draw() comes next anyway
    return true;
}

void Runtime::flushSave() {
    if (!saveDirty_ || !L_ || failed_ || depth_ > 0) return;
    writeSave();
}

void Runtime::writeSave() {
    // Serialising uses only stack operations and C++ strings (no Lua memory).
    const int top = lua_gettop(L_);
    lua_getfield(L_, LUA_REGISTRYINDEX, kSaveKey);
    std::string out, err;
    const bool ok = lua_istable(L_, -1) && serialize(L_, lua_gettop(L_), out, err);
    lua_settop(L_, top);
    if (!ok) {
        host_.log("save data not written: " + err);
        saveDirty_ = false;  // would fail again
        return;
    }
    if (out.size() > limits_.maxFile) {
        host_.log("save data not written: too large");
        saveDirty_ = false;
        return;
    }
    if (host_.storeSave(out)) saveWritten();
}

void Runtime::saveWritten() {
    saveDirty_ = false;
    lastSave_ = host_.millis();
}

void Runtime::draw() {
    callApp("draw", nullptr, limits_.callMs);
    dialogShown_ = false;
}

void Runtime::pause() {
    callApp("pause", nullptr, limits_.callMs);
    dialogShown_ = false;
    flushSave();
}

void Runtime::stop() {
    callApp("stop", nullptr, limits_.callMs);
    dialogShown_ = false;
    flushSave();
}

void Runtime::afterCallback() {
    // A dialog drew over the screen: let the app repaint.
    const bool redraw = dialogShown_ && !failed_ && host_.canDraw() && !host_.interrupted();
    dialogShown_ = false;
    if (redraw) draw();
}

std::string Runtime::keyName(const key::Event& ev) {
    switch (ev.code) {
        case key::Backspace: return "backspace";
        case key::Tab: return "tab";
        case key::Enter: return "enter";
        case key::Esc: return "esc";
        case ' ': return "space";
        case key::Up: return "up";
        case key::Down: return "down";
        case key::Left: return "left";
        case key::Right: return "right";
        case key::Home: return "home";
        case key::End: return "end";
        case key::PageUp: return "pageup";
        case key::PageDown: return "pagedown";
        case key::Delete: return "delete";
        default: break;
    }
    if (ev.code >= key::F1 && ev.code <= key::F10) return "f" + std::to_string(ev.code - key::F1 + 1);
    if (ev.printable()) return std::string(1, (char)ev.code);
    return "";
}

void Runtime::key(const key::Event& ev) {
    const std::string name = keyName(ev);
    if (name.empty()) return;
    const key::Event e = ev;
    callApp(
        "key",
        [&name, e](lua_State* L) {
            lua_createtable(L, 0, 7);
            lua_pushstring(L, name.c_str());
            lua_setfield(L, -2, "key");
            if (e.isChar()) {
                const char c = (char)e.code;
                lua_pushlstring(L, &c, 1);
                lua_setfield(L, -2, "char");
            }
            lua_pushboolean(L, e.ctrl());
            lua_setfield(L, -2, "ctrl");
            lua_pushboolean(L, e.alt());
            lua_setfield(L, -2, "alt");
            lua_pushboolean(L, e.shift());
            lua_setfield(L, -2, "shift");
            lua_pushboolean(L, e.opt());
            lua_setfield(L, -2, "opt");
            lua_pushboolean(L, e.repeat);
            lua_setfield(L, -2, "repeated");
            return 1;
        },
        limits_.callMs);
    afterCallback();
}

void Runtime::tick(bool foreground) {
    if (!L_ || failed_) return;
    const uint32_t now = host_.millis();
    if (!ticked_) {
        ticked_ = true;
        lastTick_ = now;
    }
    if (interval_ && now - lastTick_ >= interval_) {
        const lua_Integer dt = now - lastTick_;
        lastTick_ = now;
        callApp(
            foreground ? "update" : "background",
            [dt](lua_State* L) {
                lua_pushinteger(L, dt);
                return 1;
            },
            limits_.callMs);
        if (foreground) afterCallback();
        else dialogShown_ = false;
    }
    if (saveDirty_ && host_.millis() - lastSave_ > 10000) flushSave();
}

}  // namespace script
