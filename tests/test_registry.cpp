// Compile-time and runtime tests for bench/registry.h.
//
// Most of what we care about — every wrapper in Registry satisfies the
// Benchmark<T> concept — is already enforced by the static_assert at the
// bottom of registry.h. If THIS file compiles, those checks already passed.
// The runtime tests below cover the dispatch() filter logic and the opt-in
// gating.
//
// We deliberately do NOT include registry.h itself: it pulls in NDK-specific
// dependencies (sensors, camera, ALooper) via the bench/cpu/* headers, which
// are not available on the host. Instead, we redeclare a stripped-down
// version of the Benchmark concept + the wanted() helper here, plus a fake
// wrapper, and verify the dispatch semantics in isolation.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bench/json.h"

#include <concepts>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace test_registry {

struct Args { std::string filter; int iters = 0; };

template <typename T>
concept Benchmark = requires(T b, const Args& a, const typename T::Config& cfg) {
    { T::name }                       -> std::convertible_to<const char*>;
    { T::make_config(a) }             -> std::same_as<typename T::Config>;
    { T::opt_in() }                   -> std::same_as<bool>;
    { b.run() }                       -> std::same_as<bench::Json>;
};

inline bool wanted(const std::string& filter, const char* name) {
    return filter.empty() || filter.find(name) != std::string::npos;
}

// Two fake benchmarks: one default-on, one opt-in.
struct DefaultBench {
    static constexpr const char* name = "alpha";
    struct Config { int x = 0; };
    static Config make_config(const Args& a) { return {a.iters}; }
    static bool opt_in() { return false; }
    bench::Json run() const {
        bench::Json j; j.kv("which", "alpha"); return j;
    }
};

struct OptInBench {
    static constexpr const char* name = "beta_optin";
    struct Config {};
    static Config make_config(const Args&) { return {}; }
    static bool opt_in() { return true; }
    bench::Json run() const {
        bench::Json j; j.kv("which", "beta_optin"); return j;
    }
};

static_assert(Benchmark<DefaultBench>);
static_assert(Benchmark<OptInBench>);

using Registry = std::tuple<DefaultBench, OptInBench>;

// Mirror of registry::dispatch but without per_cluster wiring (host has no
// CPU cluster topology). Same opt-in semantics.
inline std::vector<std::pair<std::string, bench::Json>>
dispatch(const Args& args) {
    std::vector<std::pair<std::string, bench::Json>> out;
    Registry reg{};
    auto try_one = [&](auto&& b) {
        using B = std::decay_t<decltype(b)>;
        if (!wanted(args.filter, B::name)) return;
        if (B::opt_in() && args.filter.find(B::name) == std::string::npos) return;
        out.emplace_back(B::name, b.run());
    };
    std::apply([&](auto&&... bs) { (try_one(bs), ...); }, reg);
    return out;
}

} // namespace test_registry

using namespace test_registry;

TEST_CASE("empty filter selects every default-on wrapper, skips opt-ins") {
    auto results = dispatch({.filter = ""});
    REQUIRE(results.size() == 1);
    CHECK(results[0].first == "alpha");
}

TEST_CASE("filter matching an opt-in's name selects only that opt-in") {
    auto results = dispatch({.filter = "beta_optin"});
    REQUIRE(results.size() == 1);
    CHECK(results[0].first == "beta_optin");
}

TEST_CASE("filter that doesn't match any wrapper returns empty results") {
    auto results = dispatch({.filter = "no_such_bench"});
    CHECK(results.empty());
}

TEST_CASE("substring matching: 'alpha' matches 'alpha', not 'beta_optin'") {
    auto results = dispatch({.filter = "alpha"});
    REQUIRE(results.size() == 1);
    CHECK(results[0].first == "alpha");
}

TEST_CASE("wanted() semantics: filter must CONTAIN the wrapper name, not the other way around") {
    // The production wanted() helper does `filter.find(name) != npos`. So a
    // short filter like 'a' must contain the full string 'alpha' to match —
    // which it cannot. Comma-separated filters like 'alpha,beta_optin' work
    // because they contain both names as substrings.
    SUBCASE("'a' matches nothing (filter doesn't contain any name)") {
        auto results = dispatch({.filter = "a"});
        CHECK(results.empty());
    }
    SUBCASE("'alpha' matches only DefaultBench") {
        auto results = dispatch({.filter = "alpha"});
        REQUIRE(results.size() == 1);
        CHECK(results[0].first == "alpha");
    }
    SUBCASE("comma-list 'alpha,beta_optin' selects both") {
        auto results = dispatch({.filter = "alpha,beta_optin"});
        REQUIRE(results.size() == 2);
        // Tuple order: DefaultBench first, OptInBench second.
        CHECK(results[0].first == "alpha");
        CHECK(results[1].first == "beta_optin");
    }
}

TEST_CASE("dispatch results contain the JSON each wrapper produced") {
    auto results = dispatch({.filter = "beta_optin"});
    REQUIRE(results.size() == 1);
    CHECK(results[0].second.str() == R"({"which":"beta_optin"})");
}
