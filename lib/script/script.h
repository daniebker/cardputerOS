// Lua app runtime (portable): runs one app script in a sandboxed Lua state and
// connects it to the device through the Host interface. The API offered to
// scripts is documented in docs/lua-apps.md.
//
// Safety: a script can only reach what the Host offers. Its memory is capped
// (allocation fails beyond the limit and the script gets a Lua error), every
// callback has a time limit (an endless loop ends with an error instead of
// freezing the device), only source code can be loaded (no bytecode) and the io,
// package and debug libraries and the dangerous parts of os are not there. Files
// are limited to the app's own folder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "key.h"

struct lua_State;

namespace script {

// ------------------------------------------------------------ app manifest

// Describes one app folder (/cardputerOS/apps/<id>/ with main.lua and app.ini).
struct Manifest {
    std::string id;           // folder name
    std::string name;         // shown on the home screen (default: the id)
    std::string description;  // shown under the icons
    uint32_t color = 0xBD93F9;
    std::string icon;    // name of a built-in icon, "" = first letter of the name
    std::string folder;  // home screen folder ("Games"), "" = the home screen itself
};

// True if `id` can be an app folder name: 1-31 of A-Z a-z 0-9 _ - . (not starting with '.').
bool validId(const std::string& id);
// Reads app.ini ("key = value" lines, '#' or ';' comments). Unknown keys are
// ignored. Returns false with a message for a malformed file.
bool parseManifest(const std::string& id, const std::string& text, Manifest& out, std::string& error);
// Colour from "#RRGGBB" or a colour name (red, green, purple, ...). False if unknown.
bool parseColor(const std::string& text, uint32_t& out);
// Turns a path given by a script into one relative to the app folder ("a/b.txt").
// A leading "/" means the app folder itself. False for "..", empty parts that
// matter, backslashes, control characters or paths longer than 128 characters.
bool cleanPath(const std::string& in, std::string& out);

// ------------------------------------------------------------ host

enum class Font { Small, Normal, Large, Mono, Bold };
enum class Align { Left, Center, Right };

struct FileEntry {
    std::string name;
    bool dir = false;
    uint32_t size = 0;
};

// What the device offers to a script. Colours are 0xRRGGBB.
class Host {
public:
    virtual ~Host() {}

    virtual uint32_t millis() = 0;
    virtual uint32_t random32() = 0;
    // print() output.
    virtual void log(const std::string& line) = 0;
    // True while the OS waits for the app to return (the user pressed home).
    virtual bool interrupted() { return false; }

    // --- drawing; ignored while the app is in the background (canDraw() false)
    virtual bool canDraw() = 0;
    virtual int width() = 0;
    virtual int height() = 0;
    virtual void fillRect(int x, int y, int w, int h, uint32_t c) = 0;
    virtual void drawRect(int x, int y, int w, int h, uint32_t c) = 0;
    virtual void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) = 0;
    virtual void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) = 0;
    virtual void drawLine(int x0, int y0, int x1, int y1, uint32_t c) = 0;
    virtual void drawPixel(int x, int y, uint32_t c) = 0;
    virtual void drawCircle(int x, int y, int r, uint32_t c) = 0;
    virtual void fillCircle(int x, int y, int r, uint32_t c) = 0;
    virtual void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) = 0;
    virtual void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) = 0;
    // `y` is the top of the text; `x` its left edge, centre or right edge.
    // `scale` (1-8) enlarges the font that many times.
    virtual void drawText(const std::string& text, int x, int y, Font f, int scale, Align a, uint32_t fg, bool hasBg,
                          uint32_t bg) = 0;
    // Unscaled sizes.
    virtual int textWidth(const std::string& text, Font f) = 0;
    virtual int fontHeight(Font f) = 0;
    // Title line at the top of the screen; returns the y below it.
    virtual int header(const std::string& title) = 0;
    // Key hint line at the bottom of the screen; returns its top y.
    virtual int footer(const std::string& hint) = 0;

    // --- blocking dialogs (only called while canDraw())
    virtual void alert(const std::string& title, const std::string& message) = 0;
    virtual bool confirm(const std::string& title, const std::string& message, bool defaultYes) = 0;
    virtual bool prompt(const std::string& title, std::string& value, bool secret) = 0;
    // Index into `items`, or -1 if cancelled.
    virtual int choose(const std::string& title, const std::vector<std::string>& items, int initial) = 0;
    virtual void busy(const std::string& message) = 0;

    // --- files in the app's own folder; `path` is already cleaned (see cleanPath)
    virtual bool readFile(const std::string& path, std::string& out, size_t maxBytes, std::string& error) = 0;
    virtual bool writeFile(const std::string& path, const std::string& data, bool append, std::string& error) = 0;
    virtual bool listDir(const std::string& path, std::vector<FileEntry>& out, std::string& error) = 0;
    virtual bool removeFile(const std::string& path, std::string& error) = 0;
    virtual bool exists(const std::string& path) = 0;

    // --- the app's save data (kept in the device's internal flash)
    virtual bool loadSave(std::string& out) = 0;
    virtual bool storeSave(const std::string& data) = 0;

    // --- system
    virtual int64_t unixTime() = 0;  // 0 while the clock is not set
    virtual int battery() = 0;       // percent, -1 if unknown
    virtual std::string wifi() = 0;  // connected network, "" if none
    virtual std::string osVersion() = 0;
};

// ------------------------------------------------------------ runtime

struct Limits {
    size_t memory = 96 * 1024;  // bytes of Lua memory
    uint32_t callMs = 2000;     // longest a callback may run (time in dialogs not counted)
    uint32_t startMs = 5000;    // for main.lua and app.start
    size_t maxFile = 64 * 1024; // largest file fs.read / require / save data may handle
};

class Runtime {
public:
    Runtime(Host& host, const Manifest& manifest, const Limits& limits = Limits());
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Runs main.lua (`source`), which must return the app table, loads the save
    // data and calls app.start(). False on error (see error()).
    bool start(const std::string& source);

    // Lifecycle, forwarded to the app table's functions if it has them. Errors
    // stop the app: failed() becomes true and every later call does nothing.
    void draw();   // the whole screen must be repainted (app.draw)
    void pause();  // leaving the foreground (app.pause), saves
    void stop();   // the app is closed (app.stop), saves
    void key(const key::Event& ev);
    // Call continuously; runs app.update(dt) (foreground) or app.background(dt)
    // every interval milliseconds, and writes changed save data now and then.
    void tick(bool foreground);

    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

    // State set by the script through sys.*
    const std::string& status() const { return status_; }
    const std::string& notice() const { return notice_; }
    bool keepAwake() const { return keepAwake_; }
    bool exitRequested() const { return exit_; }
    uint32_t interval() const { return interval_; }
    size_t memoryUsed() const { return memUsed_; }
    size_t memoryLimit() const { return limits_.memory; }

    // Writes the save data if it changed.
    void flushSave();

    // Converts a key event to its script name ("a", "enter", "up", "f1", ...).
    static std::string keyName(const key::Event& ev);

    // Used by the Lua bindings.
    Host& host() { return host_; }
    const Manifest& manifest() const { return manifest_; }
    const Limits& limits() const { return limits_; }
    // Time spent in a dialog: not counted towards the callback's time limit.
    void excludeTime(uint32_t ms);
    void dialogShown() { dialogShown_ = true; }
    void markSaveDirty() { saveDirty_ = true; }
    void saveWritten();
    void setStatus(const std::string& s) { status_ = s; }
    void setNotice(const std::string& s) { notice_ = s; }
    void setKeepAwake(bool on) { keepAwake_ = on; }
    void requestExit() { exit_ = true; }
    void setInterval(uint32_t ms) { interval_ = ms; }
    int appRef() const { return appRef_; }
    void setAppRef(int ref) { appRef_ = ref; }

private:
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void hook(lua_State* L, struct lua_Debug* ar);
    bool deadlinePassed();
    // Runs `body` (a lua_CFunction getting `ud` as light userdata) protected, with
    // a time limit. On error: fatal -> the app fails, else only logged.
    bool run(int (*body)(lua_State*), void* ud, uint32_t budgetMs, bool fatal);
    bool hasFunction(const char* name);
    // Calls app[name](args pushed by `args`). True if it ran or does not exist.
    bool callApp(const char* name, const std::function<int(lua_State*)>& args, uint32_t budgetMs);
    // After a key / update callback: repaint if a dialog covered the screen.
    void afterCallback();
    void writeSave();
    void fail(const std::string& message);

    Host& host_;
    Manifest manifest_;
    Limits limits_;
    lua_State* L_ = nullptr;
    size_t memUsed_ = 0;
    bool failed_ = false, exit_ = false, keepAwake_ = false, killed_ = false;
    bool dialogShown_ = false, saveDirty_ = false;
    int depth_ = 0;  // a callback is running
    std::string error_, status_, notice_;
    uint32_t callStart_ = 0, budget_ = 0;
    uint32_t interval_ = 33, lastTick_ = 0, lastSave_ = 0;
    bool ticked_ = false;
    int appRef_ = -1;
};

// Save data format: a Lua table constructor ("return {...}") of strings,
// numbers, booleans and nested tables. Exposed for the unit tests.
// Serialises the value at `index`; false with a message for functions, cycles,
// too deep nesting or unsupported keys.
bool serialize(lua_State* L, int index, std::string& out, std::string& error);

}  // namespace script
