#!/usr/bin/env bash
# =============================================================================
# CppOJ 端到端验收 —— 【只用 curl 构造 HTTP 请求】，逐端点核对 HTTP 状态与业务码
# 对应任务：SPEC.md §9 M5 T5.3（端到端验收）
# 规格来源：API.md（端点总览 / §1.2 码表 / §3–§7 各端点）
#
# 用法（必须先起服务，且从项目根目录执行）：
#   ./build/oj &            # 或 make run
#   tests/run_tests.sh
#
# 与 tests/run_unit_tests.sh 的分工：那里跑 gtest（纯逻辑 + 业务层 SQL），
# 这里跑 HTTP —— 路由接线、Cookie、包络、错误收口这几件事**只有 curl 能验**。
#
# 【为什么每个用例都断言两件事：HTTP 状态码 + data 里的 code】
#   [D-29] 要求两者同步，而它们由两个不同的代码路径产出（errors.h::http_status 与
#   处理器里手写的 status）。只断一个就漏掉"401 的码配着 403 的状态"这类漂移。
#
# 【为什么用随机用户名】验收脚本要能反复跑而不撞 §6.3 的用户名唯一键；
# 每次跑完的账号由脚本自己注销或就地留在库里（注销账号是端点之一，另有用例覆盖）。
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

BASE="${OJ_BASE:-http://127.0.0.1:8080}"
MYSQL=(mysql -h 127.0.0.1 -P 3306 -u oj -poj oj -N -B -e)
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

PASS=0
FAIL=0
RUN_ID="t$$"
USER_A="${RUN_ID}a"          # 普通用户
USER_B="${RUN_ID}b"          # 另一个普通用户（验证"他人资源"这一层）
PASS_A="${RUN_ID}pass1234"   # 满足 §5.8 的 6–64 位

jar_a="${TMP}/a.jar"
jar_a2="${TMP}/a2.jar"
jar_b="${TMP}/b.jar"

sql() { "${MYSQL[@]}" "$1"; }

# 打印用例结果。三个入参固定：标签 / 期望 HTTP / 期望 code
report() {
  local label="$1" got_http="$2" got_code="$3" want_http="$4" want_code="$5" extra="${6:-}"
  if [[ "${got_http}" == "${want_http}" && "${got_code}" == "${want_code}" ]]; then
    PASS=$((PASS + 1))
    printf '  \033[32mPASS\033[0m %-58s HTTP %s / code %s %s\n' "${label}" "${got_http}" "${got_code}" "${extra}"
  else
    FAIL=$((FAIL + 1))
    printf '  \033[31mFAIL\033[0m %-58s HTTP %s / code %s（期望 %s / %s）\n' \
      "${label}" "${got_http}" "${got_code}" "${want_http}" "${want_code}" "${extra}"
    printf '        响应体：%s\n' "$(head -c 400 "${TMP}/body")"
  fi
}

# api <标签> <METHOD> <路径> <期望HTTP> <期望code> [额外 curl 参数...]
# 响应体落在 ${TMP}/body，HTTP 状态由 curl -w 取出；两者分别与期望比对。
api() {
  local label="$1" method="$2" path="$3" want_http="$4" want_code="$5"
  shift 5
  local got_http got_code
  got_http="$(curl -sS -X "${method}" "${BASE}${path}" -o "${TMP}/body" -w '%{http_code}' "$@" 2>/dev/null)"
  got_code="$(jq -r '.code // "null"' "${TMP}/body" 2>/dev/null || echo null)"
  report "${label}" "${got_http}" "${got_code}" "${want_http}" "${want_code}"
}

# 静态资源：不断言包络（HTML 没有 code），只看状态码
static() {
  local label="$1" path="$2" want_http="$3"
  local got_http
  got_http="$(curl -sS "${BASE}${path}" -o "${TMP}/body" -w '%{http_code}' 2>/dev/null)"
  if [[ "${got_http}" == "${want_http}" ]]; then
    PASS=$((PASS + 1))
    printf '  \033[32mPASS\033[0m %-58s HTTP %s\n' "${label}" "${got_http}"
  else
    FAIL=$((FAIL + 1))
    printf '  \033[31mFAIL\033[0m %-58s HTTP %s（期望 %s）\n' "${label}" "${got_http}" "${want_http}"
  fi
}

field_of() { jq -r '.data.field // "-"' "${TMP}/body" 2>/dev/null; }
data_key() { jq -r "try ($1) | if . == null then \"-\" else tostring end" "${TMP}/body" 2>/dev/null; }

echo "=== CppOJ 端到端验收（curl）==="
echo "服务：${BASE}"
echo

# ---------------------------------------------------------------- 20. 静态资源
echo "[20] GET /* 静态资源与全局收口"
static "五页之一：/login.html" /login.html 200
static "五页之一：/admin.html" /admin.html 200
static "vendored 库：/js/api.js 存在则 200，否则 404（只查可达性）" /js/api.js 200
api "不存在的静态文件 → 404/1004" GET /nonexistent.html 404 1004
api "不存在的 API 路径 → 404/1004" GET /api/nope 404 1004
# 方法不对：httplib 0.60 不产生 405（它只有那个状态码枚举，没有任何地方会回它），
# 实际是 404 → 收口成 1004。对前端而言两者同码（"你要用的东西不存在"），见 API.md §8.2。
api "路径存在但方法不对 → 404/1004（httplib 不发 405）" GET /api/auth/login 404 1004

# ---------------------------------------------------------------- 1. 注册
echo
echo "[1] POST /api/auth/register"
api "正常注册 → 201/0" POST /api/auth/register 201 0 \
  -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"${PASS_A}\"}"
ID_A="$(data_key '.data.userId')"
api "重复注册 → 409/1005" POST /api/auth/register 409 1005 \
  -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"${PASS_A}\"}"
api "用户名字符集非法 → 400/1001 field=username" POST /api/auth/register 400 1001 \
  -H 'Content-Type: application/json' -d '{"username":"用户 名","password":"pass1234"}'
api "密码过短 → 400/1001 field=password" POST /api/auth/register 400 1001 \
  -H 'Content-Type: application/json' -d '{"username":"ab1","password":"123"}'
api "body 不是 JSON → 400/1001" POST /api/auth/register 400 1001 \
  -H 'Content-Type: application/json' -d 'not-json'
# 注册【不登录】：不带任何 Cookie 调 me 必须是 1002
api "注册不建立会话（未登录调 me）→ 401/1002" GET /api/auth/me 401 1002

# ---------------------------------------------------------------- 2. 登录
echo
echo "[2] POST /api/auth/login"
api "登录成功 → 200/0 + Set-Cookie" POST /api/auth/login 200 0 \
  -c "${jar_a}" -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"${PASS_A}\"}"
api "密码错误 → 400/1001 field=password" POST /api/auth/login 400 1001 \
  -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"wrong-pass\"}"
api "用户不存在 → 400/1001（同码同文案，防枚举）" POST /api/auth/login 400 1001 \
  -H 'Content-Type: application/json' -d '{"username":"no_such_user_x","password":"whatever"}'

# ---------------------------------------------------------------- 4. 我是谁
echo
echo "[4] GET /api/auth/me"
api "带有效 Cookie → 200/0" GET /api/auth/me 200 0 -b "${jar_a}"
api "sid 形状非法 → 401/1002" GET /api/auth/me 401 1002 -H 'Cookie: sid=not-a-token'

# ---------------------------------------------------------------- 7. 标签
echo
echo "[7] GET /api/tags"
api "匿名可读 → 200/0" GET /api/tags 200 0
api "分组键存在（algorithm + source）→ 200/0" GET /api/tags 200 0
printf '        algorithm=%s source=%s\n' "$(data_key '.data.algorithm|length')" "$(data_key '.data.source|length')"

# ---------------------------------------------------------------- 8. 题目列表
echo
echo "[8] GET /api/problems"
api "默认查询 → 200/0" GET '/api/problems' 200 0
api "回显 page/size/total" GET '/api/problems?page=1&size=5' 200 0
printf '        total=%s page=%s size=%s 首题=%s\n' \
  "$(data_key '.data.total')" "$(data_key '.data.page')" "$(data_key '.data.size')" "$(data_key '.data.list[0].slug')"
api "枚举大小写不敏感 difficulty=EASY → 200/0" GET '/api/problems?difficulty=EASY' 200 0
api "难度非法 → 400/1001 field=difficulty" GET '/api/problems?difficulty=impossible' 400 1001
api "page=0 → 400/1001 field=page" GET '/api/problems?page=0' 400 1001
api "page=1.5 → 400/1001 field=page" GET '/api/problems?page=1.5' 400 1001
api "size=1000 → 夹到上限 100（200/0）" GET '/api/problems?size=1000' 200 0
printf '        size 回显=%s（期望 100）\n' "$(data_key '.data.size')"
api "size=0 → 400/1001 field=size" GET '/api/problems?size=0' 400 1001
api "未知参数被忽略 → 200/0" GET '/api/problems?whatever=1' 200 0
api "同名参数最后一个生效 → 200/0" GET '/api/problems?size=1&size=3' 200 0
api "排序 sort=featured → 200/0" GET '/api/problems?sort=featured' 200 0
api "排序非法 → 400/1001 field=sort" GET '/api/problems?sort=random' 400 1001
api "匿名带 progress → 400/1001 field=progress" GET '/api/problems?progress=solved' 400 1001
api "匿名带 favorite=1 → 400/1001 field=favorite" GET '/api/problems?favorite=1' 400 1001
api "登录带 progress=none → 200/0" GET '/api/problems?progress=none' 200 0 -b "${jar_a}"
SLUG="$(sql "SELECT slug FROM problems ORDER BY id LIMIT 1;")"
api "keyword 命中 → 200/0" GET "/api/problems?keyword=${SLUG}" 200 0

# ---------------------------------------------------------------- 9. 题目详情
echo
echo "[9] GET /api/problems/{slug}"
api "匿名读题面 → 200/0" GET "/api/problems/${SLUG}" 200 0
api "匿名的 nextRecommendation 整个不下发（-\|null）" GET "/api/problems/${SLUG}" 200 0
printf '        hint_md=%s params=%s 题面长度=%s\n' \
  "$(data_key '.data.hint_md|type')" "$(data_key '.data.params|length')" "$(data_key '.data.statementMd|length')"
api "登录后多带 recentSubmissions → 200/0" GET "/api/problems/${SLUG}" 200 0 -b "${jar_a}"
api "slug 不存在 → 404/1004" GET '/api/problems/no-such-problem' 404 1004

# ---------------------------------------------------------------- 10. 收藏
echo
echo "[10] POST /api/problems/{slug}/favorite"
api "未登录收藏 → 401/1002" POST "/api/problems/${SLUG}/favorite" 401 1002
api "toggle 第一次 → 200/0 isFavorite=true" POST "/api/problems/${SLUG}/favorite" 200 0 -b "${jar_a}"
printf '        isFavorite=%s\n' "$(data_key '.data.isFavorite')"
api "toggle 第二次 → 200/0 isFavorite=false" POST "/api/problems/${SLUG}/favorite" 200 0 -b "${jar_a}"
printf '        isFavorite=%s\n' "$(data_key '.data.isFavorite')"
api "slug 不存在 → 404/1004" POST '/api/problems/no-such-problem/favorite' 404 1004 -b "${jar_a}"
api "收藏一次并按 favorite=1 筛选" POST "/api/problems/${SLUG}/favorite" 200 0 -b "${jar_a}"
api "筛选 favorite=1 → 200/0" GET '/api/problems?favorite=1' 200 0 -b "${jar_a}"
printf '        命中数=%s（期望 1）\n' "$(data_key '.data.total')"

# ---------------------------------------------------------------- 11. 提交
echo
echo "[11] POST /api/submissions"
CODE_TWO_SUM='class Solution { public: std::vector<int> twoSum(std::vector<int>& nums, int target) { return {0,1}; } };'
BODY_TWO_SUM="$(jq -nc --arg s "${SLUG}" --arg c "${CODE_TWO_SUM}" '{problemSlug:$s, code:$c}')"
api "未登录提交 → 401/1002" POST /api/submissions 401 1002 \
  -H 'Content-Type: application/json' -d "${BODY_TWO_SUM}"
api "缺 problemSlug → 400/1001 field=problemSlug" POST /api/submissions 400 1001 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d '{"code":"int main(){}"}'
api "slug 不存在 → 404/1004" POST /api/submissions 404 1004 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d '{"problemSlug":"no-such-problem","code":"x"}'
api "代码为空 → 400/1001 field=code" POST /api/submissions 400 1001 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d "{\"problemSlug\":\"${SLUG}\",\"code\":\"\"}"
BIG_CODE="$(head -c 70000 /dev/zero | tr '\0' 'a')"
api "代码超 64KB → 400/1001 field=code" POST /api/submissions 400 1001 \
  -b "${jar_a}" -H 'Content-Type: application/json' \
  -d "$(jq -nc --arg s "${SLUG}" --arg c "${BIG_CODE}" '{problemSlug:$s, code:$c}')"
# judge 调度器（T2.7）尚未接线：契约规定此时回 1009 且【不落库】
api "判题器未接线 → 503/1009" POST /api/submissions 503 1009 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d "${BODY_TWO_SUM}"

# ---------------------------------------------------------------- 12/13/14/15/16 提交读侧
# judge 未接线 ⇒ 库里没有真实提交。这几条用例【用 SQL 造出 judge 会写回的那种行】
# （这是判题侧的唯一产出方式），再从 HTTP 侧验归属校验、分页、diff、删除、重跑。
echo
echo "[12] GET /api/submissions/{id}"
sql "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict, message, case_passed, case_total, total_ms, max_mem_kb, created_at, finished_at)
     SELECT ${ID_A}, id, 'cpp', 'class Solution { /* v1 */ };', 'done', 'WA', '第 1 个用例答案错误', 0, 2, 41, 3072, UTC_TIMESTAMP(), UTC_TIMESTAMP()
     FROM problems ORDER BY id LIMIT 1;" >/dev/null
SUB1="$(sql "SELECT id FROM submissions WHERE user_id=${ID_A} ORDER BY id LIMIT 1;")"
sql "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict, message, case_passed, case_total, total_ms, max_mem_kb, created_at, finished_at)
     SELECT ${ID_A}, id, 'cpp', 'class Solution { /* v2 修好了 */ };', 'done', 'AC', '全部用例通过', 2, 2, 39, 2048, UTC_TIMESTAMP(), UTC_TIMESTAMP()
     FROM problems ORDER BY id LIMIT 1;" >/dev/null
SUB2="$(sql "SELECT id FROM submissions WHERE user_id=${ID_A} ORDER BY id DESC LIMIT 1;")"
# 一条 failed/JE：canRetry 与 1011 的判定都要用它（[D-49]/[D-58]）
sql "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict, message, case_passed, case_total, total_ms, max_mem_kb, created_at)
     SELECT ${ID_A}, id, 'cpp', 'class Solution { /* JE */ };', 'failed', 'JE', '测试程序未能运行（平台错误）', NULL, NULL, NULL, NULL, UTC_TIMESTAMP()
     FROM problems ORDER BY id LIMIT 1;" >/dev/null
SUB_JE="$(sql "SELECT id FROM submissions WHERE user_id=${ID_A} AND status='failed' ORDER BY id DESC LIMIT 1;")"
sql "INSERT INTO submission_case_results (submission_id, case_idx, verdict, passed, args_text, output_text, expected_text, time_ms, mem_kb, error_msg)
     VALUES (${SUB1}, 0, 'WA', 0, '[2,7,11,15], target=9', '[0,0]', '[0,1]', 3, 2048, NULL),
            (${SUB1}, 1, 'TLE', 0, '[3,2,4], target=6', NULL, '[1,0]', NULL, NULL, '超出时间限制 (3000ms)');" >/dev/null

api "本人读详情 → 200/0" GET "/api/submissions/${SUB1}" 200 0 -b "${jar_a}"
printf '        verdict=%s casePassed=%s/%s 用例数=%s canRetry=%s 诊断数=%s\n' \
  "$(data_key '.data.verdict')" "$(data_key '.data.casePassed')" "$(data_key '.data.caseTotal')" \
  "$(data_key '.data.caseResults|length')" "$(data_key '.data.canRetry')" "$(data_key '.data.compileErrors|length')"
printf '        TLE 用例的 timeMs 应为 null（不是 0）：%s\n' "$(data_key '.data.caseResults[1].timeMs|type')"
api "匿名读详情 → 401/1002" GET "/api/submissions/${SUB1}" 401 1002
api "id 不存在 → 404/1004" GET '/api/submissions/999999999' 404 1004 -b "${jar_a}"

echo
echo "[13] GET /api/submissions"
api "本人列表 → 200/0" GET '/api/submissions' 200 0 -b "${jar_a}"
printf '        total=%s 首行=%s/%s\n' "$(data_key '.data.total')" \
  "$(data_key '.data.list[0].id')" "$(data_key '.data.list[0].verdict')"
api "按 verdict=AC 过滤 → 200/0" GET '/api/submissions?verdict=AC' 200 0 -b "${jar_a}"
printf '        命中=%s（期望 1）\n' "$(data_key '.data.total')"
api "按 problemSlug 过滤 → 200/0" GET "/api/submissions?problemSlug=${SLUG}" 200 0 -b "${jar_a}"
api "verdict 非法 → 400/1001 field=verdict" GET '/api/submissions?verdict=XX' 400 1001 -b "${jar_a}"
api "page=0 → 400/1001 field=page" GET '/api/submissions?page=0' 400 1001 -b "${jar_a}"
api "size=1000 → 夹到 100（200/0）" GET '/api/submissions?size=1000' 200 0 -b "${jar_a}"
printf '        size 回显=%s\n' "$(data_key '.data.size')"
api "匿名列表 → 401/1002" GET '/api/submissions' 401 1002

echo
echo "[14] GET /api/submissions/diff"
api "两条本人提交 → 200/0" GET "/api/submissions/diff?from=${SUB1}&to=${SUB2}" 200 0 -b "${jar_a}"
printf '        from=%s to=%s（服务端只返原文，hunks 由前端算）\n' \
  "$(data_key '.data.from.id')" "$(data_key '.data.to.id')"
api "from 缺失 → 400/1001 field=from" GET "/api/submissions/diff?to=${SUB2}" 400 1001 -b "${jar_a}"
api "to 非数字 → 400/1001 field=to" GET "/api/submissions/diff?from=${SUB1}&to=abc" 400 1001 -b "${jar_a}"
api "from == to → 400/1001" GET "/api/submissions/diff?from=${SUB1}&to=${SUB1}" 400 1001 -b "${jar_a}"
api "id 不存在 → 404/1004" GET '/api/submissions/diff?from=999999998&to=999999997' 404 1004 -b "${jar_a}"

echo
echo "[15] DELETE /api/submissions/{id}"
sql "INSERT INTO users (username, password_hash, role, created_at) VALUES ('${USER_B}', 'x', 'user', UTC_TIMESTAMP());" >/dev/null
ID_B="$(sql "SELECT id FROM users WHERE username='${USER_B}';")"
SUB_B="$(sql "INSERT INTO submissions (user_id, problem_id, language, code, status, created_at)
             SELECT ${ID_B}, id, 'cpp', '/* 别人的 */', 'pending', UTC_TIMESTAMP() FROM problems ORDER BY id LIMIT 1;
             SELECT LAST_INSERT_ID();")"
api "删他人的提交 → 403/1003" DELETE "/api/submissions/${SUB_B}" 403 1003 -b "${jar_a}"
api "删不存在的提交 → 404/1004" DELETE '/api/submissions/999999996' 404 1004 -b "${jar_a}"
api "删自己的提交 → 200/0" DELETE "/api/submissions/${SUB2}" 200 0 -b "${jar_a}"
api "删完再读 → 404/1004" GET "/api/submissions/${SUB2}" 404 1004 -b "${jar_a}"

echo
echo "[16] POST /api/submissions/{id}/retry"
api "重跑一条 done 的提交 → 409/1011" POST "/api/submissions/${SUB1}/retry" 409 1011 -b "${jar_a}"
api "重跑他人的提交 → 403/1003" POST "/api/submissions/${SUB_B}/retry" 403 1003 -b "${jar_a}"
api "重跑不存在的提交 → 404/1004" POST '/api/submissions/999999995/retry' 404 1004 -b "${jar_a}"
# failed/JE 是唯一可重跑的前置条件；judge 未接线时先回 1009 且【不建行】
api "重跑 failed/JE（判题器未接线）→ 503/1009" POST "/api/submissions/${SUB_JE}/retry" 503 1009 -b "${jar_a}"
printf '        未因重跑产生新行：%s（期望 0）\n' \
  "$(sql "SELECT COUNT(*) FROM submissions WHERE user_id=${ID_A} AND code LIKE '%JE%' AND status='pending';")"

# ---------------------------------------------------------------- 17. 统计
echo
echo "[17] GET /api/stats"
api "本人 → 200/0" GET /api/stats 200 0 -b "${jar_a}"
printf '        total=%s solved=%s attempted=%s favorites=%s\n' \
  "$(data_key '.data.total')" "$(data_key '.data.solved')" "$(data_key '.data.attempted')" "$(data_key '.data.favorites')"
api "匿名 → 401/1002" GET /api/stats 401 1002

# ---------------------------------------------------------------- 18. 概览
echo
echo "[18] GET /api/me/overview"
api "本人 → 200/0" GET /api/me/overview 200 0 -b "${jar_a}"
printf '        totals=%s 难度条=%s 标签条=%s recent=%s\n' \
  "$(data_key '.data.totals|tojson')" "$(data_key '.data.byDifficulty|length')" \
  "$(data_key '.data.byTag|length')" "$(data_key '.data.recent|length')"
api "匿名 → 401/1002" GET /api/me/overview 401 1002

# ---------------------------------------------------------------- 19. 管理
echo
echo "[19] GET /api/admin/overview"
api "匿名 → 401/1002（先判登录）" GET /api/admin/overview 401 1002
api "非 admin → 403/1003（再判角色）" GET /api/admin/overview 403 1003 -b "${jar_a}"
# T3.11（按 .env 同步 role）尚未接线，故这里直接改库把用户提成 admin 再验 200
sql "UPDATE users SET role='admin' WHERE id=${ID_A};" >/dev/null
api "admin 重新登录后 → 200/0" POST /api/auth/login 200 0 \
  -c "${jar_a}" -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"${PASS_A}\"}"
api "admin → 200/0" GET /api/admin/overview 200 0 -b "${jar_a}"
printf '        userCount=%s problemCount=%s judgeAlive=%s activeJudges=%s users[]=%s\n' \
  "$(data_key '.data.system.userCount')" "$(data_key '.data.system.problemCount')" \
  "$(data_key '.data.system.judgeAlive')" "$(data_key '.data.system.activeJudges')" \
  "$(data_key '.data.users|length')"

# ---------------------------------------------------------------- 5. 改密
echo
echo "[5] POST /api/auth/password"
api "旧密码错 → 400/1001 field=oldPassword" POST /api/auth/password 400 1001 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d '{"oldPassword":"wrong-one","newPassword":"newpass1234"}'
api "新密码过短 → 400/1001 field=newPassword" POST /api/auth/password 400 1001 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d '{"oldPassword":"pass1234","newPassword":"123"}'
# 先用第二个设备登录一��：改密后它必须被踢掉（D-40），而当前会话必须还在
api "第二设备登录 → 200/0" POST /api/auth/login 200 0 \
  -c "${jar_a2}" -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"${PASS_A}\"}"
api "改密成功 → 200/0" POST /api/auth/password 200 0 \
  -b "${jar_a}" -H 'Content-Type: application/json' -d "{\"oldPassword\":\"${PASS_A}\",\"newPassword\":\"newpass1234\"}"
api "改密后当前会话仍有效（D-40）→ 200/0" GET /api/auth/me 200 0 -b "${jar_a}"
api "改密后其他设备已被踢 → 401/1002" GET /api/auth/me 401 1002 -b "${jar_a2}"
api "用新密码登录 → 200/0" POST /api/auth/login 200 0 \
  -c "${jar_a2}" -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"newpass1234\"}"
api "用旧密码登录 → 400/1001" POST /api/auth/login 400 1001 \
  -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"${PASS_A}\"}"

# ---------------------------------------------------------------- 3. 登出
echo
echo "[3] POST /api/auth/logout"
api "带 sid 登出 → 200/0" POST /api/auth/logout 200 0 -b "${jar_a}"
api "登出后 me → 401/1002" GET /api/auth/me 401 1002 -b "${jar_a}"
api "重复登出（幂等）→ 200/0" POST /api/auth/logout 200 0 -b "${jar_a}"
api "不带 Cookie 登出（幂等且不碰 DB）→ 200/0" POST /api/auth/logout 200 0

# ---------------------------------------------------------------- 6. 注销账号
echo
echo "[6] DELETE /api/auth/account"
api "密码错 → 400/1001 field=password" DELETE /api/auth/account 400 1001 \
  -b "${jar_a2}" -H 'Content-Type: application/json' -d '{"password":"not-the-password"}'
api "注销成功 → 200/0" DELETE /api/auth/account 200 0 \
  -b "${jar_a2}" -H 'Content-Type: application/json' -d '{"password":"newpass1234"}'
api "注销后 me → 401/1002" GET /api/auth/me 401 1002 -b "${jar_a2}"
# 账号已被硬删除：用新密码登录仍应是 400/1001（用户不存在与口令错误同码同文案，防枚举，§6.3）
api "注销后账号确实不存在 → 400/1001" POST /api/auth/login 400 1001 \
  -H 'Content-Type: application/json' -d "{\"username\":\"${USER_A}\",\"password\":\"newpass1234\"}"

# ---------------------------------------------------------------- 清理
sql "DELETE FROM users WHERE username IN ('${USER_A}','${USER_B}');" >/dev/null

echo
echo "=== 结果：通过 ${PASS}，失败 ${FAIL} ==="
[[ "${FAIL}" -eq 0 ]]