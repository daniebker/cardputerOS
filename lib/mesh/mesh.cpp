#include "mesh.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mesh {

// ---------------------------------------------------------------- protobuf wire format

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    explicit Reader(const std::string& s) : p((const uint8_t*)s.data()), end((const uint8_t*)s.data() + s.size()) {}
    Reader(const uint8_t* b, size_t n) : p(b), end(b + n) {}
    bool more() const { return ok && p < end; }

    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) {
                ok = false;
                return 0;
            }
            const uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    uint32_t fixed32() {
        if (end - p < 4) {
            ok = false;
            return 0;
        }
        uint32_t v;
        memcpy(&v, p, 4);  // little endian, as on every platform we run on
        p += 4;
        return v;
    }
    // Reads the next field header. Returns false at the end or on error.
    bool next(int& field, int& wire) {
        if (!more()) return false;
        const uint64_t tag = varint();
        if (!ok) return false;
        field = (int)(tag >> 3);
        wire = (int)(tag & 7);
        return field != 0;
    }
    // For wire type 2: the bytes of the field.
    bool bytes(const uint8_t*& b, size_t& n) {
        const uint64_t len = varint();
        if (!ok || len > (uint64_t)(end - p)) {
            ok = false;
            return false;
        }
        b = p;
        n = (size_t)len;
        p += len;
        return true;
    }
    std::string str() {
        const uint8_t* b;
        size_t n;
        return bytes(b, n) ? std::string((const char*)b, n) : std::string();
    }
    void skip(int wire) {
        const uint8_t* b;
        size_t n;
        switch (wire) {
            case 0: varint(); break;
            case 1: if (end - p < 8) ok = false; else p += 8; break;
            case 2: bytes(b, n); break;
            case 5: fixed32(); break;
            default: ok = false; break;  // groups are not used
        }
    }
};

float asFloat(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

struct Writer {
    std::string out;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            out.push_back((char)(v | 0x80));
            v >>= 7;
        }
        out.push_back((char)v);
    }
    void tag(int field, int wire) { varint(((uint64_t)field << 3) | wire); }
    void uint(int field, uint64_t v) {
        tag(field, 0);
        varint(v);
    }
    void fixed32(int field, uint32_t v) {
        tag(field, 5);
        out.append((const char*)&v, 4);
    }
    void bytes(int field, const std::string& b) {
        tag(field, 2);
        varint(b.size());
        out += b;
    }
};

bool decodeData(const uint8_t* b, size_t n, Packet& p) {
    Reader r(b, n);
    int f, w;
    while (r.next(f, w)) {
        if (f == 1 && w == 0) p.port = (int)r.varint();
        else if (f == 2 && w == 2) p.payload = r.str();
        else if (f == 6 && w == 5) p.requestId = r.fixed32();
        else r.skip(w);
    }
    return r.ok;
}

bool decodePacket(const uint8_t* b, size_t n, Packet& p) {
    Reader r(b, n);
    int f, w;
    while (r.next(f, w)) {
        const uint8_t* sub;
        size_t len;
        if (f == 1 && w == 5) p.from = r.fixed32();
        else if (f == 2 && w == 5) p.to = r.fixed32();
        else if (f == 3 && w == 0) p.channel = (int)r.varint();
        else if (f == 4 && w == 2) {
            if (!r.bytes(sub, len) || !decodeData(sub, len, p)) return false;
            p.decoded = true;
        } else if (f == 6 && w == 5) p.id = r.fixed32();
        else if (f == 7 && w == 5) p.rxTime = r.fixed32();
        else if (f == 8 && w == 5) p.snr = asFloat(r.fixed32());
        else if (f == 9 && w == 0) p.hopLimit = (int)r.varint();
        else if (f == 10 && w == 0) p.wantAck = r.varint() != 0;
        else if (f == 12 && w == 0) p.rssi = (int)(int32_t)r.varint();
        else if (f == 14 && w == 0) p.viaMqtt = r.varint() != 0;
        else if (f == 15 && w == 0) p.hopStart = (int)r.varint();
        else r.skip(w);
    }
    return r.ok;
}

bool decodeUserInto(const uint8_t* b, size_t n, NodeInfo& node) {
    Reader r(b, n);
    int f, w;
    while (r.next(f, w)) {
        if (f == 1 && w == 2) node.id = r.str();
        else if (f == 2 && w == 2) node.longName = r.str();
        else if (f == 3 && w == 2) node.shortName = r.str();
        else r.skip(w);
    }
    return r.ok;
}

bool decodeNodeInfo(const uint8_t* b, size_t n, NodeInfo& node) {
    Reader r(b, n);
    int f, w;
    while (r.next(f, w)) {
        const uint8_t* sub;
        size_t len;
        if (f == 1 && w == 0) node.num = (uint32_t)r.varint();
        else if (f == 2 && w == 2) {
            if (!r.bytes(sub, len) || !decodeUserInto(sub, len, node)) return false;
        } else if (f == 4 && w == 5) node.snr = asFloat(r.fixed32());
        else if (f == 5 && w == 5) node.lastHeard = r.fixed32();
        else if (f == 6 && w == 2) {  // DeviceMetrics
            if (!r.bytes(sub, len)) return false;
            Reader m(sub, len);
            int mf, mw;
            while (m.next(mf, mw)) {
                if (mf == 1 && mw == 0) node.battery = (int)m.varint();
                else m.skip(mw);
            }
            if (!m.ok) return false;
        } else if (f == 9 && w == 0) node.hopsAway = (int)r.varint();
        else r.skip(w);
    }
    return r.ok;
}

bool decodeChannel(const uint8_t* b, size_t n, ChannelInfo& ch) {
    Reader r(b, n);
    int f, w;
    while (r.next(f, w)) {
        const uint8_t* sub;
        size_t len;
        if (f == 1 && w == 0) ch.index = (int)(int32_t)r.varint();
        else if (f == 2 && w == 2) {  // ChannelSettings
            if (!r.bytes(sub, len)) return false;
            Reader s(sub, len);
            int sf, sw;
            while (s.next(sf, sw)) {
                if (sf == 3 && sw == 2) ch.name = s.str();
                else s.skip(sw);
            }
            if (!s.ok) return false;
        } else if (f == 3 && w == 0) ch.role = (int)r.varint();
        else r.skip(w);
    }
    return r.ok;
}

}  // namespace

bool decodeFromRadio(const std::string& bytes, FromRadio& out) {
    out = FromRadio();
    Reader r(bytes);
    int f, w;
    while (r.next(f, w)) {
        const uint8_t* sub;
        size_t len;
        if (f == 2 && w == 2) {
            if (!r.bytes(sub, len) || !decodePacket(sub, len, out.packet)) return false;
            out.kind = FromRadio::PacketKind;
        } else if (f == 3 && w == 2) {  // MyNodeInfo
            if (!r.bytes(sub, len)) return false;
            Reader m(sub, len);
            int mf, mw;
            while (m.next(mf, mw)) {
                if (mf == 1 && mw == 0) out.myNodeNum = (uint32_t)m.varint();
                else m.skip(mw);
            }
            if (!m.ok) return false;
            out.kind = FromRadio::MyInfo;
        } else if (f == 4 && w == 2) {
            if (!r.bytes(sub, len) || !decodeNodeInfo(sub, len, out.node)) return false;
            out.kind = FromRadio::Node;
        } else if (f == 7 && w == 0) {
            out.configId = (uint32_t)r.varint();
            out.kind = FromRadio::ConfigComplete;
        } else if (f == 8 && w == 0) {
            if (r.varint()) out.kind = FromRadio::Rebooted;
        } else if (f == 10 && w == 2) {
            if (!r.bytes(sub, len) || !decodeChannel(sub, len, out.channel)) return false;
            out.kind = FromRadio::Channel;
        } else {
            r.skip(w);
        }
    }
    return r.ok;
}

bool decodeUser(const std::string& bytes, NodeInfo& out) {
    return decodeUserInto((const uint8_t*)bytes.data(), bytes.size(), out);
}

int decodeRoutingError(const std::string& bytes) {
    Reader r(bytes);
    int f, w, err = -1;
    while (r.next(f, w)) {
        if (f == 3 && w == 0) err = (int)r.varint();
        else r.skip(w);
    }
    // an empty Routing message is an ack too (error_reason NONE is the default value 0)
    if (r.ok && err < 0 && bytes.empty()) err = 0;
    return r.ok ? err : -1;
}

std::string encodeWantConfig(uint32_t configId) {
    Writer w;
    w.uint(3, configId);
    return w.out;
}

std::string encodeDisconnect() {
    Writer w;
    w.uint(4, 1);
    return w.out;
}

std::string encodeText(uint32_t to, int channel, uint32_t packetId, const std::string& text, bool wantAck, int hopLimit) {
    Writer data;
    data.uint(1, PortText);
    data.bytes(2, text);
    Writer pkt;
    pkt.fixed32(2, to);
    if (channel) pkt.uint(3, (uint64_t)channel);
    pkt.bytes(4, data.out);
    pkt.fixed32(6, packetId);
    if (hopLimit) pkt.uint(9, (uint64_t)hopLimit);
    if (wantAck) pkt.uint(10, 1);
    Writer w;
    w.bytes(1, pkt.out);
    return w.out;
}

// ---------------------------------------------------------------- state

void State::beginConfig() {
    configured_ = false;
    myNode_ = 0;
    nodes_.clear();
    channels_.clear();
}

// Keeps the node list bounded on busy meshes: drops the node not heard for longest.
void State::trimNodes() {
    while (nodes_.size() >= kMaxNodes) {
        auto oldest = nodes_.begin();
        for (auto it = nodes_.begin(); it != nodes_.end(); ++it)
            if (it->second.lastHeard < oldest->second.lastHeard) oldest = it;
        nodes_.erase(oldest);
    }
}

int State::conversation(bool direct, uint32_t key) {
    for (size_t i = 0; i < convs_.size(); i++)
        if (convs_[i].direct == direct && convs_[i].key == key) return (int)i;
    if (convs_.size() >= kMaxConversations) {
        // drop the direct conversation that has been quiet the longest
        int oldest = -1;
        for (size_t i = 0; i < convs_.size(); i++)
            if (convs_[i].direct && (oldest < 0 || convs_[i].lastTime < convs_[oldest].lastTime)) oldest = (int)i;
        if (oldest < 0) oldest = 0;
        convs_.erase(convs_.begin() + oldest);
    }
    Conversation c;
    c.direct = direct;
    c.key = key;
    convs_.push_back(c);
    dirty_ = true;
    return (int)convs_.size() - 1;
}

void State::addMessage(int ci, const Message& m, bool countUnread) {
    Conversation& c = convs_[ci];
    c.messages.push_back(m);
    if (c.messages.size() > kMaxMessages) c.messages.erase(c.messages.begin());
    if (m.time > c.lastTime) c.lastTime = m.time;
    else c.lastTime++;  // keeps "most recent first" working without a clock
    if (countUnread) c.unread++;
    dirty_ = true;
}

bool State::apply(const FromRadio& fr, uint32_t now) {
    switch (fr.kind) {
        case FromRadio::MyInfo:
            myNode_ = fr.myNodeNum;
            return false;
        case FromRadio::Node:
            if (fr.node.num) {
                trimNodes();
                nodes_[fr.node.num] = fr.node;
            }
            return false;
        case FromRadio::Channel: {
            for (auto& c : channels_) {
                if (c.index == fr.channel.index) {
                    c = fr.channel;
                    return false;
                }
            }
            channels_.push_back(fr.channel);
            std::sort(channels_.begin(), channels_.end(),
                      [](const ChannelInfo& a, const ChannelInfo& b) { return a.index < b.index; });
            return false;
        }
        case FromRadio::ConfigComplete:
            configured_ = true;
            // every usable channel gets a conversation, so it can be written to
            for (auto& c : channels_)
                if (c.role != 0) conversation(false, (uint32_t)c.index);
            return false;
        case FromRadio::PacketKind:
            break;
        default:
            return false;
    }

    const Packet& p = fr.packet;
    if (p.from && p.from != myNode_) {
        trimNodes();
        NodeInfo& n = nodes_[p.from];  // every packet tells us the sender is alive
        n.num = p.from;
        if (p.rxTime || now) n.lastHeard = p.rxTime ? p.rxTime : now;
        if (p.snr != 0) n.snr = p.snr;
        if (p.hopStart >= p.hopLimit && p.hopStart > 0) n.hopsAway = p.hopStart - p.hopLimit;
    }
    if (!p.decoded) return false;

    if (p.port == PortNodeInfo) {
        NodeInfo named;
        if (p.from && decodeUser(p.payload, named)) {
            NodeInfo& n = nodes_[p.from];
            n.num = p.from;
            if (!named.id.empty()) n.id = named.id;
            if (!named.longName.empty()) n.longName = named.longName;
            if (!named.shortName.empty()) n.shortName = named.shortName;
        }
        return false;
    }
    if (p.port == PortRouting && p.requestId) {
        // delivery report for something we sent
        const int err = decodeRoutingError(p.payload);
        for (auto& c : convs_) {
            for (auto& m : c.messages) {
                if (m.mine && m.id == p.requestId && m.status != Status::Delivered) {
                    m.status = err == 0 ? Status::Delivered : (err > 0 ? Status::Failed : m.status);
                    dirty_ = true;
                }
            }
        }
        return false;
    }
    if (p.port != PortText || p.payload.empty()) return false;

    Message m;
    m.id = p.id;
    m.from = p.from;
    m.time = p.rxTime ? p.rxTime : now;
    m.text = p.payload;
    int ci;
    if (p.to == kBroadcast) ci = conversation(false, (uint32_t)p.channel);
    else if (p.to == myNode_ && myNode_) ci = conversation(true, p.from);
    else return false;  // addressed to someone else
    // the node may hand us the same packet twice (e.g. after a reconnect)
    for (auto& old : convs_[ci].messages)
        if (old.id == m.id && old.from == m.from && m.id) return false;
    addMessage(ci, m, true);
    return true;
}

std::string State::nodeName(uint32_t num, bool shortName) const {
    auto it = nodes_.find(num);
    if (it != nodes_.end()) {
        const std::string& n = shortName ? it->second.shortName : it->second.longName;
        if (!n.empty()) return n;
        if (!shortName && !it->second.shortName.empty()) return it->second.shortName;
    }
    char buf[16];
    if (shortName) snprintf(buf, sizeof buf, "%04x", (unsigned)(num & 0xFFFF));
    else snprintf(buf, sizeof buf, "!%08x", (unsigned)num);
    return buf;
}

std::string State::channelName(int index) const {
    for (auto& c : channels_)
        if (c.index == index && !c.name.empty()) return c.name;
    return index == 0 ? "Primary channel" : "Channel " + std::to_string(index);
}

std::string State::title(const Conversation& c) const {
    return c.direct ? nodeName(c.key) : "# " + channelName((int)c.key);
}

int State::totalUnread() const {
    int n = 0;
    for (auto& c : convs_) n += c.unread;
    return n;
}

std::string State::sendText(int ci, const std::string& text, uint32_t packetId, uint32_t now) {
    if (ci < 0 || ci >= (int)convs_.size() || text.empty()) return std::string();
    const Conversation c = convs_[ci];
    Message m;
    m.id = packetId;
    m.from = myNode_;
    m.time = now;
    m.text = text;
    m.mine = true;
    m.status = Status::Sending;
    addMessage(ci, m, false);
    return encodeText(c.direct ? c.key : kBroadcast, c.direct ? 0 : (int)c.key, packetId, text, true);
}

// Saved form: one line per conversation ("C") followed by its messages ("M"),
// tab separated; tabs, newlines and backslashes in text are escaped.
namespace {
std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c != '\r') o.push_back(c);
    }
    return o;
}
std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o.push_back(n == 'n' ? '\n' : (n == 't' ? '\t' : n));
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t e = line.find('\t', pos);
        out.push_back(line.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return out;
}
}  // namespace

std::string State::serialize() const {
    std::string s = "mesh1\n";
    for (auto& c : convs_) {
        s += "C\t" + std::to_string((int)c.direct) + "\t" + std::to_string(c.key) + "\t" + std::to_string(c.unread) + "\t" +
             std::to_string(c.lastTime) + "\n";
        for (auto& m : c.messages) {
            // a message still "sending" when we stopped will never get its report
            const Status st = m.status == Status::Sending ? Status::Failed : m.status;
            s += "M\t" + std::to_string(m.id) + "\t" + std::to_string(m.from) + "\t" + std::to_string(m.time) + "\t" +
                 std::to_string((int)st) + "\t" + std::to_string((int)m.mine) + "\t" + escape(m.text) + "\n";
        }
    }
    return s;
}

bool State::deserialize(const std::string& data) {
    std::vector<Conversation> convs;
    size_t pos = 0;
    bool first = true;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        const std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        if (first) {
            if (line != "mesh1") return false;
            first = false;
            continue;
        }
        if (line.empty()) continue;
        const auto f = split(line);
        if (f[0] == "C" && f.size() == 5) {
            if (convs.size() >= kMaxConversations) return false;
            Conversation c;
            c.direct = f[1] == "1";
            c.key = (uint32_t)strtoul(f[2].c_str(), nullptr, 10);
            c.unread = atoi(f[3].c_str());
            c.lastTime = (uint32_t)strtoul(f[4].c_str(), nullptr, 10);
            if (c.unread < 0) c.unread = 0;
            convs.push_back(c);
        } else if (f[0] == "M" && f.size() == 7 && !convs.empty()) {
            if (convs.back().messages.size() >= kMaxMessages) continue;
            Message m;
            m.id = (uint32_t)strtoul(f[1].c_str(), nullptr, 10);
            m.from = (uint32_t)strtoul(f[2].c_str(), nullptr, 10);
            m.time = (uint32_t)strtoul(f[3].c_str(), nullptr, 10);
            const int st = atoi(f[4].c_str());
            m.status = (st >= 0 && st <= 3) ? (Status)st : Status::Received;
            m.mine = f[5] == "1";
            m.text = unescape(f[6]);
            convs.back().messages.push_back(m);
        } else {
            return false;
        }
    }
    if (first) return false;
    convs_.swap(convs);
    dirty_ = false;
    return true;
}

}  // namespace mesh
