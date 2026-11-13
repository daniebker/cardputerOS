// Continuous Bluetooth LE scan for Wardrive. Advertisements arrive on the
// Bluetooth host task and are collected here until the UI thread takes them.
#pragma once
#include <mutex>
#include <vector>

#include "wardrive.h"

class BleScanner {
public:
    ~BleScanner() { stop(); }

    // Claims Bluetooth and starts scanning. False if Bluetooth is used by another
    // app (bt::owner()) or could not be started.
    bool start();
    void stop();  // stops scanning and shuts the Bluetooth stack down
    bool running() const { return running_; }

    // Moves the sightings collected since the last call into `out` (kind == Ble,
    // one entry per device with its latest signal).
    void take(std::vector<wardrive::Device>& out);

    // used by the scan callback
    void report(const wardrive::Device& d);

private:
    bool running_ = false;
    std::mutex mutex_;
    std::vector<wardrive::Device> pending_;
};
