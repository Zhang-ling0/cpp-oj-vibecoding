# =============================================================================
# CppOJ 构建入口（SPEC.md §9 T0.1 / §6.7.1）
#
# 【为什么是 CMake 而不是手写 Makefile】
#   SPEC v1.7 曾把 cmake 列入「明确不装」，理由是"两个并存只会让人不知用哪个"。
#   v1.8 起改用本文件作为唯一构建规则，Makefile 降为 15 行的薄封装（只转发
#   cmake --build / ctest），"并存"的困惑不复存在，该理由随之作废。
#
# 【源文件用显式列表而非 GLOB —— 刻意选择】
#   本项目有 20+ 个模块，每加一个 .cpp 都要在这里加一行。换成 GLOB 可以省这一步，
#   但代价是【漏加时不报错、代码根本不会被编译】—— 对一个正在按 SPEC 逐条落地的
#   项目，"响亮地失败"比"安静地少编译"重要。空 .cpp（尚未实现的模块）是合法翻译单元，
#   不会报错。
#
# 【编译器】判题侧的 g++-12 是 worker spawn 出来的子进程（§5.1），与本文件编译应用本体
#   的编译器是两件事：这里只对「不是 GNU」或「低于 g++-9」硬失败，对「不是 12」告警。
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(oj VERSION 0.1.0 LANGUAGES CXX)

# ---- 语言标准与全局选项 ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)          # D-13：C++17
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # 默认带调试符号的优化版：§10.3 的判题问题排查依赖可调试的 release
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

add_compile_options(-Wall -Wextra)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(WARNING
    "当前编译器是 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}；"
    "本项目按 Ubuntu 24.04 + g++-12 验证，非 GNU 编译器未经验证")
elseif(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  message(FATAL_ERROR "需要 g++ 9 或更高（实测锁定 12），当前 ${CMAKE_CXX_COMPILER_VERSION}")
elseif(NOT CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
  message(WARNING
    "当前 g++ 是 ${CMAKE_CXX_COMPILER_VERSION}，本项目在 g++-12 上验证（dependence.md §1）。"
    "应用本体可照常编译；但请注意判题侧 spawn 的 g++-12 是另一条独立约束（D-13），"
    "不受本编译器影响。用 make build 可自动锁定 g++-12")
endif()

# ---- 第三方依赖（全部来自 apt，见 dependence.md §1）---------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)              # D-75：密码哈希用 Blowfish（libcrypto）
find_package(nlohmann_json 3.11.3 REQUIRED) # T1.1 的 JSON 解析

# MySQL C Connector：apt 的 libmysqlclient-dev（/usr/include/mysql/mysql.h）。
# 不 REQUIRED —— 单测不碰 DB，缺它时先告警，等 dao 真正 include <mysql/mysql.h> 时
# 才会以"找不到头文件"的形式硬失败。
find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h)
find_library(MYSQLCLIENT_LIBRARY NAMES mysqlclient)
if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQLCLIENT_LIBRARY)
  message(WARNING
    "未找到 MySQL C Connector（libmysqlclient-dev），T3.1 之后需要补装："
    "sudo apt-get install -y libmysqlclient-dev")
endif()

# cpp-httplib（[D-72]，v1.9 修订）：版本真源是本机自装的 header-only 0.60.1，
# 落在 /usr/local/include/httplib.h —— 不是 apt 的 libcpp-httplib-dev（0.14.3）。
# 两处 include 写法同为 <httplib.h>，所以源码零差异（§6.2 尾注）。
find_path(HTTPLIB_INCLUDE_DIR NAMES httplib.h)
if(HTTPLIB_INCLUDE_DIR)
  set(OJ_HTTPLIB_INCLUDE "${HTTPLIB_INCLUDE_DIR}")
  # 把命中的路径与版本一并打出来：误装 apt 的 0.14.3 会命中 /usr/include 而静默用了旧版，
  # 这条 STATUS 就是为了让它可见（[D-72] 要消除的版本漂移，换了个形式仍在）
  file(STRINGS "${OJ_HTTPLIB_INCLUDE}/httplib.h" OJ_HTTPLIB_VERSION_LINE
       REGEX "^#define[ \t]+CPPHTTPLIB_VERSION[ \t]+\"")
  string(REGEX REPLACE "^#define[ \t]+CPPHTTPLIB_VERSION[ \t]+\"(.*)\"$" "\\1"
         OJ_HTTPLIB_VERSION "${OJ_HTTPLIB_VERSION_LINE}")
  message(STATUS "httplib：${OJ_HTTPLIB_INCLUDE}（版本 ${OJ_HTTPLIB_VERSION}，应为 0.60.1）")
  if(NOT OJ_HTTPLIB_VERSION VERSION_EQUAL "0.60.1")
    message(WARNING
      "命中的 httplib 版本是 ${OJ_HTTPLIB_VERSION}，与 [D-72] v1.9 修订的真源 0.60.1 不一致。"
      "常见原因：误装了 libcpp-httplib-dev（0.14.3，落在 /usr/include）。"
      "处置见 SPEC §6.7.1 —— 版本漂移正是这条决策要消除的东西")
  endif()
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib/httplib.h")
  set(OJ_HTTPLIB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib")
  message(WARNING
    "未找到 /usr/local/include/httplib.h（0.60.1），回退到仓库内 vendored 的 "
    "third_party/httplib —— 它是 0.14.3，与真源版本不一致，应视为【环境未就绪】。"
    "源码无需改动（两边都是 #include <httplib.h>），但 T0.5 接线前应按 §6.7.2 装上 0.60.1")
else()
  message(FATAL_ERROR
    "既找不到 /usr/local/include/httplib.h，也没有 third_party/httplib/httplib.h；"
    "请按 SPEC §6.7.2 安装 0.60.1："
    "sudo install -m 644 <httplib.h> /usr/local/include/httplib.h")
endif()

# ---- 核心静态库：除 main.cpp 外的全部模块 --------------------------------------
set(OJCORE_SOURCES
    src/common/config.cpp
    src/common/errors.cpp
    src/common/log.cpp
    src/common/response.cpp
    src/db/dao.cpp
    src/db/mysql_pool.cpp
    src/judge/compiler.cpp
    src/judge/generator.cpp
    src/judge/queue.cpp
    src/judge/runner.cpp
    src/judge/scheduler.cpp
    src/judge/worker.cpp
    src/model/problem.cpp
    src/model/submission.cpp
    src/model/user.cpp
    src/problem/json_schema.cpp
    src/problem/loader.cpp
    src/web/admin.cpp
    src/web/ratelimit.cpp
    src/web/routes.cpp
    src/web/session.cpp
)

add_library(ojcore STATIC ${OJCORE_SOURCES})
target_include_directories(ojcore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${OJ_HTTPLIB_INCLUDE}"
)
target_link_libraries(ojcore PUBLIC
    Threads::Threads
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
)
if(MYSQL_INCLUDE_DIR AND MYSQLCLIENT_LIBRARY)
  target_include_directories(ojcore PUBLIC "${MYSQL_INCLUDE_DIR}")
  target_link_libraries(ojcore PUBLIC "${MYSQLCLIENT_LIBRARY}")
endif()

# ---- 可执行文件 ----------------------------------------------------------------
add_executable(oj src/main.cpp)
target_link_libraries(oj PRIVATE ojcore)

# ---- 测试 ----------------------------------------------------------------------
option(OJ_BUILD_TESTS "构建 gtest 单元测试" ON)
if(OJ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()