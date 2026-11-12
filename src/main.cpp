// cardputerOS — entry point. Everything interesting lives in src/os and src/apps.
#include <Arduino.h>

#include "os/kernel.h"

void setup() { os::begin(); }

void loop() { os::loopOnce(); }
