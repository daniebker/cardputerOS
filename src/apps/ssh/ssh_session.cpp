#include "ssh_session.h"

#include <libssh_esp32.h>

#include <libssh/libssh.h>

#include "ssh_keys.h"

#include <cstdarg>

namespace {
constexpr size_t kRxSize = 8192;
constexpr size_t kTxSize = 1024;
constexpr uint32_t kTaskStack = 51200;  // libssh's key exchange is stack hungry
constexpr uint32_t kKeepAliveMs = 30000;
}  // namespace

SshSession::SshSession() {
    rx_ = xStreamBufferCreate(kRxSize, 1);
    tx_ = xStreamBufferCreate(kTxSize, 1);
    answerSem_ = xSemaphoreCreateBinary();
}

SshSession::~SshSession() {
    if (rx_) vStreamBufferDelete(rx_);
    if (tx_) vStreamBufferDelete(tx_);
    if (answerSem_) vSemaphoreDelete(answerSem_);
}

bool SshSession::start(const Config& cfg) {
    if (state_.load() != State::Idle || !rx_ || !tx_ || !answerSem_) return false;
    sshkeys::libInit();
    cfg_ = cfg;
    cols_ = cfg.cols;
    rows_ = cfg.rows;
    state_ = State::Connecting;
    // The task owns a reference so the object outlives the UI letting go of it.
    auto* self = new std::shared_ptr<SshSession>(shared_from_this());
    if (xTaskCreate(taskEntry, "ssh", kTaskStack, self, 1, nullptr) != pdPASS) {
        delete self;
        state_ = State::Closed;
        return false;
    }
    return true;
}

void SshSession::stop() {
    stop_ = true;
    answer(false);
}

size_t SshSession::read(char* buf, size_t max) { return xStreamBufferReceive(rx_, buf, max, 0); }

void SshSession::send(const char* data, size_t len) {
    if (state_.load() != State::Open || len == 0) return;
    xStreamBufferSend(tx_, data, len, pdMS_TO_TICKS(50));
}

void SshSession::resize(int cols, int rows) {
    cols_ = cols;
    rows_ = rows;
    resizePending_ = true;
}

void SshSession::installKey(const std::string& publicKeyLine) {
    {
        std::lock_guard<std::mutex> lock(askMutex_);
        installLine_ = publicKeyLine;
    }
    installPending_ = true;
}

bool SshSession::pendingAsk(Ask& kind, std::string& text) {
    std::lock_guard<std::mutex> lock(askMutex_);
    if (askKind_ == Ask::None) return false;
    kind = askKind_;
    text = askText_;
    return true;
}

void SshSession::answer(bool ok, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(askMutex_);
        if (askKind_ == Ask::None) return;
        askKind_ = Ask::None;
        answerOk_ = ok;
        answerValue_ = value;
    }
    xSemaphoreGive(answerSem_);
}

// ---- everything below runs on the connection task ----

void SshSession::taskEntry(void* arg) {
    auto* self = static_cast<std::shared_ptr<SshSession>*>(arg);
    (*self)->run();
    (*self)->state_ = State::Closed;
    delete self;
    vTaskDelete(nullptr);
}

void SshSession::log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    xStreamBufferSend(rx_, buf, n, pdMS_TO_TICKS(500));
}

bool SshSession::ask(Ask kind, const std::string& text, std::string* value) {
    xSemaphoreTake(answerSem_, 0);  // drop any stale answer
    {
        std::lock_guard<std::mutex> lock(askMutex_);
        askKind_ = kind;
        askText_ = text;
    }
    while (xSemaphoreTake(answerSem_, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (stop_) {
            std::lock_guard<std::mutex> lock(askMutex_);
            askKind_ = Ask::None;
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(askMutex_);
    if (value) *value = answerValue_;
    answerValue_.assign(answerValue_.size(), '\0');
    answerValue_.clear();
    return answerOk_ && !stop_;
}

bool SshSession::authenticate(void* sessionPtr) {
    ssh_session session = (ssh_session)sessionPtr;

    int rc = ssh_userauth_none(session, nullptr);
    if (rc == SSH_AUTH_SUCCESS) return true;
    if (rc == SSH_AUTH_ERROR) {
        log("\x1b[31mAuthentication error: %s\x1b[0m\r\n", ssh_get_error(session));
        return false;
    }
    const int methods = ssh_userauth_list(session, nullptr);

    if (!cfg_.keyPath.empty() && (methods & SSH_AUTH_METHOD_PUBLICKEY)) {
        ssh_key pk = nullptr;
        if (ssh_pki_import_privkey_file(cfg_.keyPath.c_str(), nullptr, nullptr, nullptr, &pk) == SSH_OK) {
            rc = ssh_userauth_publickey(session, nullptr, pk);
            ssh_key_free(pk);
            if (rc == SSH_AUTH_SUCCESS) {
                log("Authenticated with private key.\r\n");
                return true;
            }
            log("Server did not accept the private key.\r\n");
        } else {
            log("Could not load the private key (passphrase-protected keys are not supported).\r\n");
        }
    }

    const bool canPassword = methods & SSH_AUTH_METHOD_PASSWORD;
    const bool canKbd = methods & SSH_AUTH_METHOD_INTERACTIVE;
    if (!canPassword && !canKbd) {
        log("\x1b[31mNo usable authentication method (server wants a key).\x1b[0m\r\n");
        return false;
    }

    for (int attempt = 0; attempt < 3 && !stop_; attempt++) {
        std::string pw;
        if (attempt == 0 && !cfg_.password.empty()) {
            pw = cfg_.password;
        } else if (!ask(Ask::Password, cfg_.user + "@" + cfg_.host + " password", &pw)) {
            log("Cancelled.\r\n");
            return false;
        }
        if (canPassword) {
            rc = ssh_userauth_password(session, nullptr, pw.c_str());
        } else {
            rc = ssh_userauth_kbdint(session, nullptr, nullptr);
            while (rc == SSH_AUTH_INFO) {
                const int n = ssh_userauth_kbdint_getnprompts(session);
                for (int i = 0; i < n; i++) ssh_userauth_kbdint_setanswer(session, i, pw.c_str());
                rc = ssh_userauth_kbdint(session, nullptr, nullptr);
            }
        }
        pw.assign(pw.size(), '\0');
        if (rc == SSH_AUTH_SUCCESS) return true;
        if (rc == SSH_AUTH_ERROR) {
            log("\x1b[31mAuthentication error: %s\x1b[0m\r\n", ssh_get_error(session));
            return false;
        }
        log("Permission denied, please try again.\r\n");
    }
    return false;
}

void SshSession::runInstallKey(void* sessionPtr, const std::string& line) {
    ssh_session session = (ssh_session)sessionPtr;
    // The key is base64 plus our own comment: no quotes, so single-quoting is safe.
    if (line.empty() || line.find('\'') != std::string::npos || line.find('\n') != std::string::npos) {
        log("\r\n\x1b[31m[No usable public key.]\x1b[0m\r\n");
        return;
    }
    const std::string cmd = "umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && "
                            "{ grep -qxF '" + line + "' ~/.ssh/authorized_keys && echo already-present || "
                            "{ echo '" + line + "' >> ~/.ssh/authorized_keys && echo added; }; }";
    ssh_channel ch = ssh_channel_new(session);
    if (!ch || ssh_channel_open_session(ch) != SSH_OK || ssh_channel_request_exec(ch, cmd.c_str()) != SSH_OK) {
        log("\r\n\x1b[31m[Could not run the key install command: %s]\x1b[0m\r\n", ssh_get_error(session));
        if (ch) ssh_channel_free(ch);
        return;
    }
    std::string out;
    char buf[256];
    const uint32_t t0 = millis();
    while (!stop_ && millis() - t0 < 15000) {
        int r = ssh_channel_read_timeout(ch, buf, sizeof buf, 0, 100);
        if (r > 0 && out.size() < 512) out.append(buf, r);
        r = ssh_channel_read_timeout(ch, buf, sizeof buf, 1, 0);  // drain stderr
        if (r > 0 && out.size() < 512) out.append(buf, r);
        if (ssh_channel_is_eof(ch)) break;
    }
    uint32_t code = 255;
    char* sig = nullptr;
    int core = 0;
    ssh_channel_get_exit_state(ch, &code, &sig, &core);
    if (sig) ssh_string_free_char(sig);
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
    if (code == 0 && out.find("already-present") != std::string::npos) {
        log("\r\n\x1b[32m[This device's key was already in ~/.ssh/authorized_keys.]\x1b[0m\r\n");
    } else if (code == 0 && out.find("added") != std::string::npos) {
        log("\r\n\x1b[32m[Key added to ~/.ssh/authorized_keys. Next time no password is needed.]\x1b[0m\r\n");
    } else {
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        log("\r\n\x1b[31m[Installing the key failed (exit %u): %s]\x1b[0m\r\n", (unsigned)code, out.c_str());
    }
}

void SshSession::run() {
    log("Connecting to %s@%s:%d ...\r\n", cfg_.user.c_str(), cfg_.host.c_str(), cfg_.port);

    ssh_session session = ssh_new();
    if (!session) {
        log("\x1b[31mOut of memory.\x1b[0m\r\n");
        return;
    }
    ssh_channel channel = nullptr;
    const char* closeReason = "Connection closed";

    do {
        unsigned int port = (unsigned int)cfg_.port;
        long timeout = 15;
        bool processConfig = false;
        ssh_options_set(session, SSH_OPTIONS_HOST, cfg_.host.c_str());
        ssh_options_set(session, SSH_OPTIONS_PORT, &port);
        ssh_options_set(session, SSH_OPTIONS_USER, cfg_.user.c_str());
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);

        if (ssh_connect(session) != SSH_OK) {
            log("\x1b[31mConnection failed: %s\x1b[0m\r\n", ssh_get_error(session));
            break;
        }
        if (stop_) break;

        // --- host key check (trust on first use; the UI keeps the known-hosts list) ---
        {
            ssh_key srv = nullptr;
            unsigned char* hash = nullptr;
            size_t hlen = 0;
            std::string fp;
            if (ssh_get_server_publickey(session, &srv) == SSH_OK) {
                if (ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == SSH_OK) {
                    char* s = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
                    if (s) {
                        fp = std::string(ssh_key_type_to_char(ssh_key_type(srv))) + " " + s;
                        ssh_string_free_char(s);
                    }
                    ssh_clean_pubkey_hash(&hash);
                }
                ssh_key_free(srv);
            }
            if (fp.empty()) {
                log("\x1b[31mCould not read the server's host key.\x1b[0m\r\n");
                break;
            }
            if (!ask(Ask::HostKey, fp, nullptr)) {
                log("Host key not accepted.\r\n");
                break;
            }
        }

        if (!authenticate(session)) break;
        if (stop_) break;

        channel = ssh_channel_new(session);
        if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
            log("\x1b[31mCould not open a channel: %s\x1b[0m\r\n", ssh_get_error(session));
            break;
        }
        resizePending_ = false;
        if (ssh_channel_request_pty_size(channel, "xterm-256color", cols_.load(), rows_.load()) != SSH_OK ||
            ssh_channel_request_shell(channel) != SSH_OK) {
            log("\x1b[31mCould not start a shell: %s\x1b[0m\r\n", ssh_get_error(session));
            break;
        }

        state_ = State::Open;
        char buf[1024];
        uint32_t lastActivity = millis();
        while (!stop_) {
            size_t n = xStreamBufferReceive(tx_, buf, sizeof buf, 0);
            if (n > 0) {
                if (ssh_channel_write(channel, buf, n) < 0) {
                    closeReason = "Connection lost";
                    break;
                }
                lastActivity = millis();
            }
            if (resizePending_.exchange(false)) ssh_channel_change_pty_size(channel, cols_.load(), rows_.load());
            if (installPending_.exchange(false)) {
                std::string line;
                {
                    std::lock_guard<std::mutex> lock(askMutex_);
                    line = installLine_;
                }
                runInstallKey(session, line);
                lastActivity = millis();
            }

            int r = ssh_channel_read_timeout(channel, buf, sizeof buf, 0, 10);
            if (r > 0) {
                lastActivity = millis();
                size_t off = 0;
                while (off < (size_t)r && !stop_) {
                    off += xStreamBufferSend(rx_, buf + off, r - off, pdMS_TO_TICKS(100));
                }
            } else if (r < 0 && r != SSH_AGAIN) {
                closeReason = ssh_channel_is_eof(channel) ? "Connection closed" : "Connection lost";
                break;
            }
            if (ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel)) break;

            if (millis() - lastActivity > kKeepAliveMs) {
                ssh_send_ignore(session, "");
                lastActivity = millis();
            }
        }
        if (stop_) closeReason = "Disconnected";
    } while (false);

    if (channel) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (ssh_is_connected(session)) ssh_disconnect(session);
    ssh_free(session);
    log("\r\n\x1b[0m\x1b[33m[%s. Press Enter.]\x1b[0m\r\n", closeReason);
}
