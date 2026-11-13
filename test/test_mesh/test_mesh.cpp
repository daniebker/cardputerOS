#include <unity.h>

#include <string>

#include "mesh.h"
#include "vectors.h"

using namespace mesh;

void setUp() {}
void tearDown() {}

#define BYTES(v) std::string(v, v##_len)

static FromRadio dec(const std::string& b) {
    FromRadio fr;
    TEST_ASSERT_TRUE(decodeFromRadio(b, fr));
    return fr;
}

void test_decode_reference_messages() {
    FromRadio fr = dec(BYTES(kMyInfo));
    TEST_ASSERT_EQUAL(FromRadio::MyInfo, fr.kind);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, fr.myNodeNum);

    fr = dec(BYTES(kNode));
    TEST_ASSERT_EQUAL(FromRadio::Node, fr.kind);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, fr.node.num);
    TEST_ASSERT_EQUAL_STRING("!a1b2c3d4", fr.node.id.c_str());
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 Base Station", fr.node.longName.c_str());
    TEST_ASSERT_EQUAL_STRING("CAFE", fr.node.shortName.c_str());
    TEST_ASSERT_EQUAL_FLOAT(-7.25f, fr.node.snr);
    TEST_ASSERT_EQUAL_UINT32(1731400000u, fr.node.lastHeard);
    TEST_ASSERT_EQUAL(87, fr.node.battery);
    TEST_ASSERT_EQUAL(2, fr.node.hopsAway);

    fr = dec(BYTES(kChannel));
    TEST_ASSERT_EQUAL(FromRadio::Channel, fr.kind);
    TEST_ASSERT_EQUAL(1, fr.channel.index);
    TEST_ASSERT_EQUAL_STRING("friends", fr.channel.name.c_str());
    TEST_ASSERT_EQUAL(2, fr.channel.role);
    fr = dec(BYTES(kChannel0));
    TEST_ASSERT_EQUAL(0, fr.channel.index);
    TEST_ASSERT_EQUAL(1, fr.channel.role);
    TEST_ASSERT_EQUAL_STRING("", fr.channel.name.c_str());

    fr = dec(BYTES(kComplete));
    TEST_ASSERT_EQUAL(FromRadio::ConfigComplete, fr.kind);
    TEST_ASSERT_EQUAL(42, fr.configId);

    fr = dec(BYTES(kTextBroadcast));
    TEST_ASSERT_EQUAL(FromRadio::PacketKind, fr.kind);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, fr.packet.from);
    TEST_ASSERT_EQUAL_HEX32(kBroadcast, fr.packet.to);
    TEST_ASSERT_EQUAL(1, fr.packet.channel);
    TEST_ASSERT_EQUAL_HEX32(0xdeadbeef, fr.packet.id);
    TEST_ASSERT_EQUAL_UINT32(1731400100u, fr.packet.rxTime);
    TEST_ASSERT_EQUAL_FLOAT(5.5f, fr.packet.snr);
    TEST_ASSERT_EQUAL(-97, fr.packet.rssi);
    TEST_ASSERT_EQUAL(2, fr.packet.hopLimit);
    TEST_ASSERT_EQUAL(3, fr.packet.hopStart);
    TEST_ASSERT_TRUE(fr.packet.decoded);
    TEST_ASSERT_EQUAL(PortText, fr.packet.port);
    TEST_ASSERT_EQUAL_STRING("hello mesh \xF0\x9F\x91\x8B", fr.packet.payload.c_str());

    fr = dec(BYTES(kEncrypted));
    TEST_ASSERT_EQUAL(FromRadio::PacketKind, fr.kind);
    TEST_ASSERT_FALSE(fr.packet.decoded);

    fr = dec(BYTES(kAck));
    TEST_ASSERT_EQUAL(PortRouting, fr.packet.port);
    TEST_ASSERT_EQUAL_HEX32(0x55aa55aa, fr.packet.requestId);
    TEST_ASSERT_EQUAL(0, decodeRoutingError(fr.packet.payload));
    fr = dec(BYTES(kNak));
    TEST_ASSERT_EQUAL(5, decodeRoutingError(fr.packet.payload));

    fr = dec(BYTES(kOther));
    TEST_ASSERT_EQUAL(FromRadio::Other, fr.kind);
}

void test_encode_matches_official_library() {
    TEST_ASSERT_TRUE(encodeWantConfig(42) == BYTES(kWantConfig));
    TEST_ASSERT_TRUE(encodeText(kBroadcast, 1, 0x55aa55aa, "hi all", true) == BYTES(kSendBroadcast));
    TEST_ASSERT_TRUE(encodeText(0xa1b2c3d4, 0, 0x01020304, "caf\xC3\xA9", true) == BYTES(kSendDirect));
    TEST_ASSERT_TRUE(encodeDisconnect() == BYTES(kDisconnect));
}

static State configured() {
    State s;
    s.beginConfig();
    for (auto& b : {BYTES(kMyInfo), BYTES(kNode), BYTES(kChannel0), BYTES(kChannel), BYTES(kComplete)}) s.apply(dec(b), 1000);
    return s;
}

void test_state_config_and_names() {
    State s = configured();
    TEST_ASSERT_TRUE(s.configured());
    TEST_ASSERT_EQUAL_HEX32(0x12345678, s.myNode());
    TEST_ASSERT_EQUAL(2, (int)s.channels().size());
    TEST_ASSERT_EQUAL(2, (int)s.conversations().size());  // one per usable channel
    TEST_ASSERT_EQUAL_STRING("# Primary channel", s.title(s.conversations()[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("# friends", s.title(s.conversations()[1]).c_str());
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 Base Station", s.nodeName(0xa1b2c3d4).c_str());
    TEST_ASSERT_EQUAL_STRING("CAFE", s.nodeName(0xa1b2c3d4, true).c_str());
    TEST_ASSERT_EQUAL_STRING("!00000042", s.nodeName(0x42).c_str());
    TEST_ASSERT_EQUAL_STRING("0042", s.nodeName(0x42, true).c_str());
}

void test_state_receiving() {
    State s = configured();
    TEST_ASSERT_TRUE(s.apply(dec(BYTES(kTextBroadcast)), 2000));
    TEST_ASSERT_FALSE(s.apply(dec(BYTES(kTextBroadcast)), 2000));  // duplicate delivery
    TEST_ASSERT_TRUE(s.apply(dec(BYTES(kTextDirect)), 2000));
    TEST_ASSERT_FALSE(s.apply(dec(BYTES(kEncrypted)), 2000));
    TEST_ASSERT_EQUAL(3, (int)s.conversations().size());
    const Conversation& ch = s.conversations()[s.conversation(false, 1)];
    TEST_ASSERT_EQUAL(1, (int)ch.messages.size());
    TEST_ASSERT_EQUAL(1, ch.unread);
    TEST_ASSERT_FALSE(ch.messages[0].mine);
    TEST_ASSERT_EQUAL_UINT32(1731400100u, ch.messages[0].time);
    const Conversation& dm = s.conversations()[s.conversation(true, 0xa1b2c3d4)];
    TEST_ASSERT_EQUAL_STRING("direct hi", dm.messages[0].text.c_str());
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 Base Station", s.title(dm).c_str());
    TEST_ASSERT_EQUAL(2, s.totalUnread());
    // a node-info packet teaches us a new node's name
    s.apply(dec(BYTES(kNodeInfoPacket)), 2000);
    TEST_ASSERT_EQUAL_STRING("New Node", s.nodeName(0x0badf00d).c_str());
    TEST_ASSERT_EQUAL_UINT32(2000u, s.nodes().at(0x0badf00d).lastHeard);
    // hops are worked out from the packet
    TEST_ASSERT_EQUAL(1, s.nodes().at(0xa1b2c3d4).hopsAway);
}

void test_state_sending_and_acks() {
    State s = configured();
    const int ci = s.conversation(false, 1);
    std::string wire = s.sendText(ci, "hi all", 0x55aa55aa, 3000);
    TEST_ASSERT_TRUE(wire == BYTES(kSendBroadcast));
    const Message& m = s.conversations()[ci].messages.back();
    TEST_ASSERT_TRUE(m.mine);
    TEST_ASSERT_TRUE(m.status == Status::Sending);
    TEST_ASSERT_EQUAL(0, s.conversations()[ci].unread);
    s.apply(dec(BYTES(kAck)), 3001);
    TEST_ASSERT_TRUE(s.conversations()[ci].messages.back().status == Status::Delivered);
    s.apply(dec(BYTES(kNak)), 3002);  // a late failure report does not undo a delivery
    TEST_ASSERT_TRUE(s.conversations()[ci].messages.back().status == Status::Delivered);

    const int di = s.conversation(true, 0xa1b2c3d4);
    wire = s.sendText(di, "caf\xC3\xA9", 0x55aa55aa + 1, 3003);
    FromRadio nak = dec(BYTES(kNak));
    nak.packet.requestId = 0x55aa55aa + 1;
    s.apply(nak, 3004);
    TEST_ASSERT_TRUE(s.conversations()[di].messages.back().status == Status::Failed);
    TEST_ASSERT_TRUE(s.sendText(99, "x", 1, 1).empty());
    TEST_ASSERT_TRUE(s.sendText(di, "", 1, 1).empty());
}

void test_state_limits_and_reconnect() {
    State s = configured();
    FromRadio fr = dec(BYTES(kTextBroadcast));
    for (uint32_t i = 0; i < 100; i++) {
        fr.packet.id = 1000 + i;
        s.apply(fr, 5000 + i);
    }
    const int ci = s.conversation(false, 1);
    TEST_ASSERT_EQUAL((int)State::kMaxMessages, (int)s.conversations()[ci].messages.size());
    TEST_ASSERT_EQUAL_UINT32(1099u, s.conversations()[ci].messages.back().id);
    // many direct conversations: the list stays bounded, channels survive
    FromRadio dm = dec(BYTES(kTextDirect));
    for (uint32_t i = 0; i < 60; i++) {
        dm.packet.from = 0x5000 + i;
        dm.packet.id = 7000 + i;
        dm.packet.rxTime = 9000 + i;
        s.apply(dm, 0);
    }
    TEST_ASSERT_TRUE(s.conversations().size() <= State::kMaxConversations);
    TEST_ASSERT_FALSE(s.conversations()[s.conversation(false, 0)].direct);
    TEST_ASSERT_FALSE(s.conversations()[s.conversation(false, 1)].direct);
    // reconnecting forgets nodes and channels but keeps the messages
    s.beginConfig();
    TEST_ASSERT_FALSE(s.configured());
    TEST_ASSERT_EQUAL(0, (int)s.nodes().size());
    TEST_ASSERT_EQUAL((int)State::kMaxMessages, (int)s.conversations()[s.conversation(false, 1)].messages.size());
}

void test_save_and_load() {
    State s = configured();
    s.apply(dec(BYTES(kTextBroadcast)), 2000);
    s.apply(dec(BYTES(kTextDirect)), 2000);
    const int ci = s.conversation(false, 1);
    s.sendText(ci, "tabs\tand\nnewlines \\ ok", 77, 2500);
    TEST_ASSERT_TRUE(s.takeDirty());
    TEST_ASSERT_FALSE(s.takeDirty());
    const std::string saved = s.serialize();
    State t;
    TEST_ASSERT_TRUE(t.deserialize(saved));
    TEST_ASSERT_EQUAL((int)s.conversations().size(), (int)t.conversations().size());
    const Conversation& c = t.conversations()[t.conversation(false, 1)];
    TEST_ASSERT_EQUAL(2, (int)c.messages.size());
    TEST_ASSERT_EQUAL_STRING("tabs\tand\nnewlines \\ ok", c.messages[1].text.c_str());
    TEST_ASSERT_TRUE(c.messages[1].mine);
    TEST_ASSERT_TRUE(c.messages[1].status == Status::Failed);  // was still "sending" when saved
    TEST_ASSERT_EQUAL(1, c.unread);
    TEST_ASSERT_EQUAL_STRING("hello mesh \xF0\x9F\x91\x8B", c.messages[0].text.c_str());
    State bad;
    TEST_ASSERT_FALSE(bad.deserialize(""));
    TEST_ASSERT_FALSE(bad.deserialize("something else\n"));
    TEST_ASSERT_FALSE(bad.deserialize("mesh1\nM\t1\t2\t3\t0\t0\torphan\n"));
}

void test_malformed_input_is_rejected_safely() {
    // truncations of every reference message
    for (auto& b : {BYTES(kMyInfo), BYTES(kNode), BYTES(kChannel), BYTES(kTextBroadcast), BYTES(kNodeInfoPacket), BYTES(kAck)}) {
        for (size_t n = 0; n < b.size(); n++) {
            FromRadio fr;
            decodeFromRadio(b.substr(0, n), fr);  // must not crash; result may be either
        }
    }
    // random corruption and random bytes, applied to a live state
    State s = configured();
    uint32_t r = 99;
    const std::string base = BYTES(kTextBroadcast) + BYTES(kNode);
    for (int i = 0; i < 200000; i++) {
        std::string b = (i & 1) ? BYTES(kTextBroadcast) : BYTES(kNode);
        r = r * 1664525u + 1013904223u;
        const int flips = 1 + (r >> 28) % 3;
        for (int k = 0; k < flips; k++) {
            r = r * 1664525u + 1013904223u;
            b[(r >> 8) % b.size()] = (char)(r >> 24);
        }
        if ((r & 7) == 0) b.resize((r >> 12) % b.size());
        FromRadio fr;
        if (decodeFromRadio(b, fr)) s.apply(fr, i);
    }
    TEST_ASSERT_TRUE(s.conversations().size() <= State::kMaxConversations);
    for (auto& c : s.conversations()) TEST_ASSERT_TRUE(c.messages.size() <= State::kMaxMessages);
    TEST_ASSERT_TRUE(s.nodes().size() <= State::kMaxNodes);
    State t;
    TEST_ASSERT_TRUE(t.deserialize(s.serialize()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_decode_reference_messages);
    RUN_TEST(test_encode_matches_official_library);
    RUN_TEST(test_state_config_and_names);
    RUN_TEST(test_state_receiving);
    RUN_TEST(test_state_sending_and_acks);
    RUN_TEST(test_state_limits_and_reconnect);
    RUN_TEST(test_save_and_load);
    RUN_TEST(test_malformed_input_is_rejected_safely);
    return UNITY_END();
}
