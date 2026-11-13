// Mesh: a Meshtastic text messenger. Talks to a Meshtastic node over Bluetooth LE
// (the same way the phone apps do); the node does the LoRa part.
#include <esp_random.h>

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "../../os/icons.h"
#include "../../os/kernel.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "mesh.h"
#include "mesh_link.h"

namespace {

namespace dr = theme::dracula;
constexpr const char* kMessagesPath = "/mesh.msgs";
constexpr int kCw = 8, kLineH = 16;  // chat text: 8x16 fixed-width font

uint32_t nowEpoch() {
    const time_t t = time(nullptr);
    return t > 1700000000 ? (uint32_t)t : 0;
}

using ui::displayable;

std::string ago(uint32_t then) {
    const uint32_t now = nowEpoch();
    if (!now || !then || then > now) return std::string();
    const uint32_t s = now - then;
    if (s < 90) return "now";
    if (s < 3600) return std::to_string(s / 60) + "m";
    if (s < 86400) return std::to_string(s / 3600) + "h";
    return std::to_string(s / 86400) + "d";
}

class MeshApp : public os::App {
public:
    void onStart() override {
        std::string saved;
        if (storage::readFile(kMessagesPath, saved)) state_.deserialize(saved);
        auto f = storage::splitFields(storage::getStr("mesh_dev"));
        if (f.size() == 3 && !f[1].empty()) {
            device_.name = f[0];
            device_.address = f[1];
            device_.addressType = (uint8_t)atoi(f[2].c_str());
            haveDevice_ = autoConnect_ = true;
        }
        if (!link_.begin()) error_ = "Bluetooth could not be started (not enough memory?).";
    }

    ~MeshApp() override {
        saveMessages(true);
        link_.end();
    }

    void onResume() override { redraw(); }
    void onPause() override { saveMessages(true); }

    // Bluetooth does not survive sleep, and a messenger should keep listening.
    bool keepAwake() override { return link_.state() != MeshLink::State::Off; }

    const char* statusText() override {
        status_ = linkText();
        return status_.c_str();
    }

    const char* notice() override {
        const int n = state_.totalUnread();
        if (n == 0) return nullptr;
        notice_ = "Mesh: " + std::to_string(n) + (n == 1 ? " new message" : " new messages");
        return notice_.c_str();
    }

    void onBackgroundTick() override { pump(false); }

    void onTick() override {
        if (link_.pinRequested()) {
            std::string pin;
            const bool ok = ui::prompt("Pairing PIN", pin, false, "Shown on the node's screen (or 123456 if it has none)");
            link_.providePin(ok, (uint32_t)strtoul(pin.c_str(), nullptr, 10));
            redraw();
        }
        if (pump(true)) redraw();
    }

    void onKey(const key::Event& ev) override {
        switch (screen_) {
            case Screen::Chats: chatsKey(ev); break;
            case Screen::Chat: chatKey(ev); break;
            case Screen::Nodes: nodesKey(ev); break;
        }
    }

private:
    enum class Screen { Chats, Chat, Nodes };

    // ------------------------------------------------------------ link handling
    std::string linkText() const {
        switch (link_.state()) {
            case MeshLink::State::Connected: return (syncing_ ? "syncing with " : "") + device_.name;
            case MeshLink::State::Connecting: return "connecting to " + device_.name + "...";
            case MeshLink::State::Scanning: return "scanning...";
            case MeshLink::State::Off: return "Bluetooth off";
            default: return haveDevice_ && autoConnect_ ? "waiting for " + device_.name : "not connected";
        }
    }

    // Runs the link: returns true if something visible changed.
    bool pump(bool foreground) {
        bool changed = false;
        const MeshLink::State st = link_.state();
        if (st != lastState_) {
            lastState_ = st;
            changed = true;
            os::refreshStatus();
        }
        const bool connected = st == MeshLink::State::Connected;
        if (connected && !wasConnected_) {
            wasConnected_ = true;
            syncing_ = true;
            state_.beginConfig();
            link_.send(mesh::encodeWantConfig(esp_random() | 1));
        } else if (!connected && wasConnected_) {
            wasConnected_ = false;
            syncing_ = false;
            nextRetry_ = millis() + 10000;
        }

        std::string err = link_.takeError();
        if (!err.empty()) {
            error_ = err;
            changed = true;
            if (foreground && userAction_) {
                userAction_ = false;
                ui::alert("Mesh", err);
                if (err.find("Pairing") != std::string::npos &&
                    ui::confirm("Mesh", "Forget the stored pairing with " + device_.name + " and pair again?")) {
                    link_.forget(device_);
                    startConnect(true);
                }
            }
        }

        std::string frame;
        for (int i = 0; i < 40 && link_.receive(frame); i++) {
            mesh::FromRadio fr;
            if (!mesh::decodeFromRadio(frame, fr)) continue;
            const bool newMessage = state_.apply(fr, nowEpoch());
            if (fr.kind == mesh::FromRadio::ConfigComplete) {
                syncing_ = false;
                error_.clear();
                changed = true;
                os::refreshStatus();
            }
            if (fr.kind == mesh::FromRadio::PacketKind) changed = changed || newMessage || fr.packet.port == mesh::PortRouting;
        }
        // reading the open conversation marks it read
        if (screen_ == Screen::Chat && foreground) {
            mesh::Conversation& c = state_.conversations()[state_.conversation(chatDirect_, chatKey_)];
            if (c.unread) {
                c.unread = 0;
                changed = true;
            }
        }

        if (st == MeshLink::State::Idle && haveDevice_ && autoConnect_ && (int32_t)(millis() - nextRetry_) >= 0) {
            startConnect(false);
        }
        saveMessages(false);
        return changed;
    }

    void startConnect(bool byUser) {
        userAction_ = byUser;
        nextRetry_ = millis() + 25000;
        error_.clear();
        link_.connect(device_);
    }

    void saveMessages(bool now) {
        if (!now && millis() - lastSave_ < 5000) return;
        lastSave_ = millis();
        if (state_.takeDirty()) storage::writeFile(kMessagesPath, state_.serialize());
    }

    // 'c': connect / disconnect / pick a node
    void connectMenu() {
        if (link_.state() == MeshLink::State::Connected || link_.state() == MeshLink::State::Connecting) {
            if (ui::confirm("Mesh", "Disconnect from " + device_.name + "?")) {
                autoConnect_ = false;
                link_.disconnect();
            }
            redraw();
            return;
        }
        if (link_.state() != MeshLink::State::Idle) {
            redraw();
            return;
        }
        std::vector<std::string> options;
        if (haveDevice_) options.push_back("Connect to " + device_.name);
        options.push_back("Scan for Meshtastic nodes");
        const int pick = ui::choose("Mesh connection", options);
        if (pick < 0) {
            redraw();
            return;
        }
        if (haveDevice_ && pick == 0) {
            autoConnect_ = true;
            startConnect(true);
            redraw();
            return;
        }
        link_.startScan();
        ui::busy("Scanning for Meshtastic nodes...");
        const uint32_t t0 = millis();
        while (link_.state() == MeshLink::State::Scanning && millis() - t0 < 9000) os::idle();
        auto found = link_.scanResults();
        if (found.empty()) {
            ui::alert("Mesh", "No Meshtastic node found. Make sure it is switched on, nearby, has Bluetooth enabled "
                              "and is not connected to a phone.");
            redraw();
            return;
        }
        std::sort(found.begin(), found.end(), [](const MeshLink::Device& a, const MeshLink::Device& b) { return a.rssi > b.rssi; });
        std::vector<std::string> names;
        for (auto& d : found) names.push_back(d.name + "   (" + std::to_string(d.rssi) + " dBm)");
        const int which = ui::choose("Choose a node", names);
        if (which >= 0) {
            device_ = found[which];
            haveDevice_ = autoConnect_ = true;
            storage::putStr("mesh_dev", device_.name + "\t" + device_.address + "\t" + std::to_string(device_.addressType));
            startConnect(true);
        }
        redraw();
    }

    // ------------------------------------------------------------ screens
    void redraw() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        if (screen_ == Screen::Chats) drawChats();
        else if (screen_ == Screen::Chat) drawChat();
        else drawNodes();
    }

    void drawChats() {
        os::Gfx& g = os::gfx();
        os::Rect r = ui::footer(g, os::content(), "Enter: open   n: nodes   c: connect / disconnect");
        std::string title = "Mesh - " + linkText();
        if (!error_.empty() && link_.state() == MeshLink::State::Idle) title = "Mesh - " + error_;
        listRect_ = ui::header(g, r, title.c_str());

        // most recent first; remember what was selected
        const auto& convs = state_.conversations();
        std::pair<bool, uint32_t> keep{false, 0xFFFFFFFE};
        if (list_.selected < (int)order_.size() && order_[list_.selected] < (int)convs.size())
            keep = {convs[order_[list_.selected]].direct, convs[order_[list_.selected]].key};
        order_.clear();
        for (int i = 0; i < (int)convs.size(); i++) order_.push_back(i);
        std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) { return convs[a].lastTime > convs[b].lastTime; });
        list_.items.clear();
        list_.emptyText = link_.state() == MeshLink::State::Connected ? "Loading channels..."
                                                                        : "Press c to connect to a Meshtastic node.";
        for (size_t i = 0; i < order_.size(); i++) {
            const mesh::Conversation& cv = convs[order_[i]];
            ui::ListItem it;
            it.label = displayable(state_.title(cv));
            if (cv.unread) {
                it.detail = std::to_string(cv.unread) + " new";
                it.color = dr::yellow;
            } else if (!cv.messages.empty()) {
                std::string snippet = displayable(cv.messages.back().text);
                if (snippet.size() > 22) snippet = snippet.substr(0, 20) + "..";
                it.detail = snippet;
            }
            list_.items.push_back(it);
            if (cv.direct == keep.first && cv.key == keep.second) list_.selected = (int)i;
        }
        list_.draw(g, listRect_);
    }

    void chatsKey(const key::Event& ev) {
        if (ev.is('c')) {
            connectMenu();
        } else if (ev.is('n')) {
            screen_ = Screen::Nodes;
            nodeList_.selected = 0;
            redraw();
        } else if (list_.onKey(os::gfx(), listRect_, ev) == ui::List::Activated && list_.selected < (int)order_.size()) {
            const mesh::Conversation& cv = state_.conversations()[order_[list_.selected]];
            openChat(cv.direct, cv.key);
        }
    }

    void openChat(bool direct, uint32_t key) {
        chatDirect_ = direct;
        chatKey_ = key;
        state_.conversations()[state_.conversation(direct, key)].unread = 0;
        input_.clear();
        scroll_ = 0;
        hint_.clear();
        screen_ = Screen::Chat;
        redraw();
    }

    struct Line {
        std::string text;
        int nameLen = 0;       // leading characters drawn in the name colour
        uint32_t nameColor = 0, textColor = 0;
        char mark = 0;         // delivery mark after the last line of an own message
        uint32_t markColor = 0;
    };

    void drawChat() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        const mesh::Conversation& cv = state_.conversations()[state_.conversation(chatDirect_, chatKey_)];
        const os::Rect body = ui::header(g, c, displayable(state_.title(cv) + "   " + linkText()).c_str());
        const int inputH = 20;
        const int cols = (c.w - 8) / kCw;
        const int rows = (body.h - inputH - 2) / kLineH;

        // wrap every message into lines
        std::vector<Line> lines;
        for (auto& m : cv.messages) {
            const std::string name = m.mine ? "me" : displayable(state_.nodeName(m.from, true));
            std::string text = name + ": " + displayable(m.text);
            bool first = true;
            while (!text.empty()) {
                const int width = first ? cols : cols - 2;
                size_t cut = text.size();
                if ((int)cut > width) {
                    cut = text.rfind(' ', width);
                    if (cut == std::string::npos || cut < (size_t)width / 2) cut = width;
                }
                Line l;
                l.text = (first ? "" : "  ") + text.substr(0, cut);
                l.nameLen = first ? (int)name.size() + 1 : 0;
                l.nameColor = m.mine ? dr::green : dr::cyan;
                l.textColor = theme::text;
                lines.push_back(l);
                text.erase(0, cut);
                while (!text.empty() && text[0] == ' ') text.erase(0, 1);
                first = false;
            }
            if (m.mine && !lines.empty()) {
                Line& last = lines.back();
                if (m.status == mesh::Status::Sending) { last.mark = '~'; last.markColor = theme::dim; }
                else if (m.status == mesh::Status::Delivered) { last.mark = '+'; last.markColor = dr::green; }
                else if (m.status == mesh::Status::Failed) { last.mark = '!'; last.markColor = dr::red; }
            }
        }
        const int maxScroll = std::max(0, (int)lines.size() - rows);
        scroll_ = std::max(0, std::min(scroll_, maxScroll));
        const int firstLine = std::max(0, (int)lines.size() - rows - scroll_);

        g.startWrite();
        g.setFont(&fonts::AsciiFont8x16);
        g.setTextDatum(textdatum_t::top_left);
        if (lines.empty()) {
            g.setTextColor(theme::dim);
            g.drawString("No messages yet. Type and press Enter.", body.x + 4, body.y + 4);
        }
        for (int i = 0; i < rows && firstLine + i < (int)lines.size(); i++) {
            const Line& l = lines[firstLine + i];
            const int y = body.y + 2 + i * kLineH;
            if (l.nameLen) {
                g.setTextColor(l.nameColor);
                g.drawString(l.text.substr(0, l.nameLen).c_str(), body.x + 4, y);
            }
            g.setTextColor(l.textColor);
            g.drawString(l.text.substr(l.nameLen).c_str(), body.x + 4 + l.nameLen * kCw, y);
            if (l.mark) {
                const char s[2] = {l.mark, 0};
                g.setTextColor(l.markColor);
                g.drawString(s, body.x + 4 + ((int)l.text.size() + 1) * kCw, y);
            }
        }
        if (scroll_ > 0) g.fillTriangle(c.x + c.w - 10, body.y + body.h - inputH - 12, c.x + c.w - 4, body.y + body.h - inputH - 12,
                                        c.x + c.w - 7, body.y + body.h - inputH - 6, theme::dim);
        g.endWrite();
        drawInput();
    }

    void drawInput() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        const int inputH = 20, y = c.y + c.h - inputH;
        g.startWrite();
        g.fillRect(c.x, y, c.w, inputH, theme::panel);
        g.setFont(&fonts::AsciiFont8x16);
        g.setTextDatum(textdatum_t::top_left);
        const int cols = (c.w - 8) / kCw - 9;  // room for the counter
        if (!hint_.empty() && input_.empty()) {
            g.setTextColor(theme::warn);
            g.drawString(hint_.c_str(), c.x + 4, y + 2);
        } else {
            std::string shown = "> " + input_ + "_";
            if ((int)shown.size() > cols) shown = shown.substr(shown.size() - cols);
            g.setTextColor(theme::text);
            g.drawString(shown.c_str(), c.x + 4, y + 2);
            g.setFont(theme::fontSmall());
            g.setTextColor(theme::dim);
            const std::string count = std::to_string(input_.size()) + "/" + std::to_string(mesh::kMaxTextBytes);
            g.drawString(count.c_str(), c.x + c.w - g.textWidth(count.c_str()) - 4, y + 5);
        }
        g.endWrite();
    }

    void chatKey(const key::Event& ev) {
        if (ev.code == key::Esc) {
            screen_ = Screen::Chats;
            redraw();
            return;
        }
        const int page = 6;
        if (ev.code == key::Up || ev.code == key::PageUp || ev.isOpt(';')) {
            scroll_ += ev.code == key::Up ? 1 : page;
            drawChatBody();
            return;
        }
        if (ev.code == key::Down || ev.code == key::PageDown || ev.isOpt('.')) {
            scroll_ -= ev.code == key::Down ? 1 : page;
            drawChatBody();
            return;
        }
        if (ev.is(key::Enter)) {
            if (input_.empty()) return;
            if (link_.state() != MeshLink::State::Connected || syncing_) {
                hint_ = "Not connected - message kept";
                const std::string keep = input_;
                input_.clear();
                drawInput();
                input_ = keep;
                return;
            }
            const int ci = state_.conversation(chatDirect_, chatKey_);
            link_.send(state_.sendText(ci, input_, esp_random() | 1, nowEpoch()));
            input_.clear();
            hint_.clear();
            scroll_ = 0;
            redraw();
            return;
        }
        if (ev.code == key::Backspace) {
            if (ev.ctrl()) input_.clear();
            else if (!input_.empty()) input_.pop_back();
        } else if (ev.isChar() && (int)input_.size() < mesh::kMaxTextBytes) {
            input_.push_back((char)ev.code);
        } else {
            return;
        }
        hint_.clear();
        drawInput();
    }

    void drawChatBody() { redraw(); }

    // ---- nodes ----
    void drawNodes() {
        os::Gfx& g = os::gfx();
        os::Rect r = ui::footer(g, os::content(), "Enter: direct message   Backspace: back");
        nodes_.clear();
        for (auto& kv : state_.nodes())
            if (kv.first != state_.myNode()) nodes_.push_back(kv.second);
        std::sort(nodes_.begin(), nodes_.end(), [](const mesh::NodeInfo& a, const mesh::NodeInfo& b) { return a.lastHeard > b.lastHeard; });
        nodeRect_ = ui::header(g, r, ("Nodes (" + std::to_string(nodes_.size()) + ")").c_str());
        nodeList_.items.clear();
        nodeList_.emptyText = "No other nodes known yet.";
        for (auto& n : nodes_) {
            ui::ListItem it;
            it.label = displayable(state_.nodeName(n.num));
            if (!n.shortName.empty()) it.label += "  (" + displayable(n.shortName) + ")";
            std::string d;
            if (n.hopsAway == 0) d = "direct";
            else if (n.hopsAway > 0) d = std::to_string(n.hopsAway) + (n.hopsAway == 1 ? " hop" : " hops");
            const std::string a = ago(n.lastHeard);
            if (!a.empty()) d += (d.empty() ? "" : "  ") + a;
            if (n.battery >= 0 && n.battery <= 100) d += (d.empty() ? "" : "  ") + std::to_string(n.battery) + "%";
            it.detail = d;
            nodeList_.items.push_back(it);
        }
        nodeList_.draw(g, nodeRect_);
    }

    void nodesKey(const key::Event& ev) {
        if (ev.code == key::Backspace || ev.code == key::Esc) {
            screen_ = Screen::Chats;
            redraw();
        } else if (nodeList_.onKey(os::gfx(), nodeRect_, ev) == ui::List::Activated && nodeList_.selected < (int)nodes_.size()) {
            openChat(true, nodes_[nodeList_.selected].num);
        }
    }

    Screen screen_ = Screen::Chats;
    MeshLink link_;
    mesh::State state_;
    MeshLink::Device device_;
    bool haveDevice_ = false, autoConnect_ = false, userAction_ = false;
    bool wasConnected_ = false, syncing_ = false;
    MeshLink::State lastState_ = MeshLink::State::Off;
    uint32_t nextRetry_ = 0, lastSave_ = 0;
    std::string error_, status_, notice_;

    ui::List list_, nodeList_;
    os::Rect listRect_{0, 0, 0, 0}, nodeRect_{0, 0, 0, 0};
    std::vector<int> order_;
    std::vector<mesh::NodeInfo> nodes_;

    bool chatDirect_ = false;
    uint32_t chatKey_ = 0;
    std::string input_, hint_;
    int scroll_ = 0;
};

OS_REGISTER_APP(MeshApp, "Mesh", "Meshtastic messenger (Bluetooth)", theme::dracula::purple, icons::mesh, 25);

}  // namespace
