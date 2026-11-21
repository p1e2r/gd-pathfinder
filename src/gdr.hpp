#pragma once
// Minimal GDR (Geometry Dash Replay) writer.
// .gdr is MessagePack, .gdr.json is JSON; both carry the same fields that
// xdBot and other GDR bots read (gameVersion, bot, level, framerate, inputs...).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <utility>

namespace pf::gdr {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>; // keeps key order

struct Value {
    std::variant<std::nullptr_t, bool, int64_t, double, std::string,
                 std::shared_ptr<Array>, std::shared_ptr<Object>> v;

    Value() : v(nullptr) {}
    Value(bool b) : v(b) {}
    Value(int i) : v(int64_t(i)) {}
    Value(int64_t i) : v(i) {}
    Value(uint32_t i) : v(int64_t(i)) {}
    Value(double d) : v(d) {}
    Value(float f) : v(double(f)) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(Array a) : v(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : v(std::make_shared<Object>(std::move(o))) {}
};

namespace detail {
inline void be(std::vector<uint8_t>& o, uint64_t x, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) o.push_back(uint8_t(x >> (8 * i)));
}

inline void pack(std::vector<uint8_t>& o, Value const& val) {
    std::visit([&](auto const& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            o.push_back(0xc0);
        } else if constexpr (std::is_same_v<T, bool>) {
            o.push_back(x ? 0xc3 : 0xc2);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (x >= 0) {
                if (x < 128) o.push_back(uint8_t(x));
                else if (x <= 0xff) { o.push_back(0xcc); be(o, x, 1); }
                else if (x <= 0xffff) { o.push_back(0xcd); be(o, x, 2); }
                else if (x <= 0xffffffffLL) { o.push_back(0xce); be(o, x, 4); }
                else { o.push_back(0xcf); be(o, uint64_t(x), 8); }
            } else {
                if (x >= -32) o.push_back(uint8_t(int8_t(x)));
                else if (x >= -128) { o.push_back(0xd0); be(o, uint8_t(int8_t(x)), 1); }
                else if (x >= -32768) { o.push_back(0xd1); be(o, uint16_t(int16_t(x)), 2); }
                else if (x >= INT32_MIN) { o.push_back(0xd2); be(o, uint32_t(int32_t(x)), 4); }
                else { o.push_back(0xd3); be(o, uint64_t(x), 8); }
            }
        } else if constexpr (std::is_same_v<T, double>) {
            // float32, like the reference GDR library writes for its float fields
            float f = float(x);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            o.push_back(0xca);
            be(o, bits, 4);
        } else if constexpr (std::is_same_v<T, std::string>) {
            size_t n = x.size();
            if (n < 32) o.push_back(uint8_t(0xa0 | n));
            else if (n <= 0xff) { o.push_back(0xd9); be(o, n, 1); }
            else if (n <= 0xffff) { o.push_back(0xda); be(o, n, 2); }
            else { o.push_back(0xdb); be(o, n, 4); }
            o.insert(o.end(), x.begin(), x.end());
        } else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
            size_t n = x->size();
            if (n < 16) o.push_back(uint8_t(0x90 | n));
            else if (n <= 0xffff) { o.push_back(0xdc); be(o, n, 2); }
            else { o.push_back(0xdd); be(o, n, 4); }
            for (auto const& e : *x) pack(o, e);
        } else if constexpr (std::is_same_v<T, std::shared_ptr<Object>>) {
            size_t n = x->size();
            if (n < 16) o.push_back(uint8_t(0x80 | n));
            else if (n <= 0xffff) { o.push_back(0xde); be(o, n, 2); }
            else { o.push_back(0xdf); be(o, n, 4); }
            for (auto const& [k, e] : *x) { pack(o, Value(k)); pack(o, e); }
        }
    }, val.v);
}

inline void jsonString(std::string& o, std::string const& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else o += char(c);
        }
    }
    o += '"';
}

inline void json(std::string& o, Value const& val) {
    std::visit([&](auto const& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) o += "null";
        else if constexpr (std::is_same_v<T, bool>) o += x ? "true" : "false";
        else if constexpr (std::is_same_v<T, int64_t>) o += std::to_string(x);
        else if constexpr (std::is_same_v<T, double>) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.9g", x);
            o += buf;
        }
        else if constexpr (std::is_same_v<T, std::string>) jsonString(o, x);
        else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
            o += '[';
            for (size_t i = 0; i < x->size(); ++i) { if (i) o += ','; json(o, (*x)[i]); }
            o += ']';
        } else if constexpr (std::is_same_v<T, std::shared_ptr<Object>>) {
            o += '{';
            for (size_t i = 0; i < x->size(); ++i) {
                if (i) o += ',';
                jsonString(o, (*x)[i].first);
                o += ':';
                json(o, (*x)[i].second);
            }
            o += '}';
        }
    }, val.v);
}
} // namespace detail

struct Input {
    int frame;
    int button; // 1 = jump, 2 = left, 3 = right
    bool player2;
    bool down;
};

struct Replay {
    std::string author;
    std::string description;
    float gameVersion = 2.2091f;
    float version = 1.0f;
    float framerate = 240.f;
    float duration = 0.f;
    int seed = 0;
    int coins = 0;
    bool ldm = false;
    std::string botName = "Pathfinder";
    std::string botVersion;
    uint32_t levelID = 0;
    std::string levelName;
    std::vector<Input> inputs;

    Value toValue() const {
        Array ins;
        ins.reserve(inputs.size());
        for (auto const& i : inputs) {
            ins.emplace_back(Object{
                {"frame", i.frame}, {"btn", i.button}, {"2p", i.player2}, {"down", i.down},
            });
        }
        return Object{
            {"gameVersion", gameVersion},
            {"description", description},
            {"version", version},
            {"duration", duration},
            {"bot", Object{{"name", botName}, {"version", botVersion}}},
            {"level", Object{{"id", levelID}, {"name", levelName}}},
            {"author", author},
            {"seed", seed},
            {"coins", coins},
            {"ldm", ldm},
            {"framerate", framerate},
            {"inputs", std::move(ins)},
            {"frameFixes", Array{}},
        };
    }

    std::vector<uint8_t> toMsgpack() const {
        std::vector<uint8_t> out;
        detail::pack(out, toValue());
        return out;
    }

    std::string toJson() const {
        std::string out;
        detail::json(out, toValue());
        return out;
    }
};

} // namespace pf::gdr
