#include "mesh_link.h"

#include <NimBLEDevice.h>

namespace {
const NimBLEUUID kService("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
const NimBLEUUID kToRadio("f75c76d2-129e-4dad-a1dd-7866124401e7");
const NimBLEUUID kFromRadio("2c55e69e-4993-11ed-b878-0242ac120002");
const NimBLEUUID kFromNum("ed9da18c-a800-4f66-a670-aa7547e34453");
constexpr size_t kMaxQueued = 80;  // messages waiting for the UI thread

MeshLink* active = nullptr;

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*) override {
        if (active) active->onLinkLost();
    }
    uint32_t onPassKeyRequest() override { return active ? active->waitForPin() : 0; }
    bool onConfirmPIN(uint32_t) override { return true; }
};
ClientCallbacks callbacks;
}  // namespace

bool MeshLink::begin() {
    if (state_.load() != State::Off) return true;
    pinSem_ = xSemaphoreCreateBinary();
    doneSem_ = xSemaphoreCreateBinary();
    if (!pinSem_ || !doneSem_) return false;
    NimBLEDevice::init("cardputer");
    NimBLEDevice::setMTU(517);
    // bond, require a PIN where the node has one, secure connections
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    active = this;
    stop_ = false;
    state_ = State::Idle;
    if (xTaskCreate(taskEntry, "mesh", 8192, this, 1, &task_) != pdPASS) {
        state_ = State::Off;
        NimBLEDevice::deinit(true);
        return false;
    }
    return true;
}

void MeshLink::end() {
    if (state_.load() == State::Off) return;
    stop_ = true;
    providePin(false, 0);
    if (doneSem_) xSemaphoreTake(doneSem_, pdMS_TO_TICKS(8000));  // worker closes the link
    active = nullptr;
    NimBLEDevice::deinit(true);
    if (pinSem_) vSemaphoreDelete(pinSem_);
    if (doneSem_) vSemaphoreDelete(doneSem_);
    pinSem_ = doneSem_ = nullptr;
    state_ = State::Off;
}

std::string MeshLink::takeError() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string e;
    e.swap(error_);
    return e;
}

void MeshLink::startScan() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load() != State::Idle) return;
    cmd_ = Cmd::Scan;
    state_ = State::Scanning;
}

std::vector<MeshLink::Device> MeshLink::scanResults() {
    std::lock_guard<std::mutex> lock(mutex_);
    return found_;
}

void MeshLink::connect(const Device& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load() != State::Idle) return;
    target_ = d;
    cmd_ = Cmd::Connect;
    state_ = State::Connecting;
    lost_ = false;
}

void MeshLink::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    cmd_ = Cmd::Disconnect;
}

void MeshLink::forget(const Device& d) {
    if (state_.load() == State::Off) return;
    NimBLEDevice::deleteBond(NimBLEAddress(d.address, d.addressType));
}

void MeshLink::providePin(bool ok, uint32_t pin) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pinOk_ = ok;
        pin_ = pin;
    }
    pinRequested_ = false;
    if (pinSem_) xSemaphoreGive(pinSem_);
}

void MeshLink::send(const std::string& toRadio) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tx_.size() < 16) tx_.push_back(toRadio);
}

bool MeshLink::receive(std::string& fromRadio) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rx_.empty()) return false;
    fromRadio.swap(rx_.front());
    rx_.pop_front();
    return true;
}

// ---- Bluetooth host task ----

uint32_t MeshLink::waitForPin() {
    xSemaphoreTake(pinSem_, 0);
    pinRequested_ = true;
    // the node gives up after about 30 s; do not wait longer than that
    if (xSemaphoreTake(pinSem_, pdMS_TO_TICKS(60000)) != pdTRUE) {
        pinRequested_ = false;
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return pinOk_ ? pin_ : 0;
}

void MeshLink::onLinkLost() { lost_ = true; }

// ---- worker task ----

void MeshLink::taskEntry(void* arg) {
    MeshLink* self = static_cast<MeshLink*>(arg);
    self->run();
    xSemaphoreGive(self->doneSem_);
    vTaskDelete(nullptr);
}

void MeshLink::fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = why;
}

void MeshLink::dropClient() {
    if (client_) {
        if (client_->isConnected()) client_->disconnect();
        NimBLEDevice::deleteClient(client_);
    }
    client_ = nullptr;
    toRadio_ = fromRadio_ = fromNum_ = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    tx_.clear();
}

void MeshLink::doScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
    NimBLEScanResults results = scan->start(5, false);
    std::vector<Device> found;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        if (!dev.isAdvertisingService(kService)) continue;
        Device d;
        d.name = dev.getName();
        d.address = dev.getAddress().toString();
        d.addressType = dev.getAddress().getType();
        d.rssi = dev.getRSSI();
        if (d.name.empty()) d.name = d.address;
        found.push_back(d);
    }
#ifdef SCREENSHOT_DEBUG
    Serial.printf("\nBLESCAN total=%d meshtastic=%d heap=%u\n", results.getCount(), (int)found.size(), (unsigned)ESP.getFreeHeap());
    for (int i = 0; i < results.getCount() && i < 12; i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        Serial.printf("BLEDEV %s rssi=%d name=%s svc=%d\n", dev.getAddress().toString().c_str(), dev.getRSSI(), dev.getName().c_str(), (int)dev.getServiceUUIDCount());
    }
#endif
    scan->clearResults();
    std::lock_guard<std::mutex> lock(mutex_);
    found_.swap(found);
}

void MeshLink::doConnect() {
    Device d;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        d = target_;
        rx_.clear();
    }
    dropClient();
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(&callbacks, false);
    client_->setConnectTimeout(10);
    if (!client_->connect(NimBLEAddress(d.address, d.addressType))) {
        fail("Could not reach " + d.name + ". Is it switched on and in range?");
        dropClient();
        return;
    }
    // Meshtastic only answers over an encrypted (paired) link.
    if (!client_->secureConnection()) {
        fail("Pairing with " + d.name + " failed (wrong PIN, or the node no longer knows this device).");
        dropClient();
        return;
    }
    NimBLERemoteService* svc = client_->getService(kService);
    if (svc) {
        toRadio_ = svc->getCharacteristic(kToRadio);
        fromRadio_ = svc->getCharacteristic(kFromRadio);
        fromNum_ = svc->getCharacteristic(kFromNum);
    }
    if (!toRadio_ || !fromRadio_ || !fromNum_) {
        fail(d.name + " does not look like a Meshtastic node.");
        dropClient();
        return;
    }
    fromNum_->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t*, size_t, bool) {
        if (active) active->onNotify();
    });
    notified_ = true;  // read whatever is waiting
    lost_ = false;
    state_ = State::Connected;
}

// Reads FromRadio until the node has nothing more.
void MeshLink::readAll() {
    for (int i = 0; i < 400 && !stop_ && client_ && client_->isConnected(); i++) {
        {
            // if the UI thread is not keeping up, stop pulling; the node holds the rest
            std::lock_guard<std::mutex> lock(mutex_);
            if (rx_.size() >= kMaxQueued) {
                notified_ = true;
                return;
            }
        }
        NimBLEAttValue v = fromRadio_->readValue();
        if (v.length() == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        rx_.emplace_back((const char*)v.data(), v.length());
    }
}

void MeshLink::run() {
    while (!stop_) {
        Cmd cmd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmd = cmd_;
            cmd_ = Cmd::None;
        }
        if (cmd == Cmd::Scan) {
            doScan();
            state_ = State::Idle;
        } else if (cmd == Cmd::Connect) {
            doConnect();
            if (state_.load() != State::Connected) state_ = State::Idle;
        } else if (cmd == Cmd::Disconnect) {
            dropClient();
            state_ = State::Idle;
        }

        if (state_.load() == State::Connected) {
            if (lost_ || !client_ || !client_->isConnected()) {
                fail("Connection to the node was lost.");
                dropClient();
                state_ = State::Idle;
                continue;
            }
            std::string out;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!tx_.empty()) {
                    out.swap(tx_.front());
                    tx_.pop_front();
                }
            }
            if (!out.empty()) {
                toRadio_->writeValue((const uint8_t*)out.data(), out.size(), true);
                notified_ = true;  // the node usually answers
            }
            // notifications tell us when to read; also look every few seconds in case one was missed
            if (notified_.exchange(false) || millis() - lastPoll_ > 3000) {
                lastPoll_ = millis();
                readAll();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    dropClient();
}
