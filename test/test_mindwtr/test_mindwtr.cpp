#include <unity.h>

#include <string>
#include <vector>

#include "mw_json.h"
#include "mindwtr.h"

using json::Value;

void setUp() {}
void tearDown() {}

static const char* kDoc =
    "\xEF\xBB\xBF{\"tasks\": [\n"
    "  {\"id\":\"a\",\"title\":\"Buy milk \\\"2%\\\" \\u00e9\\ud83d\\ude00\",\"status\":\"next\",\"tags\":[\"#shop\"],"
    "\"contexts\":[\"@store\"],\"dueDate\":\"2026-11-30\",\"rev\":4,\"createdAt\":\"x\",\"updatedAt\":\"y\","
    "\"checklist\":[{\"id\":\"c1\",\"title\":\"x]\",\"isCompleted\":true},{\"id\":\"c2\",\"title\":\"y\",\"isCompleted\":false}]},\n"
    "  {\"id\":\"b\",\"title\":\"{not [an] object}\",\"status\":\"done\",\"completedAt\":\"2026-11-01T10:00:00.000Z\","
    "\"tags\":[],\"contexts\":[],\"deletedAt\":\"2026-11-02T00:00:00Z\",\"recurrence\":{\"rule\":\"daily\"},\"n\":-1.5e3}\n"
    "], \"projects\":[{\"id\":\"p1\",\"title\":\"Home\",\"status\":\"active\"}], \"sections\":[], "
    "\"settings\":{\"tasks\":[{\"id\":\"decoy\"}]}}\n";

void test_parse_and_serialize() {
    Value v;
    TEST_ASSERT_TRUE(json::parse(std::string(kDoc), v));
    const Value* tasks = v.get("tasks");
    TEST_ASSERT_NOT_NULL(tasks);
    TEST_ASSERT_EQUAL(2, (int)tasks->arr.size());
    TEST_ASSERT_EQUAL_STRING("Buy milk \"2%\" \xC3\xA9\xF0\x9F\x98\x80", tasks->arr[0].getString("title").c_str());
    TEST_ASSERT_EQUAL(4, (int)tasks->arr[0].getInt("rev"));
    TEST_ASSERT_EQUAL_STRING("-1.5e3", tasks->arr[1].get("n")->str.c_str());
    // round trip keeps everything
    Value again;
    TEST_ASSERT_TRUE(json::parse(json::serialize(v), again));
    TEST_ASSERT_EQUAL_STRING(json::serialize(v).c_str(), json::serialize(again).c_str());
    TEST_ASSERT_EQUAL_STRING("{\"a\":\"x\\n\\u0001\\\\\"}", json::serialize(*[] {
                                 static Value o = Value::object();
                                 o.set("a", Value::string("x\n\x01\\"));
                                 return &o;
                             }()).c_str());
    Value bad;
    TEST_ASSERT_FALSE(json::parse(std::string("{\"a\":1,}"), bad));
    TEST_ASSERT_FALSE(json::parse(std::string("{\"a\":1} x"), bad));
    TEST_ASSERT_FALSE(json::parse(std::string("[1,2"), bad));
    TEST_ASSERT_FALSE(json::parse(std::string("\"tab\there\""), bad));
}

struct Found {
    std::string key, text;
    size_t start, end;
};

static std::vector<Found> scan(const std::string& doc, size_t chunk, bool& ok, json::ElementScanner** keep = nullptr) {
    std::vector<Found> out;
    static json::ElementScanner* sc = nullptr;
    delete sc;
    sc = new json::ElementScanner({"tasks", "projects"}, [&](const std::string& k, size_t s, size_t e, const std::string& t,
                                                            bool) { out.push_back({k, t, s, e}); });
    for (size_t i = 0; i < doc.size(); i += chunk) sc->feed(doc.data() + i, std::min(chunk, doc.size() - i));
    ok = sc->finish();
    if (keep) *keep = sc;
    return out;
}

void test_scanner_any_chunking() {
    const std::string doc = kDoc;
    for (size_t chunk : {1, 2, 3, 7, 64, 4096}) {
        bool ok;
        json::ElementScanner* sc;
        auto found = scan(doc, chunk, ok, &sc);
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_EQUAL(3, (int)found.size());
        TEST_ASSERT_EQUAL_STRING("tasks", found[0].key.c_str());
        TEST_ASSERT_EQUAL_STRING("projects", found[2].key.c_str());
        for (auto& f : found) {
            TEST_ASSERT_EQUAL_STRING(doc.substr(f.start, f.end - f.start).c_str(), f.text.c_str());
            Value v;
            TEST_ASSERT_TRUE(json::parse(f.text, v));
        }
        auto t = sc->array("tasks");
        TEST_ASSERT_TRUE(t.found);
        TEST_ASSERT_FALSE(t.empty);
        TEST_ASSERT_EQUAL('[', doc[t.open]);
        TEST_ASSERT_EQUAL(']', doc[t.close]);
        TEST_ASSERT_TRUE(sc->array("projects").found);
    }
    bool ok;
    scan("{\"tasks\":[{\"id\":\"x\"}]", 5, ok);
    TEST_ASSERT_FALSE(ok);  // cut short
    scan("[{\"id\":\"x\"}]", 5, ok);
    TEST_ASSERT_FALSE(ok);  // not an object
    scan("not json", 5, ok);
    TEST_ASSERT_FALSE(ok);
    json::ElementScanner* sc;
    auto found = scan("{\"tasks\" : [ ] , \"projects\":[]}", 3, ok, &sc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(0, (int)found.size());
    TEST_ASSERT_TRUE(sc->array("tasks").empty);
    TEST_ASSERT_TRUE(sc->array("tasks").found);
}

void test_splicer() {
    const std::string src = "0123456789";
    std::vector<json::Splice> sp = {{7, 9, "ab"}, {2, 4, ""}, {5, 5, "X"}, {5, 5, "Y"}, {10, 10, "END"}};
    for (size_t chunk : {1, 3, 10}) {
        std::string out;
        json::Splicer s(sp, [&](const char* d, size_t n) { out.append(d, n); });
        for (size_t i = 0; i < src.size(); i += chunk) s.feed(src.data() + i, std::min(chunk, src.size() - i));
        s.finish();
        TEST_ASSERT_EQUAL_STRING("014XY56ab9END", out.c_str());
        TEST_ASSERT_EQUAL(out.size(), json::splicedSize(src.size(), sp));
    }
}

void test_summary_and_patches() {
    Value v;
    TEST_ASSERT_TRUE(json::parse(std::string(kDoc), v));
    mindwtr::Task t;
    TEST_ASSERT_TRUE(mindwtr::summarize(v.get("tasks")->arr[0], t));
    TEST_ASSERT_EQUAL_STRING("next", t.status.c_str());
    TEST_ASSERT_EQUAL_STRING("2026-11-30", t.dueDate.c_str());
    TEST_ASSERT_EQUAL(2, t.checklistTotal);
    TEST_ASSERT_EQUAL(1, t.checklistDone);
    TEST_ASSERT_EQUAL_STRING("@store", t.contexts[0].c_str());
    TEST_ASSERT_FALSE(t.deleted);
    mindwtr::Task b;
    TEST_ASSERT_TRUE(mindwtr::summarize(v.get("tasks")->arr[1], b));
    TEST_ASSERT_TRUE(b.deleted);
    TEST_ASSERT_TRUE(b.recurring);
    mindwtr::TaskRow row;
    TEST_ASSERT_TRUE(mindwtr::summarize(v.get("tasks")->arr[0], row, 12));
    TEST_ASSERT_EQUAL_STRING("Buy milk \"2%", row.title.c_str());
    TEST_ASSERT_TRUE(mindwtr::summarize(v.get("tasks")->arr[0], row, 15));
    TEST_ASSERT_EQUAL_STRING("Buy milk \"2%\" ", row.title.c_str());  // the 2-byte character is not cut
    TEST_ASSERT_EQUAL(0, (int)row.completedKey);
    TEST_ASSERT_TRUE(mindwtr::summarize(v.get("tasks")->arr[1], row));
    TEST_ASSERT_TRUE(row.deleted && row.recurring);
    TEST_ASSERT_TRUE(mindwtr::minuteKey("2026-11-01T10:00:00.000Z") == mindwtr::minuteKey("2026-11-01") + 600);

    // mark done, then edit the title: the patch keeps both
    Value task = v.get("tasks")->arr[0];
    mindwtr::Patch p;
    p.id = "a";
    mindwtr::changeStatus(p, task, "done", "NOW");
    p.setField("title", Value::string("Milk"));
    p.setField("updatedAt", Value::string("NOW"));
    p.unsetField("dueDate");
    // stored and loaded again
    std::vector<mindwtr::Patch> loaded;
    TEST_ASSERT_TRUE(mindwtr::parsePatches(mindwtr::serializePatches({p}), loaded));
    TEST_ASSERT_EQUAL(1, (int)loaded.size());
    mindwtr::applyForUpload(task, loaded[0], "dev1");
    TEST_ASSERT_EQUAL_STRING("done", task.getString("status").c_str());
    TEST_ASSERT_EQUAL_STRING("NOW", task.getString("completedAt").c_str());
    TEST_ASSERT_EQUAL_STRING("Milk", task.getString("title").c_str());
    TEST_ASSERT_EQUAL_STRING("NOW", task.getString("updatedAt").c_str());
    TEST_ASSERT_NULL(task.get("dueDate"));
    TEST_ASSERT_EQUAL(5, (int)task.getInt("rev"));
    TEST_ASSERT_EQUAL_STRING("dev1", task.getString("revBy").c_str());
    TEST_ASSERT_EQUAL_STRING("x", task.getString("createdAt").c_str());  // untouched fields stay
    TEST_ASSERT_NOT_NULL(task.get("checklist"));

    // reopening clears completedAt
    mindwtr::Patch r;
    mindwtr::changeStatus(r, task, "next", "LATER");
    mindwtr::applyFields(task, r);
    TEST_ASSERT_NULL(task.get("completedAt"));
    TEST_ASSERT_EQUAL_STRING("next", task.getString("status").c_str());

    // a new task
    mindwtr::Patch n = mindwtr::newTask("id1", "Call Bob", "", "T0");
    Value fresh;
    mindwtr::applyForUpload(fresh, n, "dev1");
    mindwtr::Task nt;
    TEST_ASSERT_TRUE(mindwtr::summarize(fresh, nt));
    TEST_ASSERT_EQUAL_STRING("inbox", nt.status.c_str());
    TEST_ASSERT_EQUAL(1, (int)fresh.getInt("rev"));
    TEST_ASSERT_NOT_NULL(fresh.get("tags"));
}

void test_dates() {
    const mindwtr::Date today{2026, 11, 26};  // a Thursday
    TEST_ASSERT_EQUAL(4, mindwtr::weekday(today));
    std::string out;
    auto ok = [&](const char* in, const char* want) {
        std::string o;
        TEST_ASSERT_TRUE_MESSAGE(mindwtr::parseDateInput(in, today, o), in);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, o.c_str(), in);
    };
    ok("", "");
    ok(" - ", "");
    ok("today", "2026-11-26");
    ok("Tom", "2026-11-27");
    ok("+3", "2026-11-29");
    ok("+2w", "2026-12-10");
    ok("+1m", "2026-12-26");
    ok("fri", "2026-11-27");
    ok("thursday", "2026-12-03");  // today is Thursday: the next one
    ok("mon 9:30", "2026-11-30T09:30");
    ok("tomorrow at 2pm", "2026-11-27T14:00");
    ok("17:45", "2026-11-26T17:45");
    ok("2027-01-05", "2027-01-05");
    ok("2027-01-05T08:00", "2027-01-05T08:00");
    ok("12-24", "2026-12-24");
    ok("1-3", "2027-01-03");  // already past this year
    ok("24.12.2028", "2028-12-24");
    ok("next week", "2026-12-03");
    std::string o;
    TEST_ASSERT_FALSE(mindwtr::parseDateInput("2026-02-30", today, o));
    TEST_ASSERT_FALSE(mindwtr::parseDateInput("someday", today, o));
    TEST_ASSERT_FALSE(mindwtr::parseDateInput("mon 25:00", today, o));
    TEST_ASSERT_FALSE(mindwtr::parseDateInput("x", today, o));

    TEST_ASSERT_EQUAL_STRING("today", mindwtr::dateLabel("2026-11-26", today).c_str());
    TEST_ASSERT_EQUAL_STRING("tomorrow 09:00", mindwtr::dateLabel("2026-11-27T09:00", today).c_str());
    TEST_ASSERT_EQUAL_STRING("Mon 30 Nov", mindwtr::dateLabel("2026-11-30", today).c_str());
    TEST_ASSERT_EQUAL_STRING("Tue 5 Jan", mindwtr::dateLabel("2027-01-05", today).c_str());
    TEST_ASSERT_EQUAL_STRING("3 Mar 2025", mindwtr::dateLabel("2025-03-03", today).c_str());
    long d;
    TEST_ASSERT_TRUE(mindwtr::daysUntil("2026-11-20T10:00:00.000Z", today, d));
    TEST_ASSERT_EQUAL(-6, (int)d);
    TEST_ASSERT_FALSE(mindwtr::daysUntil("soon", today, d));

    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00.000Z", mindwtr::isoTimestamp(0).c_str());
    TEST_ASSERT_EQUAL_STRING("2026-11-26T09:05:03.000Z", mindwtr::isoTimestamp(1795683903).c_str());
    const uint8_t bytes[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_STRING("ffffffff-ffff-4fff-bfff-ffffffffffff", mindwtr::uuidFromBytes(bytes).c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_and_serialize);
    RUN_TEST(test_scanner_any_chunking);
    RUN_TEST(test_splicer);
    RUN_TEST(test_summary_and_patches);
    RUN_TEST(test_dates);
    return UNITY_END();
}
