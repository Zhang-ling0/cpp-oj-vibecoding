#!/usr/bin/env bash
# =============================================================================
# 单元测试薄封装 —— 真正的构建规则在 CMakeLists.txt / tests/CMakeLists.txt，
# 真正的执行在 ctest。本脚本只做三件事：配置 → 编译 → 转调 ctest。
#
#   tests/run_unit_tests.sh            跑全部单测
#   tests/run_unit_tests.sh Admin      只跑名字匹配 Admin 的用例
#
# 与 tests/run_tests.sh 的分工：那里是起服务 + curl + SQL 的端到端验收（§9 T5.3）。
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake -S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_CXX_COMPILER="${CXX:-g++-12}" >/dev/null
fi
cmake --build "${BUILD_DIR}" -j >/dev/null

if [[ -n "${1:-}" ]]; then
  # --no-tests=error：过滤串写错时 ctest 默认返回 0（静默"全过"），必须显式改成报错
  exec ctest --test-dir "${BUILD_DIR}" --no-tests=error --output-on-failure -R "$1"
fi
exec ctest --test-dir "${BUILD_DIR}" --output-on-failure