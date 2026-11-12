#include "sdcard.h"

#include <SD.h>
#include <SPI.h>

#include "board_config.h"

namespace hal {

SdResult sdWriteFile(const char* path, const std::string& data) {
    // The display driver already started the shared bus as the Arduino SPI object.
    if (!SD.begin(SD_CS_PIN, SPI, 20000000, "/sd", 2)) {
        SD.end();
        return SdResult::NoCard;
    }
    SdResult result = SdResult::Error;
    std::string dir(path);
    dir = dir.substr(0, dir.rfind('/'));
    if (dir.empty() || SD.exists(dir.c_str()) || SD.mkdir(dir.c_str())) {
        File f = SD.open(path, FILE_WRITE);
        if (f) {
            const bool ok = f.write((const uint8_t*)data.data(), data.size()) == data.size();
            f.close();
            if (ok) result = SdResult::Ok;
        }
    }
    SD.end();  // unmount so the card can be removed at any time
    digitalWrite(SD_CS_PIN, HIGH);
    return result;
}

}  // namespace hal
