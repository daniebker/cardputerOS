// Meshtastic client protocol (the phone API a node offers over Bluetooth) and the
// client-side state: nodes, channels, conversations. Portable, no platform code.
//
// Only what a text-messaging client needs is implemented; unknown fields and
// message types are skipped.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mesh {

constexpr uint32_t kBroadcast = 0xFFFFFFFFu;
constexpr int kMaxTextBytes = 200;  // Meshtastic payload limit is a little above this

enum Port { PortText = 1, PortPosition = 3, PortNodeInfo = 4, PortRouting = 5, PortTelemetry = 67 };

struct Packet {
    uint32_t from = 0, to = 0, id = 0, rxTime = 0, requestId = 0;
    int channel = 0;
    float snr = 0;
    int rssi = 0;
    int hopStart = 0, hopLimit = 0;
    bool decoded = false;  // false: still encrypted (a channel this node has no key for)
    bool wantAck = false, viaMqtt = false;
    int port = 0;
    std::string payload;
};

struct NodeInfo {
    uint32_t num = 0;
    std::string id, longName, shortName;
    float snr = 0;
    uint32_t lastHeard = 0;
    int battery = -1;   // percent, -1 unknown (values above 100 mean "powered")
    int hopsAway = -1;  // -1 unknown
};

struct ChannelInfo {
    int index = 0;
    std::string name;
    int role = 0;  // 0 disabled, 1 primary, 2 secondary
};

struct FromRadio {
    enum Kind { Other, PacketKind, MyInfo, Node, Channel, ConfigComplete, Rebooted } kind = Other;
    Packet packet;
    NodeInfo node;
    ChannelInfo channel;
    uint32_t myNodeNum = 0;
    uint32_t configId = 0;
};

// Decodes one FromRadio protobuf. Returns false if the bytes are malformed.
bool decodeFromRadio(const std::string& bytes, FromRadio& out);
// Decodes a User protobuf (the payload of a node-info packet) into name fields.
bool decodeUser(const std::string& bytes, NodeInfo& out);
// Routing payload: the error code (0 = delivered / acknowledged), -1 if not present.
int decodeRoutingError(const std::string& bytes);

// ToRadio messages.
std::string encodeWantConfig(uint32_t configId);
std::string encodeText(uint32_t to, int channel, uint32_t packetId, const std::string& text, bool wantAck,
                       int hopLimit = 3);
std::string encodeDisconnect();

// ---------------------------------------------------------------- client state

enum class Status : uint8_t { Received, Sending, Delivered, Failed };

struct Message {
    uint32_t id = 0, from = 0, time = 0;
    std::string text;
    Status status = Status::Received;
    bool mine = false;
};

struct Conversation {
    bool direct = false;  // true: with one node (key = node number); false: a channel (key = index)
    uint32_t key = 0;
    std::vector<Message> messages;
    int unread = 0;
    uint32_t lastTime = 0;
};

class State {
public:
    static constexpr size_t kMaxMessages = 60;       // per conversation
    static constexpr size_t kMaxConversations = 24;
    static constexpr size_t kMaxNodes = 120;

    // Forget what the node told us (before a fresh download); messages are kept.
    void beginConfig();
    // Applies one decoded message from the node. `now` is our clock (epoch seconds,
    // 0 if unknown). Returns true if it added a text message.
    bool apply(const FromRadio& fr, uint32_t now);

    bool configured() const { return configured_; }
    uint32_t myNode() const { return myNode_; }
    const std::map<uint32_t, NodeInfo>& nodes() const { return nodes_; }
    const std::vector<ChannelInfo>& channels() const { return channels_; }
    std::vector<Conversation>& conversations() { return convs_; }
    const std::vector<Conversation>& conversations() const { return convs_; }

    // Finds or creates a conversation; index stays valid until the next call that adds one.
    int conversation(bool direct, uint32_t key);
    std::string title(const Conversation& c) const;
    std::string nodeName(uint32_t num, bool shortName = false) const;
    std::string channelName(int index) const;
    int totalUnread() const;

    // Records an outgoing text in the conversation and returns the ToRadio bytes to send.
    std::string sendText(int convIndex, const std::string& text, uint32_t packetId, uint32_t now);

    // Conversations and messages as text, for saving between sessions.
    std::string serialize() const;
    bool deserialize(const std::string& data);
    // True if messages changed since the last call (for deciding when to save).
    bool takeDirty() {
        bool d = dirty_;
        dirty_ = false;
        return d;
    }

private:
    void addMessage(int convIndex, const Message& m, bool countUnread);
    void trimNodes();

    bool configured_ = false;
    uint32_t myNode_ = 0;
    std::map<uint32_t, NodeInfo> nodes_;
    std::vector<ChannelInfo> channels_;
    std::vector<Conversation> convs_;
    bool dirty_ = false;
};

}  // namespace mesh
