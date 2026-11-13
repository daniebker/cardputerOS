#include "power.h"

#include <M5Cardputer.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include "../hal/display.h"
#include "../hal/keyboard.h"
#include "board_config.h"
#include "net.h"
#include "storage.h"

namespace power {

namespace {
constexpr int kInternalBright = 70, kInternalDim = 8;
constexpr int kExternalBright = 255, kExternalDim = 40;
constexpr gpio_num_t kKeyboardInt = GPIO_NUM_11;  // TCA8418 interrupt line, low while keys are waiting
constexpr gpio_num_t kButton = GPIO_NUM_0;        // G0 button

uint32_t lastActivity = 0;
bool dimmed = false;

void setDim(bool on) {
    if (dimmed == on) return;
    dimmed = on;
    M5Cardputer.Display.setBrightness(on ? kInternalDim : kInternalBright);
    // only has an effect if the display's LED pin is driven by a GPIO (EXT_TFT_BL)
    hal::display.setBrightness(on ? kExternalDim : kExternalBright);
}
}  // namespace

int dimMinutes() { return storage::getInt("dim_min", 2); }
int sleepMinutes() { return storage::getInt("sleep_min", 10); }
void setDimMinutes(int m) { storage::putInt("dim_min", m); }
void setSleepMinutes(int m) { storage::putInt("sleep_min", m); }

bool onBattery() { return !HWCDC::isPlugged(); }

void activity() {
    lastActivity = millis();
    setDim(false);
}

Action tick() {
    if (!onBattery()) {
        activity();  // on USB power nothing dims or sleeps
        return Action::None;
    }
    const uint32_t idleMs = millis() - lastActivity;
    const int dimMin = dimMinutes(), sleepMin = sleepMinutes();
    if (sleepMin > 0 && idleMs >= (uint32_t)sleepMin * 60000u) return Action::Sleep;
    if (dimMin > 0 && idleMs >= (uint32_t)dimMin * 60000u) setDim(true);
    return Action::None;
}

void sleepNow(unsigned timerWakeMs) {
    // --- power down what we can ---
    net::suspend();
    hal::display.fillScreen(0x000000);  // black pixels: the least light a lit backlight lets through
    if (EXT_TFT_BL >= 0) hal::display.sleep();
    M5Cardputer.Display.sleep();

    // --- sleep until a key or the G0 button pulls its line low ---
    hal::keyboardBeforeSleep();
    gpio_wakeup_enable(kKeyboardInt, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(kButton, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    if (timerWakeMs) esp_sleep_enable_timer_wakeup((uint64_t)timerWakeMs * 1000);
    const int64_t sleptAt = esp_timer_get_time();
    esp_light_sleep_start();
#ifdef SCREENSHOT_DEBUG
    Serial.printf("\nSLEPT %d ms, wake cause %d\n", (int)((esp_timer_get_time() - sleptAt) / 1000), (int)esp_sleep_get_wakeup_cause());
#else
    (void)sleptAt;
#endif
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable(kKeyboardInt);
    gpio_wakeup_disable(kButton);

    // --- back to life ---
    hal::keyboardAfterSleep();
    M5Cardputer.Display.wakeup();
    if (EXT_TFT_BL >= 0) hal::display.wakeup();
    dimmed = true;  // force the brightness to be set again
    activity();
    net::resume();
}

}  // namespace power
