// Unit tests for bench/json.h. The Json class is a header-only minimal JSON
// emitter used by every benchmark to produce output; correctness here is
// load-bearing for downstream parsers (scripts/compare.py, scripts/dashboard.py).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bench/json.h"

#include <string>
#include <vector>

using bench::Json;

TEST_CASE("empty Json produces an empty object literal") {
    Json j;
    CHECK(j.str() == "{}");
}

TEST_CASE("kv chains preserve insertion order") {
    Json j;
    j.kv("c", 1).kv("a", 2).kv("b", 3);
    CHECK(j.str() == R"({"c":1,"a":2,"b":3})");
}

TEST_CASE("numeric types round-trip with full precision") {
    Json j;
    j.kv("int", static_cast<int>(42))
     .kv("int64", static_cast<int64_t>(9'000'000'000ll))
     .kv("uint64", static_cast<uint64_t>(18'000'000'000ull))
     .kv("double", 3.141592653589793);
    const auto s = j.str();
    // We're not parsing — just check the literals appear and the double
    // carries enough digits to be unambiguous.
    CHECK(s.find(R"("int":42)") != std::string::npos);
    CHECK(s.find(R"("int64":9000000000)") != std::string::npos);
    CHECK(s.find(R"("uint64":18000000000)") != std::string::npos);
    CHECK(s.find(R"("double":3.14159265358979)") != std::string::npos);
}

TEST_CASE("string values escape JSON-significant characters") {
    Json j;
    j.kv("plain", "hello")
     .kv("quote", "with \"quotes\"")
     .kv("backslash", "a\\b")
     .kv("newline", "line1\nline2");
    const auto s = j.str();
    CHECK(s.find(R"("plain":"hello")") != std::string::npos);
    CHECK(s.find(R"("quote":"with \"quotes\"")") != std::string::npos);
    CHECK(s.find(R"("backslash":"a\\b")") != std::string::npos);
    CHECK(s.find(R"("newline":"line1\nline2")") != std::string::npos);
}

TEST_CASE("nested Json embeds as a child object") {
    Json inner;
    inner.kv("x", 1).kv("y", 2);
    Json outer;
    outer.kv("point", inner).kv("label", "p");
    CHECK(outer.str() == R"({"point":{"x":1,"y":2},"label":"p"})");
}

TEST_CASE("vector<double> serialises as a JSON array") {
    Json j;
    j.kv("xs", std::vector<double>{1.0, 2.5, -3.0});
    CHECK(j.str().find(R"("xs":[1,2.5,-3])") != std::string::npos);
}

TEST_CASE("vector<string> serialises with each element quoted+escaped") {
    Json j;
    j.kv("labels", std::vector<std::string>{"a", "b\"c", "d"});
    CHECK(j.str() == R"({"labels":["a","b\"c","d"]})");
}

TEST_CASE("vector<Json> serialises as an array of objects") {
    Json a; a.kv("n", 1);
    Json b; b.kv("n", 2);
    Json outer;
    outer.kv("entries", std::vector<Json>{a, b});
    CHECK(outer.str() == R"({"entries":[{"n":1},{"n":2}]})");
}

TEST_CASE("bool values emit literal true / false (not 0 / 1)") {
    Json j;
    j.kv("t", true).kv("f", false);
    CHECK(j.str() == R"({"t":true,"f":false})");
}

TEST_CASE("key names with quotes are escaped") {
    Json j;
    j.kv("with\"quote", 1);
    CHECK(j.str() == R"({"with\"quote":1})");
}

TEST_CASE("empty vector<double> emits an empty array, not 'null'") {
    Json j;
    j.kv("xs", std::vector<double>{});
    CHECK(j.str() == R"({"xs":[]})");
}
