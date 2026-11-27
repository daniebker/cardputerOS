#include <unity.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dav.h"

void setUp() {}
void tearDown() {}

static const char* kMulti =
    "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:cal=\"urn:ietf:params:xml:ns:caldav\">"
    "<d:response><d:href>/dav/cal/notes/a%20b.ics</d:href><d:propstat><d:prop>"
    "<d:getetag>&quot;etag-1&quot;</d:getetag><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>\n"
    "<response xmlns=\"DAV:\"><href>https://host.example/dav/cal/notes/two.ics</href><propstat><prop>"
    "<getetag>W/\"2\"</getetag></prop></propstat></response>"
    "<D:response><D:href>/dav/cal/notes/</D:href><D:propstat><D:prop><D:getetag/><D:resourcetype><D:collection/></D:resourcetype></D:prop></D:propstat></D:response>"
    "</d:multistatus>";

void test_multistatus_any_chunking() {
    for (size_t chunk : {1u, 3u, 7u, 64u, 5000u}) {
        std::vector<dav::Resource> got;
        dav::MultistatusParser p([&](const dav::Resource& r) { got.push_back(r); });
        std::string s = kMulti;
        for (size_t i = 0; i < s.size(); i += chunk) p.feed(s.data() + i, std::min(chunk, s.size() - i));
        TEST_ASSERT_EQUAL(3, (int)got.size());
        TEST_ASSERT_EQUAL_STRING("/dav/cal/notes/a%20b.ics", got[0].href.c_str());
        TEST_ASSERT_EQUAL_STRING("\"etag-1\"", got[0].etag.c_str());
        TEST_ASSERT_EQUAL_STRING("/dav/cal/notes/two.ics", dav::hrefPath(got[1].href).c_str());
        TEST_ASSERT_EQUAL_STRING("W/\"2\"", got[1].etag.c_str());
        TEST_ASSERT_EQUAL_STRING("", got[2].etag.c_str());
        TEST_ASSERT_FALSE(got[0].isCollection);
        TEST_ASSERT_FALSE(got[1].isCollection);
        TEST_ASSERT_TRUE(got[2].isCollection);
    }
}

void test_url() {
    dav::Url u;
    TEST_ASSERT_TRUE(dav::parseUrl(" https://cloud.example.org/remote.php/dav/calendars/me/notes/ ", u));
    TEST_ASSERT_TRUE(u.https);
    TEST_ASSERT_EQUAL(443, u.port);
    TEST_ASSERT_EQUAL_STRING("cloud.example.org", u.host.c_str());
    TEST_ASSERT_EQUAL_STRING("/remote.php/dav/calendars/me/notes/", u.path.c_str());
    TEST_ASSERT_TRUE(dav::parseUrl("http://192.168.1.5:5232", u));
    TEST_ASSERT_FALSE(u.https);
    TEST_ASSERT_EQUAL(5232, u.port);
    TEST_ASSERT_EQUAL_STRING("/", u.path.c_str());
    TEST_ASSERT_FALSE(dav::parseUrl("cloud.example.org/dav", u));
    TEST_ASSERT_FALSE(dav::parseUrl("https://user:pw@host/dav", u));
    TEST_ASSERT_FALSE(dav::parseUrl("https:///x", u));
    TEST_ASSERT_EQUAL_STRING("dXNlcjpwYXNz", dav::base64("user:pass").c_str());
    TEST_ASSERT_EQUAL_STRING("YQ==", dav::base64("a").c_str());
    TEST_ASSERT_EQUAL_STRING("YWI=", dav::base64("ab").c_str());
}

static dav::ReadFn feeder(const std::string& data, size_t chunk, size_t* consumed = nullptr) {
    auto pos = std::make_shared<size_t>(0);
    return [=](char* buf, size_t max) -> int {
        size_t n = std::min(std::min(chunk, max), data.size() - *pos);
        memcpy(buf, data.data() + *pos, n);
        *pos += n;
        if (consumed) *consumed = *pos;
        return (int)n;
    };
}

void test_http_content_length() {
    std::string raw = "HTTP/1.1 207 Multi-Status\r\nContent-Type: text/xml\r\nETag: \"abc\"\r\nContent-Length: 5\r\n\r\nhelloNEXT";
    for (size_t chunk : {1u, 4u, 1000u}) {
        dav::HttpResponse r;
        TEST_ASSERT_TRUE(dav::readHttpResponse(feeder(raw, chunk), false, r));
        TEST_ASSERT_EQUAL(207, r.status);
        TEST_ASSERT_EQUAL_STRING("hello", r.body.c_str());
        TEST_ASSERT_EQUAL_STRING("\"abc\"", r.etag.c_str());
        TEST_ASSERT_TRUE(r.keepAlive);
    }
}

void test_http_chunked_and_sink() {
    std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                      "4\r\nWiki\r\n6;ext=1\r\npedia \r\nD\r\nin\r\n\r\nchunks.\r\n0\r\nX-Trailer: 1\r\n\r\n";
    for (size_t chunk : {1u, 5u, 1000u}) {
        dav::HttpResponse r;
        std::string sunk;
        TEST_ASSERT_TRUE(dav::readHttpResponse(feeder(raw, chunk), false, r, [&](const char* d, size_t n) { sunk.append(d, n); }));
        TEST_ASSERT_EQUAL_STRING("Wikipedia in\r\n\r\nchunks.", sunk.c_str());
        TEST_ASSERT_FALSE(r.keepAlive);
    }
}

void test_http_edge_cases() {
    dav::HttpResponse r;
    TEST_ASSERT_TRUE(dav::readHttpResponse(feeder("HTTP/1.1 204 No Content\r\nETag: W/\"9\"\r\n\r\n", 3), false, r));
    TEST_ASSERT_EQUAL(204, r.status);
    TEST_ASSERT_EQUAL_STRING("W/\"9\"", r.etag.c_str());
    // interim response is skipped
    TEST_ASSERT_TRUE(dav::readHttpResponse(feeder("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n", 7), false, r));
    TEST_ASSERT_EQUAL(201, r.status);
    // body until close
    TEST_ASSERT_TRUE(dav::readHttpResponse(feeder("HTTP/1.0 200 OK\r\n\r\nall of it", 2), false, r));
    TEST_ASSERT_EQUAL_STRING("all of it", r.body.c_str());
    TEST_ASSERT_FALSE(r.keepAlive);
    // truncated transfers are errors
    TEST_ASSERT_FALSE(dav::readHttpResponse(feeder("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort", 4), false, r));
    TEST_ASSERT_FALSE(dav::readHttpResponse(feeder("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", 4), false, r));
    TEST_ASSERT_FALSE(dav::readHttpResponse(feeder("", 4), false, r));
    TEST_ASSERT_FALSE(dav::readHttpResponse(feeder("garbage\r\n\r\n", 4), false, r));
    // body cap
    TEST_ASSERT_TRUE(dav::readHttpResponse(feeder("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", 4), false, r, nullptr, 4));
    TEST_ASSERT_EQUAL_STRING("0123", r.body.c_str());
    TEST_ASSERT_TRUE(r.truncated);
    // HEAD has no body even with a length
    TEST_ASSERT_TRUE(dav::readHttpResponse(feeder("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", 4), true, r));
}


void test_names_and_paths() {
    TEST_ASSERT_EQUAL_STRING("Shopping list", dav::fileStem("Shopping list").c_str());
    TEST_ASSERT_EQUAL_STRING("a b c", dav::fileStem("  a/b:c?  ").c_str());
    TEST_ASSERT_EQUAL_STRING("Untitled", dav::fileStem("???").c_str());
    TEST_ASSERT_EQUAL_STRING("hidden", dav::fileStem("..hidden.").c_str());
    TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9", dav::fileStem("caf\xC3\xA9").c_str());
    std::string longName(100, 'x');
    TEST_ASSERT_EQUAL(60, (int)dav::fileStem(longName).size());
    std::string utf = std::string(59, 'x') + "\xC3\xA9\xC3\xA9";
    TEST_ASSERT_EQUAL(59, (int)dav::fileStem(utf).size());  // never cuts a character in half
    TEST_ASSERT_EQUAL_STRING("My%20note%20caf%C3%A9.md", dav::encodeSegment("My note caf\xC3\xA9.md").c_str());
    TEST_ASSERT_EQUAL_STRING("/dav/My note caf\xC3\xA9.md", dav::decodePath("/dav/My%20note%20caf%C3%A9.md").c_str());
    TEST_ASSERT_EQUAL_STRING("100%", dav::decodePath("100%").c_str());
    TEST_ASSERT_EQUAL_STRING("Supported report not set",
        dav::errorMessage("<d:error xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\"><s:message>Supported report not set</s:message></d:error>").c_str());
    dav::HttpResponse r;
    TEST_ASSERT_TRUE(dav::readHttpResponse(feeder("HTTP/1.1 301 Moved\r\nLocation: /x/\r\nContent-Length: 0\r\n\r\n", 5), false, r));
    TEST_ASSERT_EQUAL_STRING("/x/", r.location.c_str());
}

void test_redirects_and_validators() {
    dav::Url u;
    TEST_ASSERT_TRUE(dav::parseUrl("https://cal.example.com:8443/feeds/me/basic.ics?token=abc", u));
    TEST_ASSERT_EQUAL_STRING("/feeds/me/basic.ics?token=abc", u.path.c_str());
    TEST_ASSERT_EQUAL_STRING("https://cal.example.com:8443", dav::origin(u).c_str());
    TEST_ASSERT_EQUAL_STRING("https://other.example/x.ics", dav::resolveUrl(u, " https://other.example/x.ics").c_str());
    TEST_ASSERT_EQUAL_STRING("https://cdn.example/x.ics", dav::resolveUrl(u, "//cdn.example/x.ics").c_str());
    TEST_ASSERT_EQUAL_STRING("https://cal.example.com:8443/new/path.ics", dav::resolveUrl(u, "/new/path.ics").c_str());
    TEST_ASSERT_EQUAL_STRING("https://cal.example.com:8443/feeds/me/other.ics", dav::resolveUrl(u, "other.ics").c_str());
    TEST_ASSERT_EQUAL_STRING("https://cal.example.com:8443/feeds/me/basic.ics?token=new", dav::resolveUrl(u, "?token=new").c_str());
    dav::HttpResponse r;
    TEST_ASSERT_TRUE(dav::readHttpResponse(
        feeder("HTTP/1.1 304 Not Modified\r\nETag: \"v2\"\r\nLast-Modified: Fri, 27 Nov 2026 08:00:00 GMT\r\n\r\n", 3), false, r));
    TEST_ASSERT_EQUAL(304, r.status);
    TEST_ASSERT_EQUAL_STRING("\"v2\"", r.etag.c_str());
    TEST_ASSERT_EQUAL_STRING("Fri, 27 Nov 2026 08:00:00 GMT", r.lastModified.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_multistatus_any_chunking);
    RUN_TEST(test_url);
    RUN_TEST(test_http_content_length);
    RUN_TEST(test_http_chunked_and_sink);
    RUN_TEST(test_http_edge_cases);
    RUN_TEST(test_names_and_paths);
    RUN_TEST(test_redirects_and_validators);
    return UNITY_END();
}
