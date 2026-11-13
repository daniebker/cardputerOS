#include <unity.h>

#include "wardrive.h"

using namespace wardrive;

void setUp() {}
void tearDown() {}

static Device wifi(uint8_t last, const char* ssid, int8_t rssi, uint8_t ch = 6, uint8_t auth = AuthWpa2) {
    Device d;
    d.kind = Kind::Wifi;
    d.mac = {0x10, 0x20, 0x30, 0x40, 0x50, last};
    d.name = ssid;
    d.rssi = rssi;
    d.channel = ch;
    d.auth = auth;
    return d;
}

static Device ble(uint8_t last, const char* name, int8_t rssi, uint16_t company = kNoCompany) {
    Device d;
    d.kind = Kind::Ble;
    d.mac = {0xc0, 0xff, 0xee, 0x00, 0x00, last};
    d.name = name;
    d.rssi = rssi;
    d.company = company;
    d.addrType = 1;
    return d;
}

void test_observe_new_and_update() {
    Table t;
    bool isNew = false;
    int i = t.observe(wifi(1, "home", -70), 1000, 1700000000, &isNew);
    TEST_ASSERT_TRUE(isNew);
    TEST_ASSERT_EQUAL(1, t.count(Kind::Wifi));
    TEST_ASSERT_EQUAL(0, t.count(Kind::Ble));
    TEST_ASSERT_EQUAL(-70, t.at(i).best);

    int j = t.observe(wifi(1, "home", -55, 11), 5000, 1700000004, &isNew);
    TEST_ASSERT_FALSE(isNew);
    TEST_ASSERT_EQUAL(i, j);
    const Device& d = t.at(i);
    TEST_ASSERT_EQUAL(-55, d.rssi);
    TEST_ASSERT_EQUAL(-55, d.best);
    TEST_ASSERT_EQUAL(11, d.channel);
    TEST_ASSERT_EQUAL(1000, d.firstMs);
    TEST_ASSERT_EQUAL(5000, d.lastMs);
    TEST_ASSERT_EQUAL(1700000000, d.firstTime);
    TEST_ASSERT_EQUAL(2, d.sightings);

    t.observe(wifi(1, "home", -80), 6000, 0);
    TEST_ASSERT_EQUAL(-80, t.at(i).rssi);
    TEST_ASSERT_EQUAL(-55, t.at(i).best);  // best is kept

    // same address, other kind: a separate device
    t.observe(ble(1, "", -60), 6000, 0, &isNew);
    TEST_ASSERT_TRUE(isNew);
    TEST_ASSERT_EQUAL(1, t.count(Kind::Ble));
}

void test_ble_keeps_name_and_company() {
    Table t;
    int i = t.observe(ble(2, "Watch", -60, 0x004C), 100, 0);
    // an advertisement without the name / manufacturer data does not erase them
    t.observe(ble(2, "", -62), 200, 0);
    TEST_ASSERT_EQUAL_STRING("Watch", t.at(i).name.c_str());
    TEST_ASSERT_EQUAL_HEX16(0x004C, t.at(i).company);
    // the clock became valid later: first sighting time gets filled in once
    t.observe(ble(2, "", -62), 300, 1700000100);
    TEST_ASSERT_EQUAL(1700000100, t.at(i).firstTime);
    t.observe(ble(2, "", -62), 400, 1700000200);
    TEST_ASSERT_EQUAL(1700000100, t.at(i).firstTime);
}

void test_capacity_drops_least_recent() {
    Table t(3);
    t.observe(wifi(1, "a", -50), 100, 0);
    t.observe(wifi(2, "b", -50), 200, 0);
    t.observe(wifi(3, "c", -50), 300, 0);
    t.observe(wifi(1, "a", -50), 400, 0);   // a seen again: b is now the oldest
    t.observe(ble(9, "x", -50), 450, 0);    // other kind does not count
    t.observe(wifi(4, "d", -50), 500, 0);
    TEST_ASSERT_EQUAL(3, t.count(Kind::Wifi));
    TEST_ASSERT_EQUAL(4, (int)t.total(Kind::Wifi));
    TEST_ASSERT_TRUE(t.find(Kind::Wifi, wifi(2, "", 0).mac) < 0);
    TEST_ASSERT_TRUE(t.find(Kind::Wifi, wifi(1, "", 0).mac) >= 0);
    TEST_ASSERT_TRUE(t.find(Kind::Wifi, wifi(4, "", 0).mac) >= 0);
    TEST_ASSERT_TRUE(t.find(Kind::Ble, ble(9, "", 0).mac) >= 0);
}

void test_sorting() {
    Table t;
    t.observe(wifi(1, "Bravo", -80), 1000, 0);
    t.observe(wifi(2, "alpha", -40), 2000, 0);
    t.observe(wifi(3, "", -60), 3000, 0);
    t.observe(wifi(4, "charlie", -30), 4000, 0);
    const uint32_t now = 40000, window = 37500;  // 4 (age 36 s) and 3 (37 s) in range, 2 and 1 not
    auto name = [&](int i) { return displayName(t.at(i)); };

    auto s = t.sorted(Kind::Wifi, Sort::Signal, now, window);
    TEST_ASSERT_EQUAL(4, (int)s.size());
    TEST_ASSERT_EQUAL_STRING("charlie", name(s[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("(hidden)", name(s[1]).c_str());
    TEST_ASSERT_EQUAL_STRING("alpha", name(s[2]).c_str());  // out of range: most recent first
    TEST_ASSERT_EQUAL_STRING("Bravo", name(s[3]).c_str());
    TEST_ASSERT_EQUAL(2, t.active(Kind::Wifi, now, window));

    s = t.sorted(Kind::Wifi, Sort::Newest, now, window);
    TEST_ASSERT_EQUAL_STRING("charlie", name(s[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("Bravo", name(s[3]).c_str());

    s = t.sorted(Kind::Wifi, Sort::Name, now, window);
    TEST_ASSERT_EQUAL_STRING("alpha", name(s[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("Bravo", name(s[1]).c_str());
    TEST_ASSERT_EQUAL_STRING("charlie", name(s[2]).c_str());
    TEST_ASSERT_EQUAL_STRING("(hidden)", name(s[3]).c_str());  // unnamed last
}

void test_millis_wraparound() {
    Table t;
    t.observe(wifi(1, "old", -40), 0xFFFFF000u, 0);
    t.observe(wifi(2, "new", -90), 0x00000100u, 0);
    const uint32_t now = 0x00000200u;
    TEST_ASSERT_EQUAL(2, t.active(Kind::Wifi, now, 10000));
    auto s = t.sorted(Kind::Wifi, Sort::Newest, now, 10000);
    TEST_ASSERT_EQUAL_STRING("new", t.at(s[0]).name.c_str());
}

void test_names() {
    TEST_ASSERT_EQUAL_STRING("10:20:30:40:50:ab", macString(wifi(0xab, "", 0).mac).c_str());
    TEST_ASSERT_EQUAL_STRING("WPA2", authName(AuthWpa2));
    TEST_ASSERT_EQUAL_STRING("open", authName(AuthOpen));
    TEST_ASSERT_EQUAL_STRING("Apple device", displayName(ble(1, "", 0, 0x004C)).c_str());
    TEST_ASSERT_EQUAL_STRING("unnamed", displayName(ble(1, "", 0, 0x1234)).c_str());
    TEST_ASSERT_EQUAL_STRING("now", ago(1200).c_str());
    TEST_ASSERT_EQUAL_STRING("42s", ago(42000).c_str());
    TEST_ASSERT_EQUAL_STRING("5m", ago(5 * 60000 + 999).c_str());
    TEST_ASSERT_EQUAL_STRING("3h", ago(3 * 3600000u).c_str());
    TEST_ASSERT_EQUAL_STRING("2023-11-14 22:13:20", formatTime(1700000000).c_str());
    TEST_ASSERT_EQUAL_STRING("", formatTime(0).c_str());
}

void test_csv() {
    TEST_ASSERT_EQUAL_STRING("plain", csvField("plain").c_str());
    TEST_ASSERT_EQUAL_STRING("\"a,b\"", csvField("a,b").c_str());
    TEST_ASSERT_EQUAL_STRING("\"say \"\"hi\"\"\"", csvField("say \"hi\"").c_str());
    TEST_ASSERT_EQUAL_STRING("tab?nl?", csvField("tab\tnl\n").c_str());
    TEST_ASSERT_EQUAL_STRING("\" pad\"", csvField(" pad").c_str());

    std::string h = csvHeader("1.2.3");
    TEST_ASSERT_EQUAL(0, (int)h.find("WigleWifi-1.4,appRelease=1.2.3,"));
    TEST_ASSERT_TRUE(h.find("\nMAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,") != std::string::npos);

    Table t;
    int i = t.observe(wifi(0x0f, "Cafe, free", -67, 11, AuthOpen), 0, 1700000000);
    TEST_ASSERT_EQUAL_STRING("10:20:30:40:50:0f,\"Cafe, free\",[ESS],2023-11-14 22:13:20,11,-67,0.0,0.0,0,0,WIFI\n",
                             csvRow(t.at(i)).c_str());
    i = t.observe(ble(1, "", -80, 0x004C), 0, 0);
    TEST_ASSERT_EQUAL_STRING("c0:ff:ee:00:00:01,,Misc [LE] Apple,,0,-80,0.0,0.0,0,0,BLE\n", csvRow(t.at(i)).c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_observe_new_and_update);
    RUN_TEST(test_ble_keeps_name_and_company);
    RUN_TEST(test_capacity_drops_least_recent);
    RUN_TEST(test_sorting);
    RUN_TEST(test_millis_wraparound);
    RUN_TEST(test_names);
    RUN_TEST(test_csv);
    return UNITY_END();
}
