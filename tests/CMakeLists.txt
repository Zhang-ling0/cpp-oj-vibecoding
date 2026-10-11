# =============================================================================
# 单元测试（gtest）—— 与 tests/run_tests.sh 的分工：
#   本目录：纯计算模块（config / generator / 编译器输出解析…）的 gtest 单测
#   run_tests.sh：起服务 + curl + SQL 的端到端验收（SPEC §9 T5.3）
#
# 用 GTestConfig.cmake（apt 的 libgtest-dev 1.14 自带），退回 CMake 的 FindGTest 模块
# 以兼容自建 gtest 的环境。
# =============================================================================
include(GoogleTest)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  find_package(GTest REQUIRED)   # 模块模式
endif()

# 测试文件用 GLOB + CONFIGURE_DEPENDS：新增 *_test.cpp 即自动成为 target。
# 这里与根 CMakeLists 的显式列表刻意相反 —— 测试是叶子，加错文件的风险为零，
# 而"新测试忘了登记"是更常见也更无味的漏测。
file(GLOB OJ_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

if(NOT OJ_TEST_SOURCES)
  message(WARNING "tests/ 下没有 *_test.cpp，将不生成任何测试目标")
endif()

foreach(test_source IN LISTS OJ_TEST_SOURCES)
  get_filename_component(test_name "${test_source}" NAME_WE)

  add_executable(${test_name} "${test_source}")
  target_link_libraries(${test_name} PRIVATE ojcore GTest::gtest GTest::gtest_main)

  # 把每个 TEST_F 注册成一条 ctest 用例：`ctest -N` 能逐条列出，
  # `ctest -R Admin` 能按名字过滤，失败时输出该用例的完整日志。
  gtest_discover_tests(${test_name}
    DISCOVERY_TIMEOUT 60
    PROPERTIES TIMEOUT 120
  )
endforeach()