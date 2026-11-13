// SSH client app: a list of saved hosts and a terminal session.
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../../hal/sdcard.h"
#include "../../os/kernel.h"
#include "../../os/icons.h"
#include "../../os/net.h"
#include "../../os/secrets.h"
#include "../../os/storage.h"
#include "../../os/term_view.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "ssh_keys.h"
#include "ssh_session.h"
#include "vterm.h"

namespace {

constexpr int kScrollbackLines = 100;
constexpr const char* kSdKeyPath = "/cardputerOS/id_ed25519.pub";
constexpr size_t kMaxKnownHosts = 40;
const char* const kKeyFiles[] = {"/ssh/id_ed25519", "/ssh/id_ecdsa", "/ssh/id_rsa"};

struct Host {
    std::string name, host, user, pass;
    int port = 22;
    std::string title() const { return name.empty() ? host : name; }
    std::string id() const { return host + ":" + std::to_string(port); }
};

class SshApp : public os::App {
public:
    ~SshApp() override { closeSession(); }

    void onStart() override { loadHosts(); }

    void onResume() override {
        if (screen_ == Screen::Hosts) drawHosts();
        else if (screen_ == Screen::Key) drawKey();
        else drawTerminal();
    }

    void onKey(const key::Event& ev) override {
        if (screen_ == Screen::Hosts) hostsKey(ev);
        else if (screen_ == Screen::Key) keyScreenKey(ev);
        else terminalKey(ev);
    }

    void onTick() override {
        if (screen_ != Screen::Terminal) return;
        pumpOutput();
        handleAsk();
        const uint32_t now = millis();
        if (term_ && term_->anyDirty() && now - lastRender_ >= 16) {
            lastRender_ = now;
            view_.render(os::gfx(), *term_, termRect_, false);
        }
    }

    void onBackgroundTick() override {
        if (screen_ == Screen::Terminal) pumpOutput();
    }

    // sleeping switches Wi-Fi off, which would drop the connection
    bool keepAwake() override { return session_ && !session_->finished(); }

    const char* statusText() override {
        if (screen_ != Screen::Terminal || !term_) return nullptr;
        status_ = label_;
        if (session_ && session_->finished()) status_ += " (closed)";
        if (term_->viewOffset() > 0) status_ += "  [scroll -" + std::to_string(term_->viewOffset()) + "]";
        return status_.c_str();
    }

private:
    enum class Screen { Hosts, Terminal, Key };

    // ------------------------------------------------------------ storage
    void loadHosts() {
        hosts_.clear();
        for (auto& line : storage::splitLines(secrets::get("ssh_hosts"))) {
            auto f = storage::splitFields(line);
            if (f.size() < 4 || f[1].empty()) continue;
            Host h;
            h.name = f[0];
            h.host = f[1];
            h.port = atoi(f[2].c_str());
            if (h.port <= 0 || h.port > 65535) h.port = 22;
            h.user = f[3];
            if (f.size() > 4) h.pass = f[4];
            hosts_.push_back(h);
        }
    }

    void saveHosts() {
        std::string text;
        for (auto& h : hosts_) {
            text += h.name + "\t" + h.host + "\t" + std::to_string(h.port) + "\t" + h.user + "\t" + h.pass + "\n";
        }
        secrets::put("ssh_hosts", text);
    }

    // known hosts: one "host:port<TAB>keytype fingerprint" line each
    static std::string knownKey(const std::string& id) {
        for (auto& line : storage::splitLines(storage::getStr("ssh_known"))) {
            auto f = storage::splitFields(line);
            if (f.size() == 2 && f[0] == id) return f[1];
        }
        return std::string();
    }

    static void rememberKey(const std::string& id, const std::string& fp) {
        auto lines = storage::splitLines(storage::getStr("ssh_known"));
        std::string text;
        size_t skip = lines.size() >= kMaxKnownHosts ? lines.size() - kMaxKnownHosts + 1 : 0;
        for (size_t i = skip; i < lines.size(); i++) {
            auto f = storage::splitFields(lines[i]);
            if (f.size() == 2 && f[0] != id) text += lines[i] + "\n";
        }
        text += id + "\t" + fp + "\n";
        storage::putStr("ssh_known", text);
    }

    // ------------------------------------------------------------ host list
    void drawHosts() {
        os::Gfx& g = os::gfx();
        os::Rect r = ui::footer(g, os::content(), "Enter: connect  n: new  e: edit  d: delete  k: key");
        listRect_ = ui::header(g, r, "SSH hosts");
        list_.items.clear();
        for (auto& h : hosts_) {
            list_.items.push_back({h.title(), h.user + "@" + h.host + (h.port != 22 ? ":" + std::to_string(h.port) : ""), 0});
        }
        list_.items.push_back({"+ New host", "", theme::accent});
        list_.draw(g, listRect_);
    }

    static bool notBlank(const std::string& s) { return s.find_first_not_of(' ') != std::string::npos; }

    // Runs the edit dialogs. Returns false if the user cancelled.
    bool editHost(Host& h) {
        Host e = h;
        std::string port = std::to_string(e.port);
        if (!ui::prompt("Host name or IP address", e.host) || !notBlank(e.host)) return false;
        if (!ui::prompt("Port", port)) return false;
        if (!ui::prompt("User name", e.user) || !notBlank(e.user)) return false;
        if (!ui::prompt("Password (optional)", e.pass, true,
                        "Stored unencrypted. Leave empty to be asked each time."))
            return false;
        if (!ui::prompt("Display name (optional)", e.name)) return false;
        e.port = atoi(port.c_str());
        if (e.port <= 0 || e.port > 65535) e.port = 22;
        // tabs and newlines are the storage separators
        for (std::string* s : {&e.name, &e.host, &e.user, &e.pass})
            for (char& c : *s)
                if (c == '\t' || c == '\n') c = ' ';
        h = e;
        return true;
    }

    void hostsKey(const key::Event& ev) {
        os::Gfx& g = os::gfx();
        const bool onHost = list_.selected < (int)hosts_.size();
        if (ev.is('n') || (ev.is(key::Enter) && !onHost)) {
            Host h;
            if (editHost(h)) {
                hosts_.push_back(h);
                saveHosts();
                list_.selected = (int)hosts_.size() - 1;
            }
            drawHosts();
        } else if (ev.is('e') && onHost) {
            if (editHost(hosts_[list_.selected])) saveHosts();
            drawHosts();
        } else if (ev.is('d') && onHost) {
            if (ui::confirm("Delete host", "Delete \"" + hosts_[list_.selected].title() + "\"?")) {
                hosts_.erase(hosts_.begin() + list_.selected);
                saveHosts();
            }
            drawHosts();
        } else if (ev.is('k')) {
            showKey();
        } else if (ev.is(key::Enter) && onHost) {
            connect(hosts_[list_.selected]);
        } else {
            list_.onKey(g, listRect_, ev);
        }
    }

    // ------------------------------------------------------------ device key
    void showKey() {
        if (!sshkeys::exists()) {
            if (!ui::confirm("SSH key", "This device has no SSH key yet. Create one now?", true)) {
                drawHosts();
                return;
            }
            createKey();
        }
        copyKeyToSd();
        screen_ = Screen::Key;
        drawKey();
    }

    // Puts the public key on the SD card (if one is inserted) for easy copying.
    void copyKeyToSd() {
        const std::string pub = sshkeys::publicKey();
        if (pub.empty()) {
            sdStatus_.clear();
            return;
        }
        ui::busy("Checking for an SD card...");
        switch (hal::sdWriteFile(kSdKeyPath, pub + "\n")) {
            case hal::SdResult::Ok:
                sdStatus_ = std::string("Copied to SD card: ") + kSdKeyPath;
                sdOk_ = true;
                break;
            case hal::SdResult::NoCard:
                sdStatus_ = "No SD card. Insert one and press s to copy the key.";
                sdOk_ = false;
                break;
            default:
                sdStatus_ = "Could not write to the SD card (s: retry).";
                sdOk_ = false;
                break;
        }
    }

    void createKey() {
        ui::busy("Creating key...");
        std::string err;
        if (!sshkeys::generate(err)) ui::alert("SSH key", err);
    }

    void drawKey() {
        os::Gfx& g = os::gfx();
        os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        os::Rect r = ui::footer(g, c, "g: new key   s: copy to SD card   Enter: back");
        r = ui::header(g, r, "This device's SSH key");
        const std::string pub = sshkeys::publicKey();
        if (pub.empty()) {
            g.setFont(theme::fontUi());
            g.setTextColor(theme::dim);
            g.setTextDatum(textdatum_t::top_left);
            g.drawString("No key. Press g to create one.", r.x + 8, r.y + 6);
            return;
        }
        // copy-and-paste friendly copy on the USB serial console
        Serial.printf("\n---- cardputerOS public key ----\n%s\n--------------------------------\n", pub.c_str());

        g.setTextDatum(textdatum_t::top_left);
        int y = r.y + 6;
        g.setFont(theme::fontSmall());
        g.setTextColor(theme::dim);
        g.drawString("Public key (add to ~/.ssh/authorized_keys):", r.x + 8, y);
        y += g.fontHeight() + 4;
        // monospace, hard-wrapped
        g.setFont(&fonts::Font0);
        g.setTextColor(theme::text);
        const int perLine = (r.w - 16) / 6;
        for (size_t i = 0; i < pub.size(); i += perLine) {
            g.drawString(pub.substr(i, perLine).c_str(), r.x + 8, y);
            y += 10;
        }
        y += 6;
        g.setFont(theme::fontSmall());
        g.setTextColor(theme::dim);
        g.drawString("Fingerprint:", r.x + 8, y);
        y += g.fontHeight() + 2;
        g.setFont(&fonts::Font0);
        g.setTextColor(theme::text);
        ui::drawClipped(g, sshkeys::fingerprint(), r.x + 8, y, r.w - 16);
        y += 16;
        g.setFont(theme::fontSmall());
        g.setTextColor(theme::dim);
        const char* help[] = {"Easiest: log in to a host with your password, then",
                              "press opt+k in the terminal to install the key there."};
        for (const char* h : help) {
            ui::drawClipped(g, h, r.x + 8, y, r.w - 16);
            y += g.fontHeight() + 2;
        }
        y += 4;
        g.setTextColor(sdOk_ ? theme::ok : theme::warn);
        ui::drawClipped(g, sdStatus_, r.x + 8, y, r.w - 16);
    }

    void keyScreenKey(const key::Event& ev) {
        if (ev.is('g')) {
            const bool had = sshkeys::exists();
            if (!had || ui::confirm("New SSH key", "Replace the current key? Hosts that trust the old key will "
                                                   "ask for a password again until you install the new one.")) {
                createKey();
                copyKeyToSd();
            }
            drawKey();
        } else if (ev.is('s')) {
            copyKeyToSd();
            drawKey();
        } else if (ev.is(key::Enter) || ev.code == key::Esc || ev.code == key::Backspace) {
            screen_ = Screen::Hosts;
            os::Rect c = os::content();
            os::gfx().fillRect(c.x, c.y, c.w, c.h, theme::bg);
            drawHosts();
        }
    }

    void installKeyOnHost() {
        if (!session_ || session_->state() != SshSession::State::Open) return;
        if (!sshkeys::exists()) {
            if (ui::confirm("SSH key", "This device has no SSH key yet. Create one now?", true)) createKey();
            if (!sshkeys::exists()) {
                drawTerminal();
                return;
            }
        }
        if (ui::confirm("Install key", "Add this device's public key to ~/.ssh/authorized_keys on " + label_ +
                                           "? You will then be able to log in without a password.",
                        true)) {
            session_->installKey(sshkeys::publicKey());
        }
        drawTerminal();
    }

    // ------------------------------------------------------------ terminal
    void layoutTerminal() {
        os::Rect c = os::content();
        const int cols = view_.colsFor(c), rows = view_.rowsFor(c);
        termRect_ = {c.x + (c.w - cols * view_.cellW()) / 2, c.y, cols * view_.cellW(), rows * view_.cellH()};
        cols_ = cols;
        rows_ = rows;
    }

    void drawTerminal() {
        os::Gfx& g = os::gfx();
        os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::termBg);
        if (term_) view_.render(g, *term_, termRect_, true);
    }

    void connect(const Host& h) {
        if (!net::connected()) {
            ui::alert("No network", "Wi-Fi is not connected. Open Settings > Wi-Fi from the launcher first.");
            drawHosts();
            return;
        }
        closeSession();
        view_.setFont(os::TermView::savedFont());
        layoutTerminal();
        term_.reset(new VTerm(cols_, rows_, kScrollbackLines));

        SshSession::Config cfg;
        cfg.host = h.host;
        cfg.port = h.port;
        cfg.user = h.user;
        cfg.password = h.pass;
        cfg.cols = cols_;
        cfg.rows = rows_;
        for (const char* k : kKeyFiles) {
            if (secrets::readFile(k, cfg.keyText)) break;
        }
        hostId_ = h.id();
        label_ = h.user + "@" + h.host;

        session_ = SshSession::create();
        std::weak_ptr<SshSession> weak = session_;
        term_->onReply([weak](const char* d, size_t n) {
            if (auto s = weak.lock()) s->send(d, n);
        });
        screen_ = Screen::Terminal;
        if (!session_->start(cfg)) {
            term_->write("\x1b[31mCould not start the SSH task (out of memory?).\x1b[0m\r\n[Press Enter.]\r\n");
        }
        cfg.keyText.assign(cfg.keyText.size(), '\0');  // the session has its own copy
        os::refreshStatus();
        drawTerminal();
    }

    void closeSession() {
        if (session_) {
            session_->stop();
            session_.reset();  // the connection task holds its own reference until it exits
        }
        term_.reset();
    }

    void pumpOutput() {
        if (!session_ || !term_) return;
        char buf[512];
        // bounded so a flood of output cannot starve the keyboard
        for (int i = 0; i < 8; i++) {
            size_t n = session_->read(buf, sizeof buf);
            if (n == 0) break;
            term_->write(buf, n);
        }
    }

    void handleAsk() {
        if (!session_) return;
        SshSession::Ask kind;
        std::string text;
        if (!session_->pendingAsk(kind, text)) return;
        auto session = session_;  // keep alive across the dialog
        if (kind == SshSession::Ask::HostKey) {
            const std::string known = knownKey(hostId_);
            if (known == text) {
                session->answer(true);
                return;
            }
            bool ok;
            if (known.empty()) {
                ok = ui::confirm("Unknown host", "First connection to " + hostId_ + ".\nHost key fingerprint:\n" + text +
                                                     "\nTrust this host?");
            } else {
                ok = ui::confirm("WARNING: HOST KEY CHANGED",
                                 "The key of " + hostId_ +
                                     " is different from last time. Someone could be intercepting the connection.\nNew: " +
                                     text + "\nConnect anyway?");
            }
            if (ok) rememberKey(hostId_, text);
            session->answer(ok);
        } else if (kind == SshSession::Ask::Password) {
            std::string pw;
            bool ok = ui::prompt(text.c_str(), pw, true);
            session->answer(ok, pw);
            pw.assign(pw.size(), '\0');
        }
        if (screen_ == Screen::Terminal) drawTerminal();
    }

    void toggleFont() {
        // cycle Small -> Normal -> Medium -> Large -> Small
        const auto next = (os::TermView::Font)((view_.font() + 1) % os::TermView::kFontCount);
        os::TermView::saveFont(next);
        view_.setFont(next);
        layoutTerminal();
        if (term_) term_->resize(cols_, rows_);
        if (session_) session_->resize(cols_, rows_);
        drawTerminal();
    }

    void terminalKey(const key::Event& ev) {
        if (!term_) return;
        const bool closed = !session_ || session_->finished();

        if (ev.opt()) {
            const int page = std::max(1, rows_ / 2);
            if (ev.code == key::Up || ev.code == ';' || ev.code == key::PageUp) term_->scrollView(page);
            else if (ev.code == key::Down || ev.code == '.' || ev.code == key::PageDown) term_->scrollView(-page);
            else if (ev.code == 'f') toggleFont();
            else if (ev.code == 'k' && !closed) installKeyOnHost();
            else if (ev.code == 'x' && !closed) {
                if (ui::confirm("Disconnect", "Close the connection to " + label_ + "?")) session_->stop();
                drawTerminal();
            }
            os::refreshStatus();
            return;
        }
        if (closed) {
            if (ev.is(key::Enter) || ev.code == key::Esc) {
                pumpOutput();
                closeSession();
                screen_ = Screen::Hosts;
                os::refreshStatus();
                os::Rect c = os::content();
                os::gfx().fillRect(c.x, c.y, c.w, c.h, theme::bg);
                drawHosts();
            }
            return;
        }
        std::string bytes = term_->encodeKey(ev);
        if (bytes.empty()) return;
        if (term_->viewOffset() != 0) {
            term_->setViewOffset(0);
            os::refreshStatus();
        }
        session_->send(bytes.data(), bytes.size());
    }

    Screen screen_ = Screen::Hosts;
    std::vector<Host> hosts_;
    ui::List list_;
    os::Rect listRect_{0, 0, 0, 0};

    std::shared_ptr<SshSession> session_;
    std::unique_ptr<VTerm> term_;
    os::TermView view_;
    os::Rect termRect_{0, 0, 0, 0};
    int cols_ = 53, rows_ = 28;
    uint32_t lastRender_ = 0;
    std::string hostId_, label_, status_;
    std::string sdStatus_;
    bool sdOk_ = false;
};

OS_REGISTER_APP(SshApp, "SSH", "Remote terminal", theme::dracula::green, icons::terminal, 10);

}  // namespace
