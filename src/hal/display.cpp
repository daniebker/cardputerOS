#include "display.h"

#include <Arduino.h>

#include "board_config.h"

namespace hal {

ExtDisplay display;

const uint8_t* Panel_ILI9341::getInitCommands(uint8_t listno) const {
    static constexpr uint8_t list0[] = {
        CMD_SWRESET, 0 + CMD_INIT_DELAY, 150,  // in case the hardware reset was missed
        0xEF, 3, 0x03, 0x80, 0x02,
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x23,        // power control 1
        0xC1, 1, 0x10,        // power control 2
        0xC5, 2, 0x3E, 0x28,  // VCOM control 1
        0xC7, 1, 0x86,        // VCOM control 2
        0xB1, 2, 0x00, 0x13,  // frame rate
        0xF2, 1, 0x00,        // 3-gamma off
        0x26, 1, 0x01,        // gamma curve 1
        0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
        0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
        0xB6, 3, 0x08, 0xC2, 0x27,  // display function control
        CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
        CMD_IDMOFF, 0,
        CMD_DISPON, 0 + CMD_INIT_DELAY, 100,
        0xFF, 0xFF,  // end
    };
    return listno == 0 ? list0 : nullptr;
}

ExtDisplay::ExtDisplay() {
    {
        auto cfg = bus_.config();
        cfg.spi_host = SPI2_HOST;  // the internal LCD owns SPI3
        cfg.spi_mode = 0;
        cfg.freq_write = EXT_TFT_FREQ;
        cfg.freq_read = 16000000;
        cfg.spi_3wire = false;  // 4-wire: G39 is the SD card's data line
        cfg.use_lock = true;
        cfg.pin_sclk = EXT_TFT_SCK;
        cfg.pin_mosi = EXT_TFT_MOSI;
        cfg.pin_miso = SD_MISO_PIN;
        cfg.pin_dc = EXT_TFT_DC;
        bus_.config(cfg);
        panel_.setBus(&bus_);
    }
    {
        auto cfg = panel_.config();
        cfg.pin_cs = EXT_TFT_CS;
        cfg.pin_rst = EXT_TFT_RST;
        cfg.pin_busy = -1;
        cfg.readable = (EXT_TFT_MISO >= 0);
        cfg.invert = EXT_TFT_INVERT;
        cfg.rgb_order = EXT_TFT_BGR;
        cfg.dlen_16bit = false;
        cfg.bus_shared = true;  // shared with the microSD slot
        panel_.config(cfg);
    }
    if (EXT_TFT_BL >= 0) {
        auto cfg = light_.config();
        cfg.pin_bl = EXT_TFT_BL;
        cfg.invert = false;
        cfg.freq = 12000;
        cfg.pwm_channel = 6;
        light_.config(cfg);
        panel_.setLight(&light_);
    }
    setPanel(&panel_);
}

bool ExtDisplay::begin(bool flipped) {
    // Keep the microSD card deselected: it sits on the same SPI pins.
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);

    // Give the panel's supply a moment after power-on, and set it up twice in case
    // the first reset happened while it was still powering up.
    while (millis() < 300) delay(10);
    if (!init()) return false;
    delay(120);
    if (!init()) return false;
    setFlipped(flipped);
    setBrightness(255);
    fillScreen(0);
    return true;
}

void ExtDisplay::setFlipped(bool flipped) { setRotation(flipped ? 3 : 1); }

}  // namespace hal
