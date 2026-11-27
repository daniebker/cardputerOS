#include "mw_json.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace json {

// ---------------------------------------------------------------- DOM

Value Value::string(const std::string& s) {
    Value v;
    v.type = String;
    v.str = s;
    return v;
}

Value Value::number(long long n) {
    Value v;
    v.type = Number;
    v.str = std::to_string(n);
    return v;
}

Value Value::boolean(bool b) {
    Value v;
    v.type = Bool;
    v.b = b;
    return v;
}

Value Value::array() {
    Value v;
    v.type = Array;
    return v;
}

Value Value::object() {
    Value v;
    v.type = Object;
    return v;
}

const Value* Value::get(const std::string& key) const {
    if (type != Object) return nullptr;
    for (auto& m : obj)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value* Value::get(const std::string& key) {
    if (type != Object) return nullptr;
    for (auto& m : obj)
        if (m.first == key) return &m.second;
    return nullptr;
}

void Value::set(const std::string& key, Value v) {
    if (type != Object) {
        *this = object();
    }
    if (Value* m = get(key)) {
        *m = std::move(v);
        return;
    }
    obj.emplace_back(key, std::move(v));
}

bool Value::erase(const std::string& key) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it->first == key) {
            obj.erase(it);
            return true;
        }
    }
    return false;
}

std::string Value::getString(const std::string& key) const {
    const Value* v = get(key);
    return v && v->type == String ? v->str : std::string();
}

long long Value::getInt(const std::string& key, long long def) const {
    const Value* v = get(key);
    if (!v || v->type != Number) return def;
    return strtoll(v->str.c_str(), nullptr, 10);
}

std::vector<std::string> Value::getStrings(const std::string& key) const {
    std::vector<std::string> out;
    const Value* v = get(key);
    if (!v || v->type != Array) return out;
    for (auto& e : v->arr)
        if (e.type == String) out.push_back(e.str);
    return out;
}

namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t& out) {
        if (end - p < 4) return false;
        out = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool str(std::string& out) {
        // at the opening quote
        p++;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p >= end) return false;
            c = *p++;
            switch (c) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t lo;
                        if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else {
                                utf8(out, 0xFFFD);
                                cp = lo;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        cp = 0xFFFD;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool literal(const char* word) {
        size_t n = strlen(word);
        if ((size_t)(end - p) < n || memcmp(p, word, n) != 0) return false;
        p += n;
        return true;
    }

    bool value(Value& v) {
        ws();
        if (p >= end) return false;
        const char c = *p;
        if (c == '{' || c == '[') {
            if (++depth > 64) return false;
            p++;
            const char close = c == '{' ? '}' : ']';
            v = c == '{' ? Value::object() : Value::array();
            ws();
            if (p < end && *p == close) {
                p++;
                depth--;
                return true;
            }
            for (;;) {
                if (c == '{') {
                    ws();
                    if (p >= end || *p != '"') return false;
                    std::string key;
                    if (!str(key)) return false;
                    ws();
                    if (p >= end || *p != ':') return false;
                    p++;
                    v.obj.emplace_back(std::move(key), Value());
                    if (!value(v.obj.back().second)) return false;
                } else {
                    v.arr.emplace_back();
                    if (!value(v.arr.back())) return false;
                }
                ws();
                if (p >= end) return false;
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p != close) return false;
                p++;
                depth--;
                return true;
            }
        }
        if (c == '"') {
            v.type = Value::String;
            return str(v.str);
        }
        if (literal("true")) {
            v = Value::boolean(true);
            return true;
        }
        if (literal("false")) {
            v = Value::boolean(false);
            return true;
        }
        if (literal("null")) {
            v = Value();
            return true;
        }
        // number
        const char* s = p;
        if (p < end && *p == '-') p++;
        bool digits = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            digits = true;
            p++;
        }
        if (!digits) return false;
        v.type = Value::Number;
        v.str.assign(s, p - s);
        return true;
    }
};

}  // namespace

bool parse(const char* data, size_t len, Value& out) {
    Parser ps{data, data + len};
    // a UTF-8 byte order mark is allowed
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        ps.p += 3;
    out = Value();
    if (!ps.value(out)) return false;
    ps.ws();
    return ps.p == ps.end;
}

void appendQuoted(std::string& out, const std::string& s) {
    static const char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 15]);
                    out.push_back(hex[c & 15]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void serialize(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.b ? "true" : "false"; break;
        case Value::Number: out += v.str; break;
        case Value::String: appendQuoted(out, v.str); break;
        case Value::Array:
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out.push_back(',');
                serialize(v.arr[i], out);
            }
            out.push_back(']');
            break;
        case Value::Object:
            out.push_back('{');
            for (size_t i = 0; i < v.obj.size(); i++) {
                if (i) out.push_back(',');
                appendQuoted(out, v.obj[i].first);
                out.push_back(':');
                serialize(v.obj[i].second, out);
            }
            out.push_back('}');
            break;
    }
}

std::string serialize(const Value& v) {
    std::string out;
    serialize(v, out);
    return out;
}

// ---------------------------------------------------------------- streaming scanner

ElementScanner::ElementScanner(std::vector<std::string> keys, Fn fn, size_t maxElement)
    : keys_(std::move(keys)), arrays_(keys_.size()), fn_(std::move(fn)), maxElement_(maxElement) {}

void ElementScanner::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len && !error_; i++) onChar(data[i], pos_ + i);
    pos_ += len;
}

void ElementScanner::onChar(char c, size_t pos) {
    if (capturing_) {
        if (capture_.size() < maxElement_) capture_.push_back(c);
        else truncated_ = true;
    }
    if (inString_) {
        if (escape_) {
            escape_ = false;
            if (stringIsKey_ && key_.size() < 64) key_.push_back(c);
        } else if (c == '\\') {
            escape_ = true;
            if (stringIsKey_ && key_.size() < 64) key_.push_back(c);
        } else if (c == '"') {
            inString_ = false;
            if (stringIsKey_) lastKey_ = key_;
        } else if (stringIsKey_ && key_.size() < 64) {
            key_.push_back(c);
        }
        return;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return;
    if (done_) {
        error_ = true;  // something after the root object
        return;
    }
    if (stack_.empty()) {
        // optional UTF-8 BOM before the root object
        static const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
        if (bom_ < 3 && pos == (size_t)bom_ && (unsigned char)c == kBom[bom_]) {
            bom_++;
            return;
        }
        if (c != '{') {
            error_ = true;
            return;
        }
        rootOpen_ = pos;
        stack_.push_back({'{', true, -1});
        return;
    }

    Level& top = stack_.back();
    auto markValue = [&]() {
        if (stack_.size() == 2 && stack_.back().arrayIndex >= 0) arrays_[stack_.back().arrayIndex].empty = false;
    };
    switch (c) {
        case '"':
            inString_ = true;
            stringIsKey_ = stack_.size() == 1 && top.expectKey;
            key_.clear();
            if (!stringIsKey_) markValue();
            break;
        case ':':
            top.expectKey = false;
            break;
        case ',':
            if (top.type == '{') top.expectKey = true;
            break;
        case '{':
        case '[': {
            if (stack_.size() == 2 && top.arrayIndex >= 0 && c == '{' && !capturing_) {
                capturing_ = true;
                truncated_ = false;
                captureStart_ = pos;
                captureArray_ = top.arrayIndex;
                capture_.assign(1, '{');
            }
            markValue();
            int ai = -1;
            if (stack_.size() == 1 && c == '[') {
                for (size_t k = 0; k < keys_.size(); k++) {
                    if (keys_[k] == lastKey_ && !arrays_[k].found) {
                        ai = (int)k;
                        arrays_[k].found = true;
                        arrays_[k].open = pos;
                    }
                }
            }
            if (stack_.size() >= 256) {
                error_ = true;
                return;
            }
            stack_.push_back({c, c == '{', ai});
            break;
        }
        case '}':
        case ']': {
            if ((c == '}') != (top.type == '{')) {
                error_ = true;
                return;
            }
            const Level closed = top;
            stack_.pop_back();
            if (closed.arrayIndex >= 0) arrays_[closed.arrayIndex].close = pos;
            if (stack_.empty()) {
                done_ = true;
            } else if (capturing_ && stack_.size() == 2 && c == '}') {
                capturing_ = false;
                if (fn_) fn_(keys_[captureArray_], captureStart_, pos + 1, capture_, truncated_);
                capture_.clear();
                capture_.shrink_to_fit();
            }
            break;
        }
        default:
            markValue();
            break;
    }
}

bool ElementScanner::finish() { return !error_ && done_ && !inString_; }

ElementScanner::ArrayInfo ElementScanner::array(const std::string& key) const {
    for (size_t k = 0; k < keys_.size(); k++)
        if (keys_[k] == key) return arrays_[k];
    return ArrayInfo();
}

// ---------------------------------------------------------------- splicing

size_t splicedSize(size_t srcSize, const std::vector<Splice>& splices) {
    size_t n = srcSize;
    for (auto& s : splices) n = n - (s.end - s.start) + s.text.size();
    return n;
}

Splicer::Splicer(std::vector<Splice> splices, Out out) : splices_(std::move(splices)), out_(std::move(out)) {
    std::stable_sort(splices_.begin(), splices_.end(),
                     [](const Splice& a, const Splice& b) { return a.start < b.start; });
}

void Splicer::feed(const char* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        const size_t abs = pos_ + i;
        if (abs < skipEnd_) {
            const size_t k = std::min(len - i, skipEnd_ - abs);
            i += k;
            continue;
        }
        if (next_ < splices_.size() && splices_[next_].start <= abs) {
            const Splice& s = splices_[next_++];
            if (!s.text.empty()) out_(s.text.data(), s.text.size());
            skipEnd_ = std::max(skipEnd_, s.end);
            continue;
        }
        size_t stop = len;
        if (next_ < splices_.size()) stop = std::min(len, splices_[next_].start - pos_);
        out_(data + i, stop - i);
        i = stop;
    }
    pos_ += len;
}

void Splicer::finish() {
    while (next_ < splices_.size()) {
        const Splice& s = splices_[next_++];
        if (!s.text.empty()) out_(s.text.data(), s.text.size());
    }
}

}  // namespace json
