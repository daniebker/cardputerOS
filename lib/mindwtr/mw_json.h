// Small JSON toolkit for editing a large JSON document on a device with little
// memory: a DOM for single objects, a streaming scanner that finds the elements
// of top-level arrays (with their byte offsets), and a splicer that rewrites a
// stream with some byte ranges replaced. Portable, unit tested on the host.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace json {

// ---------------------------------------------------------------- DOM
struct Value {
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    std::string str;  // String: the (decoded) text; Number: the number as written
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value string(const std::string& s);
    static Value number(long long n);
    static Value boolean(bool v);
    static Value array();
    static Value object();

    bool isString() const { return type == String; }
    // Object member, or nullptr.
    const Value* get(const std::string& key) const;
    Value* get(const std::string& key);
    // Replaces the member in place, or appends it.
    void set(const std::string& key, Value v);
    bool erase(const std::string& key);
    // Convenience readers: "" / def when missing or of another type.
    std::string getString(const std::string& key) const;
    long long getInt(const std::string& key, long long def = 0) const;
    std::vector<std::string> getStrings(const std::string& key) const;
};

// Parses one JSON value (the whole input, surrounding whitespace allowed).
bool parse(const char* data, size_t len, Value& out);
inline bool parse(const std::string& text, Value& out) { return parse(text.data(), text.size(), out); }
// Compact JSON. Numbers are written back exactly as they were read.
std::string serialize(const Value& v);
void serialize(const Value& v, std::string& out);
void appendQuoted(std::string& out, const std::string& s);

// ---------------------------------------------------------------- streaming scanner
// Walks a document of the form {"key": [...], ...} without keeping it in memory.
// For each object element of the top-level arrays named in `keys` it calls `fn`
// with the element's byte range [start, end) and its text (one element in
// memory at a time; elements over maxElement bytes are reported with
// truncated = true and partial text).
class ElementScanner {
public:
    using Fn = std::function<void(const std::string& key, size_t start, size_t end, const std::string& text,
                                  bool truncated)>;
    struct ArrayInfo {
        bool found = false;
        size_t open = 0, close = 0;  // offsets of '[' and ']'
        bool empty = true;           // no elements
    };

    ElementScanner(std::vector<std::string> keys, Fn fn, size_t maxElement = 32768);
    void feed(const char* data, size_t len);
    // True if the input was one complete JSON object (structure only).
    bool finish();
    ArrayInfo array(const std::string& key) const;
    size_t rootOpen() const { return rootOpen_; }

private:
    struct Level {
        char type;  // '{' or '['
        bool expectKey;
        int arrayIndex;  // index into keys_ for a wanted top-level array, else -1
    };
    void onChar(char c, size_t pos);

    std::vector<std::string> keys_;
    std::vector<ArrayInfo> arrays_;
    Fn fn_;
    size_t maxElement_;
    std::vector<Level> stack_;
    size_t pos_ = 0, rootOpen_ = 0;
    bool error_ = false, done_ = false, inString_ = false, escape_ = false, stringIsKey_ = false;
    std::string key_, lastKey_;
    // element capture
    bool capturing_ = false, truncated_ = false;
    size_t captureStart_ = 0;
    int captureArray_ = -1;
    std::string capture_;
    int bom_ = 0;
};

// ---------------------------------------------------------------- splicing
struct Splice {
    size_t start, end;  // replace [start, end) of the source (start == end: insert)
    std::string text;
};

// Size of the output after applying `splices` to a source of `srcSize` bytes.
size_t splicedSize(size_t srcSize, const std::vector<Splice>& splices);

// Streams a source through, applying splices (sorted by start, non-overlapping;
// insertions at the same offset keep their order).
class Splicer {
public:
    using Out = std::function<void(const char* data, size_t len)>;
    Splicer(std::vector<Splice> splices, Out out);
    void feed(const char* data, size_t len);
    // Emits splices at or after the end of the source.
    void finish();

private:
    std::vector<Splice> splices_;
    Out out_;
    size_t pos_ = 0, next_ = 0, skipEnd_ = 0;
};

}  // namespace json
