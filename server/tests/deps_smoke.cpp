// deps_smoke.cpp —— ep08 步骤 1：五个外部依赖的编译期可用性探针
// 编译：g++ -std=c++20 -I include tests/deps_smoke.cpp -o /tmp/deps_smoke && /tmp/deps_smoke
// 纪律：只用 __has_include 探测，不 #include 任何可选依赖——缺库也能编译 / 链接 / 运行。
// 语义：默认 lite（缺库不算失败，核心与单测零依赖照常构建，§5.7）；
//       定义 SIGHTLINE_REQUIRE_DEPS=1 时，缺任一库返回 1（装好依赖后开此宏做「全绿」验收）。
#include <cstdio>

#if defined(__has_include)
#  if __has_include(<lua.h>)
#    define SL_HAS_LUA 1
#  endif
#  if __has_include(<sol/sol.hpp>)
#    define SL_HAS_SOL2 1
#  endif
#  if __has_include(<hiredis/hiredis.h>)
#    define SL_HAS_HIREDIS 1
#  endif
#  if __has_include(<mariadb/conncpp.hpp>)
#    define SL_HAS_MARIADB 1
#  elif __has_include(<mysql/mysql.h>)
#    define SL_HAS_MARIADB 1
#  endif
#  if __has_include(<mongocxx/client.hpp>)
#    define SL_HAS_MONGOCXX 1
#  endif
#endif

#ifndef SL_HAS_LUA
#  define SL_HAS_LUA 0
#endif
#ifndef SL_HAS_SOL2
#  define SL_HAS_SOL2 0
#endif
#ifndef SL_HAS_HIREDIS
#  define SL_HAS_HIREDIS 0
#endif
#ifndef SL_HAS_MARIADB
#  define SL_HAS_MARIADB 0
#endif
#ifndef SL_HAS_MONGOCXX
#  define SL_HAS_MONGOCXX 0
#endif

namespace {
struct Probe {
    const char* name;
    int present;
    const char* hint;
};
} // namespace

int main() {
    static const Probe probes[] = {
        {"lua5.4",      SL_HAS_LUA,      "liblua5.4-dev / lua-devel"},
        {"sol2",        SL_HAS_SOL2,     "header-only（sol/sol.hpp）"},
        {"hiredis",     SL_HAS_HIREDIS,  "libhiredis-dev"},
        {"mariadb-cpp", SL_HAS_MARIADB,  "libmariadbcpp / libmysqlclient-dev"},
        {"mongocxx",    SL_HAS_MONGOCXX, "libmongocxx-dev（需 mongo-c-driver）"},
    };

    int missing = 0;
    std::printf("=== ep08 deps_smoke ===\n");
    for (const auto& p : probes) {
        std::printf("%-14s %s   %s\n", p.name, p.present ? "OK" : "--", p.hint);
        if (!p.present) ++missing;
    }

#if defined(SIGHTLINE_REQUIRE_DEPS)
    if (missing > 0) {
        std::printf("FAIL REQUIRE_DEPS=1: missing %d, full profile not buildable\n", missing);
        return 1;
    }
    std::printf("PASS REQUIRE_DEPS=1: all five present, full profile buildable\n");
#else
    std::printf("[lite] optional deps missing %d - core/usecases/tests stay dependency-free\n", missing);
#endif
    return 0;
}
