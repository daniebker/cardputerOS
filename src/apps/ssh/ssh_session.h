// One SSH connection with an interactive shell, running on its own FreeRTOS task.
// The UI thread talks to it through byte streams and a small "question" channel.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class SshSession : public std::enable_shared_from_this<SshSession> {
public:
    enum class State { Idle, Connecting, Open, Closed };
    enum class Ask { None, HostKey, Password };

    struct Config {
        std::string host, user, password;
        std::string keyPath;  // private key file (VFS path) or empty
        int port = 22;
        int cols = 80, rows = 24;
    };

    static std::shared_ptr<SshSession> create() { return std::shared_ptr<SshSession>(new SshSession()); }
    ~SshSession();

    bool start(const Config& cfg);
    // Ask the connection to close. Returns immediately; the task cleans up by itself.
    void stop();

    State state() const { return state_.load(); }
    bool finished() const { return state_.load() == State::Closed; }

    // Terminal output received from the host (plus local status messages).
    size_t read(char* buf, size_t max);
    // Keyboard input for the host.
    void send(const char* data, size_t len);
    void resize(int cols, int rows);
    // Appends `publicKeyLine` to ~/.ssh/authorized_keys on the host (over a separate
    // channel; the result is reported in the terminal output).
    void installKey(const std::string& publicKeyLine);

    // A question from the connection task that the UI must answer.
    //   HostKey:  text = "<key type> <SHA256 fingerprint>"  -> answer(accept)
    //   Password: text = prompt                             -> answer(ok, password)
    bool pendingAsk(Ask& kind, std::string& text);
    void answer(bool ok, const std::string& value = std::string());

private:
    SshSession();
    static void taskEntry(void* arg);
    void run();
    void log(const char* fmt, ...);
    bool ask(Ask kind, const std::string& text, std::string* value);
    bool authenticate(void* session);
    void runInstallKey(void* session, const std::string& line);

    Config cfg_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_{false};
    std::atomic<bool> resizePending_{false};
    std::atomic<int> cols_{80}, rows_{24};
    std::atomic<bool> installPending_{false};
    std::string installLine_;  // guarded by askMutex_

    StreamBufferHandle_t rx_ = nullptr;  // task -> UI
    StreamBufferHandle_t tx_ = nullptr;  // UI -> task
    SemaphoreHandle_t answerSem_ = nullptr;

    std::mutex askMutex_;
    Ask askKind_ = Ask::None;
    std::string askText_;
    bool answerOk_ = false;
    std::string answerValue_;
};
