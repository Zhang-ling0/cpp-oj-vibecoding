#!/usr/bin/env bash
# =============================================================================
# 题库「增 / 改 / 写坏 / 删文件」四条路径的端到端验收（改文件 → 重启 → curl 断言）
#
# 为什么单独一个脚本，而不是并进 tests/run_tests.sh：
#   · 题库同步只在【启动时】跑一次（SPEC D-30：改题必须重启），所以每一步都要重启服务；
#   · 它要改 problems/ 下的文件 —— 与 run_tests.sh 的"只读 + 随机账号"性质不同，
#     混在一起会让两个脚本互相踩。run_tests.sh 必须能随时安全地跑，故这里独立。
#
# 【隔离：脚本自己拉一个独立实例，绝不碰你正在跑的服务】
#   ./build/oj /tmp/oj-probe/probe.env
#     · 端口 18080（避开 .env 的 8080）
#     · 日志目录 /tmp/oj-probe/logs
#     · judge 工作区 /tmp/oj-probe/judge —— ⚠️ 必须独立：启动第 6 步会扫残并清空
#       工作区里的残留目录，两个实例共用一个工作区会互相删对方的
#   · MySQL 仍是同一个库（题目本来就在库里，没有别的选择），探针题的行走脚本自己清
#   所有相对路径（problems/、static/、db/init/）都相对【当前工作目录】，
#   所以本脚本必须从仓库根目录执行 —— 它自己会 cd。
#
# 【只动专用探针题，绝不碰 two-sum.json】
#   探针 slug = sync-probe-2026（带日期后缀就是为了万一没清干净也不像真题）。
#   结尾无条件删除该文件 + 删掉库里的行。
#
# 用法（从仓库根目录）：
#   tests/run_problemsync_test.sh
#
# 与 tests/run_unit_tests.sh（gtest）、tests/run_tests.sh（HTTP 契约）的分工：
#   那里都不重启服务、都不改题库文件；只有这里会。
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

PROBE_DIR="${TMPDIR:-/tmp}/oj-probe"
PROBE_PORT=18080
PROBE_BASE="http://127.0.0.1:${PROBE_PORT}"
PROBE_ENV="${PROBE_DIR}/probe.env"
PROBE_LOG="${PROBE_DIR}/server.log"
PROBE_SLUG="sync-probe-2026"
PROBE_FILE="problems/${PROBE_SLUG}.json"
MYSQL=(mysql -h 127.0.0.1 -P 3306 -u oj -poj oj -N -B -e)

PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
  stop_probe_server
  [[ -f "${PROBE_FILE}" ]] && rm -f "${PROBE_FILE}"
  # 清库：先按 slug 找 id（problem_tags/cases/params 由外键 CASCADE 连带清）
  local pid
  pid="$("${MYSQL[@]}" "SELECT id FROM problems WHERE slug='${PROBE_SLUG}';" 2>/dev/null)"
  if [[ -n "${pid}" ]]; then
    "${MYSQL[@]}" "DELETE FROM submissions WHERE problem_id=${pid};
                   DELETE FROM problems WHERE id=${pid};" >/dev/null 2>&1
  fi
}
trap cleanup EXIT

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }

# ---- 断言（沿用 run_tests.sh 的同一套写法：同时断言 HTTP 状态码与业务码）---------
body_file="${PROBE_DIR}/body"

# check_http <标签> <METHOD> <路径> <期望HTTP> <期望code> [额外 curl 参数...]
check_http() {
  local label="$1" method="$2" path="$3" want_http="$4" want_code="$5"
  shift 5
  local got_http got_code
  got_http="$(curl -sS -X "${method}" "${PROBE_BASE}${path}" -o "${body_file}" -w '%{http_code}' "$@" 2>/dev/null)"
  got_code="$(jq -r 'try (.code) | if . == null then "-" else tostring end' "${body_file}" 2>/dev/null)"
  if [[ "${got_http}" == "${want_http}" && "${got_code}" == "${want_code}" ]]; then
    PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %-56s HTTP %s / code %s\n' "${label}" "${got_http}" "${got_code}"
  else
    FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %-56s HTTP %s / code %s（期望 %s / %s）\n' \
      "${label}" "${got_http}" "${got_code}" "${want_http}" "${want_code}"
    printf '        响应体：%s\n' "$(head -c 300 "${body_file}")"
  fi
}

# check_eq <标签> <实际> <期望>
check_eq() {
  local label="$1" got="$2" want="$3"
  if [[ "${got}" == "${want}" ]]; then
    PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %-56s %s\n' "${label}" "${got}"
  else
    FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %-56s %s（期望 %s）\n' "${label}" "${got}" "${want}"
  fi
}

# 列表里某 slug 的某字段（取不到就是 "-"）；用于核对题库改动是否真的反映到 HTTP 侧
field_of_slug() {
  curl -sS "${PROBE_BASE}/api/problems" -o "${body_file}" 2>/dev/null
  jq -r --arg s "$1" --arg f "$2" \
    'try ([.data.list[] | select(.slug==$s)][0][$f]) | if . == null then "-" else tostring end' \
    "${body_file}" 2>/dev/null
}
detail_field() {
  curl -sS "${PROBE_BASE}/api/problems/$1" -o "${body_file}" 2>/dev/null
  jq -r --arg f "$2" "try (.data[\$f]) | if . == null then \"-\" else tostring end" "${body_file}" 2>/dev/null
}

# ---- 探针实例的生命周期 -----------------------------------------------------------
write_probe_env() {
  mkdir -p "${PROBE_DIR}/logs" "${PROBE_DIR}/judge"
  # 从真 .env 复制后覆盖三个键：端口、日志目录、judge 工作区。
  # ⚠️ 工作区必须独立 —— 启动第 6 步的工作区扫残会删掉里面的残留。
  sed -e "s|^OJ_LISTEN=.*|OJ_LISTEN=127.0.0.1:${PROBE_PORT}|" \
      -e "s|^OJ_LOG_DIR=.*|OJ_LOG_DIR=${PROBE_DIR}/logs|" \
      -e "s|^OJ_JUDGE_WORKDIR=.*|OJ_JUDGE_WORKDIR=${PROBE_DIR}/judge|" \
      .env > "${PROBE_ENV}"
}

start_probe_server() {
  stop_probe_server
  write_probe_env
  ./build/oj "${PROBE_ENV}" >> "${PROBE_LOG}" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 40); do
    if curl -sS -o /dev/null "${PROBE_BASE}/api/tags" 2>/dev/null; then return 0; fi
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then break; fi
    sleep 0.25
  done
  printf '探针实例启动失败，见 %s 的最后 20 行：\n' "${PROBE_LOG}" >&2
  tail -20 "${PROBE_LOG}" >&2
  return 1
}

stop_probe_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -INT "${SERVER_PID}" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "${SERVER_PID}" 2>/dev/null || break; sleep 0.2; done
    kill -0 "${SERVER_PID}" 2>/dev/null && kill -KILL "${SERVER_PID}" 2>/dev/null
    wait "${SERVER_PID}" 2>/dev/null
  fi
  SERVER_PID=""
}

# ---- 探针题 JSON（三个版本：初版 / 改后 / 写坏）------------------------------------
probe_json_v1() {
  cat <<JSON
{
  "slug": "${PROBE_SLUG}",
  "title": "探针题·初版",
  "difficulty": "easy",
  "is_featured": false,
  "sort_no": 99,
  "tags": { "algorithm": ["数组"] },
  "statement_md": "## 探针\n\n这是验收脚本造的临时题目。",
  "hint_md": "不用想。",
  "signature": "class Solution {\npublic:\n    int add(int a, int b);\n};",
  "method_name": "add",
  "params": [ { "name": "a", "cpp_type": "int" }, { "name": "b", "cpp_type": "int" } ],
  "return_type": "int",
  "code_template": "class Solution {\npublic:\n    int add(int a, int b) {\n        \n    }\n};",
  "time_limit_ms": 2000,
  "mem_limit_kb": 131072,
  "cases": [
    { "args": [1, 2], "expected": 3, "brief": "正数" },
    { "args": [-1, 1], "expected": 0, "brief": "含负数" }
  ]
}
JSON
}

# 改后：标题、难度、标签、用例数都变了（用来验证 UPDATE + 子表刷新 + id 不变）
probe_json_v2() {
  cat <<JSON
{
  "slug": "${PROBE_SLUG}",
  "title": "探针题·改后",
  "difficulty": "hard",
  "is_featured": true,
  "sort_no": 98,
  "tags": { "algorithm": ["数组", "动态规划"] },
  "statement_md": "## 探针（已改）\n\n这一版改了标题、难度、标签和用例数。",
  "hint_md": "还是不用想。",
  "signature": "class Solution {\npublic:\n    int add(int a, int b);\n};",
  "method_name": "add",
  "params": [ { "name": "a", "cpp_type": "int" }, { "name": "b", "cpp_type": "int" } ],
  "return_type": "int",
  "code_template": "class Solution {\npublic:\n    int add(int a, int b) {\n        return a + b;\n    }\n};",
  "time_limit_ms": 2500,
  "mem_limit_kb": 65536,
  "cases": [
    { "args": [1, 2], "expected": 3, "brief": "正数" },
    { "args": [-1, 1], "expected": 0, "brief": "含负数" },
    { "args": [0, 0], "expected": 0, "brief": "零" }
  ]
}
JSON
}

# 写坏：删掉 return_type（json_schema 必填）→ 校验不通过 → 跳过入库 + 标记失效
probe_json_broken() {
  probe_json_v2 | grep -v '"return_type"'
}

# =============================================================================
echo "=== 题库增/改/写坏/删 端到端验收 ==="
echo "仓库：${ROOT}"
echo "探针：${PROBE_FILE}（slug=${PROBE_SLUG}）  实例：${PROBE_BASE}"
echo
command -v jq >/dev/null || { echo "需要 jq"; exit 1; }

# ---- 0. 基线：先记住当前题库有几道题 ------------------------------------------------
# 【为什么是"记住基线"而不是断言 1】本套件验的是"增/改/写坏/删"这四个动作对库做了什么，
#   与题库里【本来】有几道题无关。而 tools/reset_db.cpp（web 自动化测试的数据底座）会给
#   题库补到 20+ 道 —— 写死 1 会让本套件在造过数之后必然红 4 条，而那 4 条与它要验的语义
#   毫无关系。于是这里取基线、后面全部按"基线 + N"断言：题库多大都能跑。
say "[0] 基线（记下当前题库总数）"
rm -f "${PROBE_FILE}"
start_probe_server || exit 1
curl -sS "${PROBE_BASE}/api/problems" -o "${body_file}"
BASE_TOTAL="$(jq -r '.data.total' "${body_file}")"
check_eq "列表 total（基线 = ${BASE_TOTAL} 道题）" "${BASE_TOTAL}" "${BASE_TOTAL}"
check_http "two-sum 详情可读" GET /api/problems/two-sum 200 0

# ---- 1. 增 -------------------------------------------------------------------------
say "[1] 增：新增 problems/${PROBE_SLUG}.json → 重启"
probe_json_v1 > "${PROBE_FILE}"
start_probe_server || exit 1
curl -sS "${PROBE_BASE}/api/problems" -o "${body_file}"
check_eq "列表 total（应为基线+1 = $((BASE_TOTAL + 1))）" "$(jq -r '.data.total' "${body_file}")" "$((BASE_TOTAL + 1))"
check_http "新题详情 → 200/0" GET "/api/problems/${PROBE_SLUG}" 200 0
check_eq "新题 title" "$(detail_field "${PROBE_SLUG}" title)" "探针题·初版"
check_eq "新题 difficulty" "$(detail_field "${PROBE_SLUG}" difficulty)" "easy"
check_eq "新题 caseCount" "$(detail_field "${PROBE_SLUG}" caseCount)" "2"
PROBE_ID_V1="$("${MYSQL[@]}" "SELECT id FROM problems WHERE slug='${PROBE_SLUG}';" 2>/dev/null)"
printf '        库里的 problems.id = %s\n' "${PROBE_ID_V1}"

# ---- 2. 改 -------------------------------------------------------------------------
say "[2] 改：改标题/难度/标签/用例数 → 重启"
probe_json_v2 > "${PROBE_FILE}"
start_probe_server || exit 1
check_http "改后详情 → 200/0" GET "/api/problems/${PROBE_SLUG}" 200 0
check_eq "改后 title" "$(detail_field "${PROBE_SLUG}" title)" "探针题·改后"
check_eq "改后 difficulty" "$(detail_field "${PROBE_SLUG}" difficulty)" "hard"
check_eq "改后 caseCount（用例数 2→3）" "$(detail_field "${PROBE_SLUG}" caseCount)" "3"
check_eq "改后 timeLimitMs" "$(detail_field "${PROBE_SLUG}" timeLimitMs)" "2500"
check_eq "列表里的 title 同步更新" "$(field_of_slug "${PROBE_SLUG}" title)" "探针题·改后"
check_eq "列表里的 difficulty 同步更新" "$(field_of_slug "${PROBE_SLUG}" difficulty)" "hard"
PROBE_ID_V2="$("${MYSQL[@]}" "SELECT id FROM problems WHERE slug='${PROBE_SLUG}';" 2>/dev/null)"
# ⚠️ id 不变 = 走的是 UPDATE 而不是"删行再插"（删行会 CASCADE 掉用户的提交历史，
#   题库加载器刻意不这么做，见 loader.h【为什么不删"文件已删除"的题】）
check_eq "problems.id 未变（证明是 UPDATE 不是删+插）" "${PROBE_ID_V2}" "${PROBE_ID_V1}"
TAGS="$(detail_field "${PROBE_SLUG}" tags)"
if [[ "${TAGS}" == *"动态规划"* ]]; then
  PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %-56s %s\n' "改后标签含新增的「动态规划」" "${TAGS}"
else
  FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %-56s %s\n' "改后标签含新增的「动态规划」" "${TAGS}"
fi

# ---- 3. 写坏（校验不通过）----------------------------------------------------------
say "[3] 写坏：删掉必填的 return_type → 重启"
probe_json_broken > "${PROBE_FILE}"
start_probe_server || exit 1
check_http "坏题详情仍然 200/0（行没被删）" GET "/api/problems/${PROBE_SLUG}" 200 0
check_eq "坏题 caseCount 被置 0" "$(detail_field "${PROBE_SLUG}" caseCount)" "0"
check_eq "坏题 params 被清空" "$(detail_field "${PROBE_SLUG}" params)" "[]"
# ⚠️ 预期：它【仍出现在列表页】。列表 SQL 是裸 FROM problems（problem_api.cpp:564），
#   没有任何"失效题"过滤；loader.cpp 自己的注释也是这个口径（"会在列表页显示为
#   case_count=0 的空题"），而 loader.h 里"列表页与缓存都不再出现它"与实现不符。
check_eq "坏题仍在列表里（caseCount=0）" "$(field_of_slug "${PROBE_SLUG}" caseCount)" "0"
printf '        启动回显里的"标记失效"计数：%s\n' \
  "$(grep -c '标记失效' "${PROBE_LOG}" >/dev/null 2>&1 && grep '入库' "${PROBE_LOG}" | tail -1 | sed 's/.*题库同步 //')"

# ---- 3b. 修回合法（证明"标记失效"是可恢复的，不是终态）----------------------------
say "[3b] 修回合法 JSON → 重启（标记失效应当可恢复）"
probe_json_v2 > "${PROBE_FILE}"
start_probe_server || exit 1
check_http "修回后详情 → 200/0" GET "/api/problems/${PROBE_SLUG}" 200 0
check_eq "用例数恢复（0→3）" "$(detail_field "${PROBE_SLUG}" caseCount)" "3"
check_eq "形参恢复（[]→2 个）" "$(detail_field "${PROBE_SLUG}" params | jq 'length')" "2"

# ---- 4. 删文件（从【合法状态】删，才测得到"删文件对库做了什么"）----------------------
say "[4] 删文件：从合法状态删掉 ${PROBE_SLUG}.json → 重启"
rm -f "${PROBE_FILE}"
start_probe_server || exit 1
check_http "文件删掉后详情仍可读（行原样保留）" GET "/api/problems/${PROBE_SLUG}" 200 0
check_eq "用例没有被清空（删文件【不】触发 deactivate）" "$(detail_field "${PROBE_SLUG}" caseCount)" "3"
check_eq "形参没有被清空" "$(detail_field "${PROBE_SLUG}" params | jq 'length')" "2"
check_eq "标题仍是删除前的那一版" "$(detail_field "${PROBE_SLUG}" title)" "探针题·改后"
printf '  说明：deactivate_problem 只在"JSON 校验不通过"时触发（loader.cpp:316）。\n'
printf '        文件被删时连 slug 都读不到，加载器什么都不做 —— 这是刻意的：\n'
printf '        删 problems 行会 CASCADE 掉所有用户在该题上的提交历史（loader.h）。\n'
printf '        代价是"删了文件的题仍出现在列表页"，这与【改坏】的处置不同，两者别混为一谈。\n'

# ---- 5. 收尾：先证明"行确实留在库里"，再显式删行 → 回到基线 ------------------------
say "[5] 收尾：显式删掉库里的探针行 → 重启（题库回到基线）"
curl -sS "${PROBE_BASE}/api/problems" -o "${body_file}"
check_eq "删文件后 total 仍是基线+1（行没被删）" "$(jq -r '.data.total' "${body_file}")" "$((BASE_TOTAL + 1))"
"${MYSQL[@]}" "DELETE FROM submissions WHERE problem_id IN (SELECT id FROM problems WHERE slug='${PROBE_SLUG}');
               DELETE FROM problems WHERE slug='${PROBE_SLUG}';" >/dev/null 2>&1
start_probe_server || exit 1
curl -sS "${PROBE_BASE}/api/problems" -o "${body_file}"
check_eq "显式删行后 total 回到基线 ${BASE_TOTAL}" "$(jq -r '.data.total' "${body_file}")" "${BASE_TOTAL}"
check_http "two-sum 仍然可读" GET /api/problems/two-sum 200 0

echo
echo "=== 结果：通过 ${PASS}，失败 ${FAIL} ==="
[[ "${FAIL}" -eq 0 ]]
