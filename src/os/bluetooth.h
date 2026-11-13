// Bluetooth is used by one app at a time (the NimBLE stack is a single global
// object). An app claims it before starting the stack and releases it after
// shutting the stack down.
#pragma once

namespace bt {

// Returns false if another app holds Bluetooth (see owner()).
bool claim(const char* app);
void release(const char* app);
// Name of the app using Bluetooth, nullptr if it is free.
const char* owner();

}  // namespace bt
