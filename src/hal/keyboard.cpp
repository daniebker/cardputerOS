#include "keyboard.h"

#include <M5Cardputer.h>
#include <driver/gpio.h>

namespace hal {

namespace {
// Physical layout: 4 rows x 14 columns (see _key_value_map in M5Cardputer).
constexpr int kCols = 14;
struct Pos {
    int8_t x, y;
};
constexpr Pos kFn{0, 2}, kShift{1, 2}, kCtrl{0, 3}, kOpt{1, 3}, kAlt{2, 3};
constexpr Pos kBackspace{13, 0}, kTab{0, 1}, kEnter{13, 2};

constexpr uint32_t kRepeatDelayMs = 380;
constexpr uint32_t kRepeatRateMs = 45;

uint64_t prevMask = 0;
int repeatBit = -1;
uint32_t repeatAt = 0;

// small FIFO so several keys landing in one poll are not lost
key::Event queue[8];
uint8_t qHead = 0, qCount = 0;

inline uint64_t keyBit(int x, int y) { return 1ULL << (y * kCols + x); }
inline uint64_t keyBit(Pos p) { return keyBit(p.x, p.y); }
constexpr uint64_t modMask() {
    return (1ULL << (2 * kCols + 0)) | (1ULL << (2 * kCols + 1)) | (1ULL << (3 * kCols + 0)) |
           (1ULL << (3 * kCols + 1)) | (1ULL << (3 * kCols + 2));
}

void push(const key::Event& ev) {
    if (qCount >= 8) return;
    queue[(qHead + qCount) % 8] = ev;
    qCount++;
}

// Translate the key at bit index `b` given the currently held modifiers.
key::Event translate(int b, uint64_t mask, bool repeat) {
    using namespace key;
    Event ev;
    ev.repeat = repeat;
    const int x = b % kCols, y = b / kCols;
    const bool fn = mask & keyBit(kFn), shift = mask & keyBit(kShift);
    if (mask & keyBit(kCtrl)) ev.mods |= Ctrl;
    if (mask & keyBit(kAlt)) ev.mods |= Alt;
    if (mask & keyBit(kOpt)) ev.mods |= Opt;
    if (shift) ev.mods |= Shift;

    const auto kv = M5Cardputer.Keyboard.getKeyValue({x, y});
    const char base = kv.value_first;

    if (x == kBackspace.x && y == kBackspace.y) {
        ev.code = fn ? Delete : Backspace;
    } else if (x == kTab.x && y == kTab.y) {
        ev.code = Tab;
    } else if (x == kEnter.x && y == kEnter.y) {
        ev.code = Enter;
    } else if (base == '`' && (ev.mods & Opt)) {
        ev.code = OsHome;
        ev.mods = 0;
    } else if (fn) {
        switch (base) {
            case '`': ev.code = Esc; break;
            case ';': ev.code = Up; break;
            case '.': ev.code = Down; break;
            case ',': ev.code = Left; break;
            case '/': ev.code = Right; break;
            case '-': ev.code = Home; break;
            case '=': ev.code = End; break;
            case '[': ev.code = PageUp; break;
            case ']': ev.code = PageDown; break;
            case '0': ev.code = F10; break;
            default:
                if (base >= '1' && base <= '9') ev.code = F1 + (base - '1');
                else ev.code = shift ? kv.value_second : base;
                break;
        }
    } else {
        ev.code = (uint8_t)(shift ? kv.value_second : base);
    }
    // Shift is already folded into printable characters.
    if (ev.code >= 0x20 && ev.code < 0x7F) ev.mods &= ~Shift;
    return ev;
}
}  // namespace

#ifdef SCREENSHOT_DEBUG
static bool shotRequested = false, sleepTestRequested = false;
bool takeSleepTestRequest() {
    bool r = sleepTestRequested;
    sleepTestRequested = false;
    return r;
}
bool takeScreenshotRequest() {
    bool r = shotRequested;
    shotRequested = false;
    return r;
}

// Serial byte -> key event. 0x1C = launcher, 0x1D = screenshot, 0x1E x = Opt+x,
// 0x1F x = Fn layer key x (arrows etc.), other control bytes = Ctrl+letter.
static void pollSerialKeys() {
    static int pending = 0;
    while (Serial.available()) {
        int c = Serial.read();
        key::Event ev;
        if (pending == 0x1E) {
            ev.code = (uint16_t)c;
            ev.mods = key::Opt;
            pending = 0;
        } else if (pending == 0x1F) {
            pending = 0;
            switch (c) {
                case 'u': ev.code = key::Up; break;
                case 'd': ev.code = key::Down; break;
                case 'l': ev.code = key::Left; break;
                case 'r': ev.code = key::Right; break;
                default: continue;
            }
        } else if (c == 0x1E || c == 0x1F) {
            pending = c;
            continue;
        } else if (c == 0x1D) {
            shotRequested = true;
            continue;
        } else if (c == 0x1A) {
            sleepTestRequested = true;
            continue;
        } else if (c == 0x1C) {
            ev.code = key::OsHome;
        } else if (c == 0x1B) {
            ev.code = key::Esc;
        } else if (c == '\r' || c == '\n') {
            ev.code = key::Enter;
        } else if (c == 0x7F || c == 0x08) {
            ev.code = key::Backspace;
        } else if (c == '\t') {
            ev.code = key::Tab;
        } else if (c < 0x20) {
            ev.code = 'a' + c - 1;
            ev.mods = key::Ctrl;
        } else {
            ev.code = (uint16_t)c;
        }
        push(ev);
    }
}
#endif

namespace {
constexpr uint8_t kTcaAddr = 0x34, kRegIntStat = 0x02, kRegEventCount = 0x03, kRegEvent = 0x04;
constexpr gpio_num_t kTcaIntPin = GPIO_NUM_11;

// Reads and discards everything in the keyboard chip's event FIFO and clears its
// interrupt flags, which lets the interrupt line go high again.
void drainChip() {
    for (int i = 0; i < 16; i++) {
        const int count = M5.In_I2C.readRegister8(kTcaAddr, kRegEventCount, 400000) & 0x0F;
        if (count == 0) break;
        for (int k = 0; k < count; k++) M5.In_I2C.readRegister8(kTcaAddr, kRegEvent, 400000);
    }
    M5.In_I2C.writeRegister8(kTcaAddr, kRegIntStat, 0x0F, 400000);
}
}  // namespace

void keyboardBeforeSleep() {
    // wait until every key is released, then make sure nothing is pending
    const uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
        M5.update();
        M5Cardputer.Keyboard.updateKeyList();
        if (M5Cardputer.Keyboard.keyList().empty() && digitalRead(kTcaIntPin) == HIGH) break;
        delay(10);
    }
    drainChip();
}

void keyboardAfterSleep() {
    // gpio_wakeup_enable() replaced the pin's interrupt type: restore the edge interrupt
    gpio_set_intr_type(kTcaIntPin, GPIO_INTR_ANYEDGE);
    // let the waking key be released, discarding its events
    const uint32_t t0 = millis();
    do {
        drainChip();
        delay(20);
    } while (millis() - t0 < 250 || (digitalRead(kTcaIntPin) == LOW && millis() - t0 < 1500));
    M5.update();  // also clears a G0 button press
    prevMask = 0;
    repeatBit = -1;
    qHead = qCount = 0;
}

void keyboardBegin() {
    prevMask = 0;
    repeatBit = -1;
}

bool keyboardPoll(key::Event& out) {
#ifdef SCREENSHOT_DEBUG
    if (qCount == 0) pollSerialKeys();
#endif
    if (qCount == 0) {
        M5.update();
        M5Cardputer.Keyboard.updateKeyList();

        if (M5.BtnA.wasPressed()) {
            key::Event ev;
            ev.code = key::OsHome;
            push(ev);
        }

        uint64_t mask = 0;
        for (const auto& p : M5Cardputer.Keyboard.keyList()) {
            if (p.x >= 0 && p.x < kCols && p.y >= 0 && p.y < 4) mask |= keyBit(p.x, p.y);
        }
        const uint32_t now = millis();
        uint64_t pressed = mask & ~prevMask & ~modMask();
        prevMask = mask;

        for (int b = 0; pressed; b++, pressed >>= 1) {
            if (!(pressed & 1)) continue;
            push(translate(b, mask, false));
            repeatBit = b;
            repeatAt = now + kRepeatDelayMs;
        }
        if (repeatBit >= 0) {
            if (!(mask & (1ULL << repeatBit))) {
                repeatBit = -1;
            } else if ((int32_t)(now - repeatAt) >= 0) {
                key::Event ev = translate(repeatBit, mask, true);
                if (ev.code != key::OsHome) push(ev);
                repeatAt = now + kRepeatRateMs;
            }
        }
    }
    if (qCount == 0) return false;
    out = queue[qHead];
    qHead = (qHead + 1) % 8;
    qCount--;
    return true;
}

}  // namespace hal
