#include "ble_scanner.h"

#include <NimBLEDevice.h>

#include "../../os/bluetooth.h"

namespace {

constexpr const char* kOwner = "Wardrive";
constexpr size_t kMaxPending = 96;  // distinct devices between two take() calls

BleScanner* active = nullptr;

class Callbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* adv) override {
        if (!active) return;
        wardrive::Device d;
        d.kind = wardrive::Kind::Ble;
        const NimBLEAddress addr = adv->getAddress();
        const uint8_t* native = addr.getNative();  // least significant byte first
        for (int i = 0; i < 6; i++) d.mac[i] = native[5 - i];
        d.addrType = addr.getType();
        d.rssi = (int8_t)adv->getRSSI();
        if (adv->haveName()) d.name = adv->getName();
        if (adv->haveManufacturerData()) {
            const std::string m = adv->getManufacturerData();
            if (m.size() >= 2) d.company = (uint16_t)((uint8_t)m[0] | ((uint8_t)m[1] << 8));
        }
        active->report(d);
    }
};
Callbacks callbacks;

}  // namespace

bool BleScanner::start() {
    if (running_) return true;
    if (!bt::claim(kOwner)) return false;
    NimBLEDevice::init("cardputer");
    if (!NimBLEDevice::getInitialized()) {
        bt::release(kOwner);
        return false;
    }
    active = this;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&callbacks, true);
    scan->setDuplicateFilter(false);  // keep reporting devices so their signal stays current
    scan->setMaxResults(0);           // report through the callback only, store nothing
    scan->setActiveScan(true);        // ask for scan responses (they often carry the name)
    scan->setInterval(160);
    scan->setWindow(80);  // leave air time for Wi-Fi scans
    if (!scan->start(0, nullptr, false)) {
        active = nullptr;
        NimBLEDevice::deinit(true);
        bt::release(kOwner);
        return false;
    }
    running_ = true;
    return true;
}

void BleScanner::stop() {
    if (!running_) return;
    running_ = false;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(nullptr);
    active = nullptr;
    NimBLEDevice::deinit(true);
    bt::release(kOwner);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
}

void BleScanner::report(const wardrive::Device& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : pending_) {
        if (p.mac != d.mac) continue;
        p.rssi = d.rssi;
        if (!d.name.empty()) p.name = d.name;
        if (d.company != wardrive::kNoCompany) p.company = d.company;
        return;
    }
    if (pending_.size() < kMaxPending) pending_.push_back(d);
}

void BleScanner::take(std::vector<wardrive::Device>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : pending_) out.push_back(std::move(p));
    pending_.clear();
}
