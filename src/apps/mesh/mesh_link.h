// Bluetooth LE link to a Meshtastic node (its "phone API"). All Bluetooth work
// runs on a worker task; the UI thread talks to it through queues, like SshSession.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class NimBLEClient;
class NimBLERemoteCharacteristic;

class MeshLink {
public:
    enum class State { Off, Idle, Scanning, Connecting, Connected };

    struct Device {
        std::string name, address;
        uint8_t addressType = 0;
        int rssi = 0;
    };

    ~MeshLink() { end(); }

    bool begin();  // starts Bluetooth and the worker task
    void end();    // stops everything and frees the Bluetooth stack

    State state() const { return state_.load(); }
    // Why the last connect attempt failed or the link dropped ("" if it did not).
    std::string takeError();

    void startScan();                   // ~5 s; state() is Scanning meanwhile
    std::vector<Device> scanResults();  // Meshtastic nodes found by the last scan

    void connect(const Device& d);
    void disconnect();
    // Deletes the stored pairing with a node (needed if the node forgot us).
    void forget(const Device& d);

    // Pairing: when the node asks for a PIN, pinRequested() turns true until answered.
    bool pinRequested() const { return pinRequested_.load(); }
    void providePin(bool ok, uint32_t pin);

    void send(const std::string& toRadio);
    bool receive(std::string& fromRadio);  // next message from the node, if any

    // used by the Bluetooth callbacks
    uint32_t waitForPin();
    void onLinkLost();
    void onNotify() { notified_ = true; }

private:
    enum class Cmd { None, Scan, Connect, Disconnect };
    static void taskEntry(void* arg);
    void run();
    void doScan();
    void doConnect();
    void dropClient();
    void readAll();
    void fail(const std::string& why);

    std::atomic<State> state_{State::Off};
    std::atomic<bool> stop_{false}, pinRequested_{false}, notified_{false}, lost_{false};
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t pinSem_ = nullptr, doneSem_ = nullptr;

    std::mutex mutex_;  // guards everything below
    Cmd cmd_ = Cmd::None;
    Device target_;
    std::vector<Device> found_;
    std::deque<std::string> tx_, rx_;
    std::string error_;
    bool pinOk_ = false;
    uint32_t pin_ = 0;

    // worker task only
    NimBLEClient* client_ = nullptr;
    NimBLERemoteCharacteristic *toRadio_ = nullptr, *fromRadio_ = nullptr, *fromNum_ = nullptr;
    uint32_t lastPoll_ = 0;
};
