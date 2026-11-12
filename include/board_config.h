// Hardware configuration: M5Stack Cardputer ADV + external 2.8" ILI9341 (SPI)
// wired to the 14-pin EXT header on the back of the Cardputer.
//
//   ILI9341 pin   EXT header pin   GPIO
//   -----------   --------------   ----
//   VCC           pin 6  (5VOUT)   -      (NOT pin 2 / 5VIN: that has no 5 V on battery)
//   GND           pin 4            -
//   CS            pin 13           G5
//   RESET         pin 1            G3
//   DC / RS       pin 5            G6
//   SDI (MOSI)    pin 9            G14
//   SCK           pin 7            G40
//   LED           pin 6  (5VOUT)   -      (shared with VCC; backlight always on)
//   SDO (MISO)    not connected           (optional: G39)
//
// (Signal pins as in github.com/AndyAiCardputer/zx-spectrum-cardputer-ili9341.)
//
// G40 / G14 / G39 are the same SPI bus the microSD slot uses (SD chip-select is G12).
// The display's SDO pin is not needed; G39 carries the SD card's data out.
// Change the numbers below if your display is wired differently.
#pragma once

#define EXT_TFT_SCK 40
#define EXT_TFT_MOSI 14
#define EXT_TFT_MISO -1  // display read-back: set to 39 if the display's SDO is wired
#define EXT_TFT_CS 5
#define EXT_TFT_DC 6
#define EXT_TFT_RST 3
#define EXT_TFT_BL -1  // GPIO driving the LED pin, or -1 if it is tied to a supply rail

// SPI clock. 40 MHz is fine with short wires; drop to 27000000 or 20000000 if
// the picture shows noise or random pixels.
#define EXT_TFT_FREQ 40000000

// Panel quirks. If colours look swapped (red <-> blue) flip EXT_TFT_BGR,
// if they look like a photo negative flip EXT_TFT_INVERT.
#define EXT_TFT_BGR 0
#define EXT_TFT_INVERT 0

// Built-in microSD slot (shares SCK G40 / MOSI G14 with the display).
#define SD_CS_PIN 12
#define SD_MISO_PIN 39
