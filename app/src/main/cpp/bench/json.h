// Minimal dependency-free JSON emitter. Not a parser, not a validator — we
// produce structurally well-formed JSON, escape the few characters that matter
// for shell-piping (", \, control chars), and emit numbers with full double
// precision (17 sig digits) so consumers can round-trip if they want.
#pragma once

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

class Json {
public:
    Json() = default;

    Json& kv(const std::string& key, double v) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        push(key, buf);
        return *this;
    }
    Json& kv(const std::string& key, int64_t v) {
        push(key, std::to_string(v));
        return *this;
    }
    Json& kv(const std::string& key, int v) { return kv(key, static_cast<int64_t>(v)); }
    Json& kv(const std::string& key, uint64_t v) {
        push(key, std::to_string(v));
        return *this;
    }
    Json& kv(const std::string& key, const std::string& v) {
        push(key, quote(v));
        return *this;
    }
    Json& kv(const std::string& key, const char* v) {
        return kv(key, std::string(v));
    }
    Json& kv(const std::string& key, bool v) {
        push(key, v ? "true" : "false");
        return *this;
    }
    Json& kv(const std::string& key, const Json& v) {
        push(key, v.str());
        return *this;
    }
    Json& kv(const std::string& key, const std::vector<double>& v) {
        std::ostringstream oss;
        oss << '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) oss << ',';
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", v[i]);
            oss << buf;
        }
        oss << ']';
        push(key, oss.str());
        return *this;
    }
    Json& kv(const std::string& key, const std::vector<std::string>& v) {
        std::ostringstream oss;
        oss << '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) oss << ',';
            oss << quote(v[i]);
        }
        oss << ']';
        push(key, oss.str());
        return *this;
    }
    Json& kv(const std::string& key, const std::vector<Json>& v) {
        std::ostringstream oss;
        oss << '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) oss << ',';
            oss << v[i].str();
        }
        oss << ']';
        push(key, oss.str());
        return *this;
    }

    std::string str() const {
        std::ostringstream oss;
        oss << '{';
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i) oss << ',';
            oss << quote(entries_[i].first) << ':' << entries_[i].second;
        }
        oss << '}';
        return oss.str();
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;

    void push(const std::string& k, std::string v) {
        entries_.emplace_back(k, std::move(v));
    }

    static std::string quote(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
        return out;
    }
};

} // namespace bench
