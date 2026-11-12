// External ILI9341 display driver (M5GFX / LovyanGFX device).
#pragma once
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#include <lgfx/v1/platforms/esp32/Light_PWM.hpp>

namespace hal {

// M5GFX only ships the ILI9342 variant, so the ILI9341 init sequence lives here.
struct Panel_ILI9341 : public lgfx::Panel_LCD {
    Panel_ILI9341() {
        _cfg.memory_width = _cfg.panel_width = 240;
        _cfg.memory_height = _cfg.panel_height = 320;
    }

protected:
    const uint8_t* getInitCommands(uint8_t listno) const override;
};

class ExtDisplay : public lgfx::LGFX_Device {
public:
    ExtDisplay();
    // flipped = rotate the landscape picture by 180 degrees
    bool begin(bool flipped);
    void setFlipped(bool flipped);

private:
    Panel_ILI9341 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Light_PWM light_;
};

extern ExtDisplay display;

}  // namespace hal
