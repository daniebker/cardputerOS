// Battery saving: on battery, dim the screens after a while without key presses
// and put the device into light sleep after a longer while. Any key wakes it.
#pragma once

namespace power {

// Settings (minutes, 0 = off), stored in NVS.
int dimMinutes();
int sleepMinutes();
void setDimMinutes(int minutes);
void setSleepMinutes(int minutes);

// True when no USB host is connected (a plain charger without data lines
// cannot be told apart from running on battery).
bool onBattery();

// A key was pressed: restarts the timers and undoes dimming.
void activity();

enum class Action { None, Sleep };
// Call regularly. Dims when due; returns Sleep when the device should sleep now.
Action tick();

// Blocks in light sleep until a key (or the G0 button) is pressed, then restores
// the screens, keyboard and Wi-Fi. `timerWakeMs` > 0 also wakes after that time.
void sleepNow(unsigned timerWakeMs = 0);

}  // namespace power
