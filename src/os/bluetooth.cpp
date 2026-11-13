#include "bluetooth.h"

#include <cstring>

namespace bt {

namespace {
const char* current = nullptr;
}

bool claim(const char* app) {
    if (current && strcmp(current, app) != 0) return false;
    current = app;
    return true;
}

void release(const char* app) {
    if (current && strcmp(current, app) == 0) current = nullptr;
}

const char* owner() { return current; }

}  // namespace bt
