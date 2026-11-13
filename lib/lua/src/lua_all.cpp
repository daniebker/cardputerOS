// Lua 5.4.7 (lua.org, MIT licence: see the end of lua.h), built as one C++ unit.
//
// Compiling Lua as C++ makes lua_error() throw a C++ exception instead of using
// longjmp, so the destructors of C++ objects in our bindings run when a script
// error unwinds through them. The Lua sources in this folder are unmodified; only
// the parts used by lib/script are included (no io, package, debug libraries).
//
// Everything that uses Lua must also be C++: include "lua.h", "lauxlib.h" and
// "lualib.h" directly (not lua.hpp, whose extern "C" would not match).

// The C stack is small on the device (the Arduino loop task, 24 KB): limit how
// deeply nested C calls and parser levels may go (Lua's default is 200). Measured
// with -fstack-usage on the ESP32-S3, one level costs up to ~500 bytes (a pcall
// inside a pcall) or ~350 (a nested block in the parser), so 40 levels stay below
// ~20 KB. Deeper nesting is a Lua error ("C stack overflow"), not a crash. Calls
// from Lua function to Lua function do not use the C stack and are not limited.
#if !defined(LUAI_MAXCCALLS)
#define LUAI_MAXCCALLS 40
#endif

// os.tmpname is not offered to scripts; this avoids linking tmpnam().
#define LUA_TMPNAMBUFSIZE 8
#define lua_tmpnam(b, e) \
    {                    \
        (b)[0] = 0;      \
        e = 1;           \
    }

#include "lapi.c"
#include "lcode.c"
#include "lctype.c"
#include "ldebug.c"
#include "ldo.c"
#include "ldump.c"
#include "lfunc.c"
#include "lgc.c"
#include "llex.c"
#include "lmem.c"
#include "lobject.c"
#include "lopcodes.c"
#include "lparser.c"
#include "lstate.c"
#include "lstring.c"
#include "ltable.c"
#include "ltm.c"
#include "lundump.c"
#include "lvm.c"
#include "lzio.c"

#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "lmathlib.c"
#include "loslib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
