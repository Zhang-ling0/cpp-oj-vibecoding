#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CppOJ 接口自动化验收（HTTP 契约层）—— ``tests/run_tests.sh`` 的 Python 对应物。

对应任务：SPEC.md §9 M5 T5.3（端到端验收）；契约来源：API.md（端点总览 / §1.2 码表 / §3–§7）。
与 ``tests/run_unit_tests.sh``（gtest，纯逻辑 + 业务层 SQL）的分工：那里钉业务层口径，
这里钉 **HTTP** —— 路由接线、Cookie、包络、错误收口这三件事只有真正发请求才能验。

用法（必须先起服务，且从仓库根目录执行）::

    make build JOBS=1
    (setsid nohup ./build/oj > /tmp/oj-run.log 2>&1 < /dev/null &)
    sleep 5
    python3 tests/python/run_api_tests.py                  # 或 python3 -m tests.python.run_api_tests

    OJ_BASE=http://127.0.0.1:18080 python3 tests/python/run_api_tests.py   # 打别的实例

可调项：

============================  ==========================================  =========================
变量/开关                       默认                                        说明
============================  ==========================================  =========================
``OJ_BASE``                    ``http://127.0.0.1:<.env 的 OJ_LISTEN 端口>``  被测实例地址
``OJ_TEST_TIMEOUT``            ``10``                                       单请求超时（秒）
``--strict``                   关                                           把包络不合规也算失败
============================  ==========================================  =========================

两条口径（与 curl 版完全一致）：

* **每个响应同时断言 HTTP 状态码与业务 ``code``**（``[D-29]``，两者出自不同的代码路径）。
* **失败必须打印响应体前 400 字节** —— 满屏 ``code=null`` 的根因是响应不是 JSON。

与 curl 版的两点刻意差异：

1. curl 版有若干「标签里写了期望、脚本却只 printf 出来」的**空断言**（``data.field``、
   ``isFavorite`` 的翻转结果、``1009`` 时**不落库**、``caseResults[1].timeMs`` 为 null…）。
   这里把它们升级成真断言，所以本版的断言条数比文档里 curl 版的 98 条**多**，
   末尾会打印实际条数 —— 拿条数与文档对比时请以本脚本输出为准。
2. 收尾清理放进 ``finally``：中途异常退出也会删掉本次造出来的账号与提交。
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import requests  # noqa: E402

from oj_testlib import (  # noqa: E402
    Mysql,
    Reporter,
    dget,
    djson,
    dlen,
    dtype,
    load_dotenv,
    wait_for,
)

# 一段合法的 C++ 提交（judge 未接线时用不到，但字段值要合法）
CODE_TWO_SUM = (
    "class Solution { public: std::vector<int> twoSum(std::vector<int>& nums, "
    "int target) { return {0,1}; } };"
)


def base_url(env: dict[str, str]) -> str:
    """``OJ_BASE`` 优先，否则按 ``.env`` 的 ``OJ_LISTEN`` 拼（0.0.0.0 → 127.0.0.1）。"""
    if os.environ.get("OJ_BASE"):
        return os.environ["OJ_BASE"]
    listen = env.get("OJ_LISTEN", "0.0.0.0:8080")
    host, _, port = listen.rpartition(":")
    host = "127.0.0.1" if host in ("", "0.0.0.0", "::") else host
    return f"http://{host}:{port}"


def field(r: Reporter, resp, want: str, label: str) -> bool:
    """把 curl 版「标签里写的 field=X」变成真断言。"""
    return r.check_eq(label, dget(resp, "data.field"), want)


def main() -> int:
    env = load_dotenv()
    base = base_url(env)
    db = Mysql.from_env(env)
    r = Reporter(
        "CppOJ 端到端验收（HTTP 契约）",
        base,
        timeout=float(os.environ.get("OJ_TEST_TIMEOUT", "10")),
        strict_envelope="--strict" in sys.argv,
    )

    # 随机用户名：验收要能反复跑而不撞 §6.3 的唯一键（用户名只允许 [A-Za-z0-9_-]）
    run_id = f"t{os.getpid()}"
    user_a, user_b = f"{run_id}a", f"{run_id}b"
    pass_a = f"{run_id}pass1234"        # 满足 §5.8 的 6–64 位

    # 三个会话 = curl 的三个 cookie jar：a 是主力、a2 是「第二台设备」（改密要验证被踢）、
    # b 留作「另一个普通用户」这一层（验证「他人资源」403）。
    sa, sa2 = requests.Session(), requests.Session()
    bad_sid = requests.Session()        # 只带一个非法 Cookie，不带任何真实会话

    print("=== CppOJ 端到端验收（HTTP 契约）===")
    print(f"服务：{r.base}")
    print(f"账号：{user_a} / {user_b}")
    if not wait_for(base, timeout=8.0):
        print("服务不可达：先起 ./build/oj（或 make run），启动回显看 /tmp/oj-run.log，"
              "末尾应有「HTTP 监听 ...」", file=sys.stderr)
        return 2

    try:
        run_all(r, db, sa, sa2, bad_sid, user_a, user_b, pass_a)
    finally:
        # 收尾：注销接口已删掉 user_a，这里兜底删掉两边的账号（提交/收藏等由外键 CASCADE）
        db.exec(f"DELETE FROM users WHERE username IN ('{user_a}','{user_b}');")
        r.summary()
    return 0 if r.failed == 0 else 1


def run_all(r: Reporter, db: Mysql, sa, sa2, bad_sid,
            user_a: str, user_b: str, pass_a: str) -> None:
    slug = db.scalar("SELECT slug FROM problems ORDER BY id LIMIT 1;")

    # ---------------------------------------------------------------- 20. 静态资源
    r.section("[20] GET /* 静态资源与全局收口")
    r.static("五页之一：/login.html", "/login.html", 200)
    r.static("五页之一：/admin.html", "/admin.html", 200)
    r.static("vendored 库：/js/api.js 存在则 200，否则 404（只查可达性）", "/js/api.js", 200)
    r.api("不存在的静态文件 → 404/1004", "GET", "/nonexistent.html", 404, 1004)
    r.api("不存在的 API 路径 → 404/1004", "GET", "/api/nope", 404, 1004)
    # 方法不对：httplib 0.60 不产生 405（它只有那个状态码枚举，没有任何地方会回它），
    # 实际是 404 → 收口成 1004。对前端而言两者同码（「你要用的东西不存在」），见 API.md §8.2。
    r.api("路径存在但方法不对 → 404/1004（httplib 不发 405）", "GET", "/api/auth/login", 404, 1004)

    # ---------------------------------------------------------------- 1. 注册
    r.section("[1] POST /api/auth/register")
    reg = {"username": user_a, "password": pass_a}
    resp = r.api("正常注册 → 201/0", "POST", "/api/auth/register", 201, 0, json=reg)
    user_id_a = dget(resp, "data.userId")
    r.check_eq("注册返回的 userId 是数字", dtype(resp, "data.userId"), "number")
    r.api("重复注册 → 409/1005", "POST", "/api/auth/register", 409, 1005, json=reg)
    resp = r.api("用户名字符集非法 → 400/1001", "POST", "/api/auth/register", 400, 1001,
                 json={"username": "用户 名", "password": "pass1234"})
    field(r, resp, "username", "      ↑ data.field 指名参数")
    resp = r.api("密码过短 → 400/1001", "POST", "/api/auth/register", 400, 1001,
                 json={"username": "ab1", "password": "123"})
    field(r, resp, "password", "      ↑ data.field 指名参数")
    r.api("body 不是 JSON → 400/1001", "POST", "/api/auth/register", 400, 1001,
          data="not-json", headers={"Content-Type": "application/json"})
    # 注册【不登录】：不带任何 Cookie 调 me 必须是 1002
    r.api("注册不建立会话（未登录调 me）→ 401/1002", "GET", "/api/auth/me", 401, 1002)

    # ---------------------------------------------------------------- 2. 登录
    r.section("[2] POST /api/auth/login")
    resp = r.api("登录成功 → 200/0 + Set-Cookie", "POST", "/api/auth/login", 200, 0,
                 session=sa, json={"username": user_a, "password": pass_a})
    r.check("响应带 Set-Cookie: sid=…", "sid=" in resp.headers.get("Set-Cookie", ""),
            resp.headers.get("Set-Cookie", "（无）")[:60])
    resp = r.api("密码错误 → 400/1001", "POST", "/api/auth/login", 400, 1001,
                 json={"username": user_a, "password": "wrong-pass"})
    field(r, resp, "password", "      ↑ data.field 指名参数")
    resp = r.api("用户不存在 → 400/1001（同码同文案，防枚举）", "POST", "/api/auth/login",
                 400, 1001, json={"username": "no_such_user_x", "password": "whatever"})
    r.check_eq("      ↑ 与「密码错误」文案一致（防枚举）",
               dget(resp, "message"), "参数校验失败")

    # ---------------------------------------------------------------- 4. 我是谁
    r.section("[4] GET /api/auth/me")
    resp = r.api("带有效 Cookie → 200/0", "GET", "/api/auth/me", 200, 0, session=sa)
    r.check_eq("data.username 是刚注册的那个", dget(resp, "data.username"), user_a)
    r.api("sid 形状非法 → 401/1002", "GET", "/api/auth/me", 401, 1002, session=bad_sid,
          headers={"Cookie": "sid=not-a-token"})

    # ---------------------------------------------------------------- 7. 标签
    r.section("[7] GET /api/tags")
    resp = r.api("匿名可读 → 200/0", "GET", "/api/tags", 200, 0)
    r.check("分组键存在（algorithm + source）",
            dlen(resp, "data.algorithm") != "-" and dlen(resp, "data.source") != "-",
            f"algorithm={dlen(resp, 'data.algorithm')} 项 source={dlen(resp, 'data.source')} 项")

    # ---------------------------------------------------------------- 8. 题目列表
    r.section("[8] GET /api/problems")
    r.api("默认查询 → 200/0", "GET", "/api/problems", 200, 0)
    resp = r.api("回显 page/size/total", "GET", "/api/problems?page=1&size=5", 200, 0)
    r.info(f"total={dget(resp, 'data.total')} page={dget(resp, 'data.page')} "
           f"size={dget(resp, 'data.size')} 首题={dget(resp, 'data.list[0].slug')}")
    r.api("枚举大小写不敏感 difficulty=EASY → 200/0", "GET", "/api/problems?difficulty=EASY", 200, 0)
    resp = r.api("难度非法 → 400/1001", "GET", "/api/problems?difficulty=impossible", 400, 1001)
    field(r, resp, "difficulty", "      ↑ data.field 指名参数")
    resp = r.api("page=0 → 400/1001", "GET", "/api/problems?page=0", 400, 1001)
    field(r, resp, "page", "      ↑ data.field 指名参数")
    resp = r.api("page=1.5 → 400/1001", "GET", "/api/problems?page=1.5", 400, 1001)
    field(r, resp, "page", "      ↑ data.field 指名参数")
    resp = r.api("size=1000 → 夹到上限 100（200/0）", "GET", "/api/problems?size=1000", 200, 0)
    r.check_eq("      ↑ data.size 回显被夹到 100", dget(resp, "data.size"), 100)
    resp = r.api("size=0 → 400/1001", "GET", "/api/problems?size=0", 400, 1001)
    field(r, resp, "size", "      ↑ data.field 指名参数")
    r.api("未知参数被忽略 → 200/0", "GET", "/api/problems?whatever=1", 200, 0)
    resp = r.api("同名参数最后一个生效 → 200/0", "GET", "/api/problems?size=1&size=3", 200, 0,
                 params=[("size", "1"), ("size", "3")])
    r.check_eq("      ↑ data.size 回显最后一个（3）", dget(resp, "data.size"), 3)
    r.api("排序 sort=featured → 200/0", "GET", "/api/problems?sort=featured", 200, 0)
    resp = r.api("排序非法 → 400/1001", "GET", "/api/problems?sort=random", 400, 1001)
    field(r, resp, "sort", "      ↑ data.field 指名参数")
    resp = r.api("匿名带 progress → 400/1001", "GET", "/api/problems?progress=solved", 400, 1001)
    field(r, resp, "progress", "      ↑ 匿名不得按进度筛")
    resp = r.api("匿名带 favorite=1 → 400/1001", "GET", "/api/problems?favorite=1", 400, 1001)
    field(r, resp, "favorite", "      ↑ 匿名不得按收藏筛")
    r.api("登录带 progress=none → 200/0", "GET", "/api/problems?progress=none", 200, 0, session=sa)
    r.api(f"keyword 命中（{slug}）→ 200/0", "GET", "/api/problems", 200, 0,
          session=sa, params={"keyword": slug})

    # ---------------------------------------------------------------- 9. 题目详情
    r.section("[9] GET /api/problems/{slug}")
    anon_detail = r.api("匿名读题面 → 200/0", "GET", f"/api/problems/{slug}", 200, 0)
    r.check_eq("匿名的 nextRecommendation 整个不下发（键不存在）",
               dget(anon_detail, "data.nextRecommendation", default="<无此键>"), "<无此键>")
    r.info(f"hint_md={dtype(anon_detail, 'data.hint_md')} "
           f"params={dlen(anon_detail, 'data.params')} 项 "
           f"题面长度={dlen(anon_detail, 'data.statementMd')} 字符")
    login_detail = r.api("登录后多带 recentSubmissions → 200/0", "GET",
                         f"/api/problems/{slug}", 200, 0, session=sa)
    r.check("登录后 nextRecommendation 才下发",
            dget(login_detail, "data.nextRecommendation", default=None) is not None,
            djson(login_detail, "data.nextRecommendation"))
    r.api("slug 不存在 → 404/1004", "GET", "/api/problems/no-such-problem", 404, 1004)

    # ---------------------------------------------------------------- 10. 收藏
    r.section("[10] POST /api/problems/{slug}/favorite")
    r.api("未登录收藏 → 401/1002", "POST", f"/api/problems/{slug}/favorite", 401, 1002)
    resp = r.api("toggle 第一次 → 200/0", "POST", f"/api/problems/{slug}/favorite", 200, 0,
                 session=sa)
    r.check_eq("      ↑ isFavorite=true", dget(resp, "data.isFavorite"), True)
    resp = r.api("toggle 第二次 → 200/0（翻回来）", "POST", f"/api/problems/{slug}/favorite",
                 200, 0, session=sa)
    r.check_eq("      ↑ isFavorite=false", dget(resp, "data.isFavorite"), False)
    r.api("slug 不存在 → 404/1004", "POST", "/api/problems/no-such-problem/favorite", 404, 1004,
          session=sa)
    r.api("收藏一次并按 favorite=1 筛选", "POST", f"/api/problems/{slug}/favorite", 200, 0,
          session=sa)
    resp = r.api("筛选 favorite=1 → 200/0", "GET", "/api/problems", 200, 0, session=sa,
                 params={"favorite": 1})
    r.check_eq("      ↑ 命中数", dget(resp, "data.total"), 1)

    # ---------------------------------------------------------------- 11. 提交
    r.section("[11] POST /api/submissions")
    body = {"problemSlug": slug, "code": CODE_TWO_SUM}
    r.api("未登录提交 → 401/1002", "POST", "/api/submissions", 401, 1002, json=body)
    resp = r.api("缺 problemSlug → 400/1001", "POST", "/api/submissions", 400, 1001,
                 session=sa, json={"code": "int main(){}"})
    field(r, resp, "problemSlug", "      ↑ data.field 指名参数")
    r.api("slug 不存在 → 404/1004", "POST", "/api/submissions", 404, 1004, session=sa,
          json={"problemSlug": "no-such-problem", "code": "x"})
    resp = r.api("代码为空 → 400/1001", "POST", "/api/submissions", 400, 1001, session=sa,
                 json={"problemSlug": slug, "code": ""})
    field(r, resp, "code", "      ↑ data.field 指名参数")
    resp = r.api("代码超 64KB → 400/1001", "POST", "/api/submissions", 400, 1001, session=sa,
                 json={"problemSlug": slug, "code": "a" * 70000})
    field(r, resp, "code", "      ↑ OJ_MAX_CODE_BYTES=65536")
    # judge 调度器（T2.7）尚未接线：契约规定此时回 1009 且【不落库】
    before = int(db.scalar(f"SELECT COUNT(*) FROM submissions WHERE user_id={user_id_a};") or 0)
    r.api("判题器未接线 → 503/1009", "POST", "/api/submissions", 503, 1009, session=sa, json=body)
    after = int(db.scalar(f"SELECT COUNT(*) FROM submissions WHERE user_id={user_id_a};") or 0)
    r.check_eq("      ↑ 1009 之后一条都没落库（文档 §11）", after, before)

    # ---------------------------------------------------------------- 12. 提交详情
    # judge 未接线 ⇒ 库里没有真实提交。这几条用例【用 SQL 造出 judge 会写回的那种行】
    # （这是判题侧的唯一产出方式），再从 HTTP 侧验归属校验、分页、diff、删除、重跑。
    r.section("[12] GET /api/submissions/{id}")
    db.exec(f"""INSERT INTO submissions (user_id, problem_id, language, code, status, verdict,
                 message, case_passed, case_total, total_ms, max_mem_kb, created_at, finished_at)
                SELECT {user_id_a}, id, 'cpp', 'class Solution {{ /* v1 */ }};', 'done', 'WA',
                       '第 1 个用例答案错误', 0, 2, 41, 3072, UTC_TIMESTAMP(), UTC_TIMESTAMP()
                FROM problems ORDER BY id LIMIT 1;""")
    sub1 = db.scalar(f"SELECT id FROM submissions WHERE user_id={user_id_a} ORDER BY id LIMIT 1;")
    db.exec(f"""INSERT INTO submissions (user_id, problem_id, language, code, status, verdict,
                 message, case_passed, case_total, total_ms, max_mem_kb, created_at, finished_at)
                SELECT {user_id_a}, id, 'cpp', 'class Solution {{ /* v2 修好了 */ }};', 'done', 'AC',
                       '全部用例通过', 2, 2, 39, 2048, UTC_TIMESTAMP(), UTC_TIMESTAMP()
                FROM problems ORDER BY id LIMIT 1;""")
    sub2 = db.scalar(f"SELECT id FROM submissions WHERE user_id={user_id_a} ORDER BY id DESC LIMIT 1;")
    # 一条 failed/JE：canRetry 与 1011 的判定都要用它（[D-49]/[D-58]）
    db.exec(f"""INSERT INTO submissions (user_id, problem_id, language, code, status, verdict,
                 message, case_passed, case_total, total_ms, max_mem_kb, created_at)
                SELECT {user_id_a}, id, 'cpp', 'class Solution {{ /* JE */ }};', 'failed', 'JE',
                       '测试程序未能运行（平台错误）', NULL, NULL, NULL, NULL, UTC_TIMESTAMP()
                FROM problems ORDER BY id LIMIT 1;""")
    sub_je = db.scalar(
        f"SELECT id FROM submissions WHERE user_id={user_id_a} AND status='failed' "
        f"ORDER BY id DESC LIMIT 1;")
    db.exec(f"""INSERT INTO submission_case_results (submission_id, case_idx, verdict, passed,
                 args_text, output_text, expected_text, time_ms, mem_kb, error_msg)
                VALUES ({sub1}, 0, 'WA', 0, '[2,7,11,15], target=9', '[0,0]', '[0,1]', 3, 2048, NULL),
                       ({sub1}, 1, 'TLE', 0, '[3,2,4], target=6', NULL, '[1,0]', NULL, NULL,
                        '超出时间限制 (3000ms)');""")

    resp = r.api("本人读详情 → 200/0", "GET", f"/api/submissions/{sub1}", 200, 0, session=sa)
    r.info(f"verdict={dget(resp, 'data.verdict')} "
           f"casePassed={dget(resp, 'data.casePassed')}/{dget(resp, 'data.caseTotal')} "
           f"用例数={dlen(resp, 'data.caseResults')} canRetry={dget(resp, 'data.canRetry')} "
           f"诊断数={dlen(resp, 'data.compileErrors')}")
    r.check_eq("TLE 用例的 timeMs 是 null 而不是 0（区分「没跑到」与「跑得很快」）",
               dtype(resp, "data.caseResults[1].timeMs"), "null")
    r.check_eq("done/WA 的提交 canRetry=false", dget(resp, "data.canRetry"), False)
    r.api("匿名读详情 → 401/1002", "GET", f"/api/submissions/{sub1}", 401, 1002)
    r.api("id 不存在 → 404/1004", "GET", "/api/submissions/999999999", 404, 1004, session=sa)

    r.section("[13] GET /api/submissions")
    resp = r.api("本人列表 → 200/0", "GET", "/api/submissions", 200, 0, session=sa)
    r.info(f"total={dget(resp, 'data.total')} 首行={dget(resp, 'data.list[0].id')}"
           f"/{dget(resp, 'data.list[0].verdict')}")
    resp = r.api("按 verdict=AC 过滤 → 200/0", "GET", "/api/submissions", 200, 0, session=sa,
                 params={"verdict": "AC"})
    r.check_eq("      ↑ 命中数（本次只有 v2 那条 AC）", dget(resp, "data.total"), 1)
    r.api("按 problemSlug 过滤 → 200/0", "GET", "/api/submissions", 200, 0, session=sa,
          params={"problemSlug": slug})
    resp = r.api("verdict 非法 → 400/1001", "GET", "/api/submissions", 400, 1001, session=sa,
                 params={"verdict": "XX"})
    field(r, resp, "verdict", "      ↑ data.field 指名参数")
    resp = r.api("page=0 → 400/1001", "GET", "/api/submissions", 400, 1001, session=sa,
                 params={"page": 0})
    field(r, resp, "page", "      ↑ data.field 指名参数")
    resp = r.api("size=1000 → 夹到 100（200/0）", "GET", "/api/submissions", 200, 0, session=sa,
                 params={"size": 1000})
    r.check_eq("      ↑ data.size 回显", dget(resp, "data.size"), 100)
    r.api("匿名列表 → 401/1002", "GET", "/api/submissions", 401, 1002)

    r.section("[14] GET /api/submissions/diff")
    resp = r.api("两条本人提交 → 200/0", "GET", "/api/submissions/diff", 200, 0, session=sa,
                 params={"from": sub1, "to": sub2})
    r.info(f"from={dget(resp, 'data.from.id')} to={dget(resp, 'data.to.id')}"
           f"（服务端只返原文，hunks 由前端算）")
    resp = r.api("from 缺失 → 400/1001", "GET", "/api/submissions/diff", 400, 1001, session=sa,
                 params={"to": sub2})
    field(r, resp, "from", "      ↑ data.field 指名参数")
    resp = r.api("to 非数字 → 400/1001", "GET", "/api/submissions/diff", 400, 1001, session=sa,
                 params={"from": sub1, "to": "abc"})
    field(r, resp, "to", "      ↑ data.field 指名参数")
    r.api("from == to → 400/1001", "GET", "/api/submissions/diff", 400, 1001, session=sa,
          params={"from": sub1, "to": sub1})
    r.api("id 不存在 → 404/1004", "GET", "/api/submissions/diff", 404, 1004, session=sa,
          params={"from": 999999998, "to": 999999997})

    r.section("[15] DELETE /api/submissions/{id}")
    db.exec("INSERT INTO users (username, password_hash, role, created_at) "
            f"VALUES ('{user_b}', 'x', 'user', UTC_TIMESTAMP());")
    user_id_b = db.scalar(f"SELECT id FROM users WHERE username='{user_b}';")
    sub_b = db.scalar(f"""INSERT INTO submissions (user_id, problem_id, language, code, status,
                          created_at)
                         SELECT {user_id_b}, id, 'cpp', '/* 别人的 */', 'pending', UTC_TIMESTAMP()
                         FROM problems ORDER BY id LIMIT 1;
                         SELECT LAST_INSERT_ID();""")
    r.api("删他人的提交 → 403/1003", "DELETE", f"/api/submissions/{sub_b}", 403, 1003, session=sa)
    r.api("删不存在的提交 → 404/1004", "DELETE", "/api/submissions/999999996", 404, 1004, session=sa)
    r.api("删自己的提交 → 200/0", "DELETE", f"/api/submissions/{sub2}", 200, 0, session=sa)
    r.api("删完再读 → 404/1004", "GET", f"/api/submissions/{sub2}", 404, 1004, session=sa)

    r.section("[16] POST /api/submissions/{id}/retry")
    r.api("重跑一条 done 的提交 → 409/1011", "POST", f"/api/submissions/{sub1}/retry", 409, 1011,
          session=sa)
    r.api("重跑他人的提交 → 403/1003", "POST", f"/api/submissions/{sub_b}/retry", 403, 1003, session=sa)
    r.api("重跑不存在的提交 → 404/1004", "POST", "/api/submissions/999999995/retry", 404, 1004,
          session=sa)
    # failed/JE 是唯一可重跑的前置条件；judge 未接线时先回 1009 且【不建行】
    r.api("重跑 failed/JE（判题器未接线）→ 503/1009", "POST", f"/api/submissions/{sub_je}/retry",
          503, 1009, session=sa)
    r.check_eq("      ↑ 未因重跑产生新行",
               db.scalar(f"SELECT COUNT(*) FROM submissions WHERE user_id={user_id_a} "
                         f"AND code LIKE '%JE%' AND status='pending';"), 0)

    r.section("[17] GET /api/stats")
    resp = r.api("本人 → 200/0", "GET", "/api/stats", 200, 0, session=sa)
    r.info(f"total={dget(resp, 'data.total')} solved={dget(resp, 'data.solved')} "
           f"attempted={dget(resp, 'data.attempted')} favorites={dget(resp, 'data.favorites')}")
    r.check("四个标量都在", all(dtype(resp, f"data.{k}") == "number"
                              for k in ("total", "solved", "attempted", "favorites")),
            "total/solved/attempted/favorites 均为 number")
    r.api("匿名 → 401/1002", "GET", "/api/stats", 401, 1002)

    r.section("[18] GET /api/me/overview")
    resp = r.api("本人 → 200/0", "GET", "/api/me/overview", 200, 0, session=sa)
    r.info(f"totals={djson(resp, 'data.totals')} 难度条={dlen(resp, 'data.byDifficulty')} "
           f"标签条={dlen(resp, 'data.byTag')} recent={dlen(resp, 'data.recent')}")
    r.check("totals + byDifficulty + byTag + recent 四段都在",
            dget(resp, "data.totals") is not None and dlen(resp, "data.byDifficulty") != "-"
            and dlen(resp, "data.byTag") != "-" and dlen(resp, "data.recent") != "-",
            "结构齐全")
    r.api("匿名 → 401/1002", "GET", "/api/me/overview", 401, 1002)

    r.section("[19] GET /api/admin/overview")
    # 顺序不可颠倒：未登录先报 1002，已登录但无权才报 1003（报反了会让人以为自己登录了却没权限）
    r.api("匿名 → 401/1002（先判登录）", "GET", "/api/admin/overview", 401, 1002)
    r.api("非 admin → 403/1003（再判角色）", "GET", "/api/admin/overview", 403, 1003, session=sa)
    # T3.11（按 .env 同步 role）尚未接线，故这里直接改库把用户提成 admin 再验 200
    db.exec(f"UPDATE users SET role='admin' WHERE id={user_id_a};")
    resp = r.api("admin 重新登录后 → 200/0", "POST", "/api/auth/login", 200, 0, session=sa,
                 json={"username": user_a, "password": pass_a})
    r.check_eq("重新登录后的 role 是 admin", dget(resp, "data.role"), "admin")
    resp = r.api("admin → 200/0", "GET", "/api/admin/overview", 200, 0, session=sa)
    r.info(f"userCount={dget(resp, 'data.system.userCount')} "
           f"problemCount={dget(resp, 'data.system.problemCount')} "
           f"judgeAlive={dget(resp, 'data.system.judgeAlive')} "
           f"activeJudges={dget(resp, 'data.system.activeJudges')} "
           f"users[]={dlen(resp, 'data.users')} 项")
    r.check("system 四个字段 + users 列表都在",
            dget(resp, "data.system.userCount") is not None
            and dget(resp, "data.system.judgeAlive") is not None
            and dlen(resp, "data.users") != "-",
            "结构齐全")
    # 最容易记错的一条：admin 不绕过「仅本人」（API.md §5.2/§5.5、SPEC §6.3.1）——
    # 提权后读/删别人的提交仍然 1003。
    r.api("admin 读他人提交 → 仍是 403/1003", "GET", f"/api/submissions/{sub_b}", 403, 1003,
          session=sa)
    r.api("admin 删他人提交 → 仍是 403/1003", "DELETE", f"/api/submissions/{sub_b}", 403, 1003,
          session=sa)

    r.section("[5] POST /api/auth/password")
    resp = r.api("旧密码错 → 400/1001", "POST", "/api/auth/password", 400, 1001, session=sa,
                 json={"oldPassword": "wrong-one", "newPassword": "newpass1234"})
    field(r, resp, "oldPassword", "      ↑ data.field 指名参数")
    resp = r.api("新密码过短 → 400/1001", "POST", "/api/auth/password", 400, 1001, session=sa,
                 json={"oldPassword": pass_a, "newPassword": "123"})
    field(r, resp, "newPassword", "      ↑ data.field 指名参数")
    # 先用第二个设备登录一次：改密后它必须被踢掉（D-40），而当前会话必须还在
    r.api("第二设备登录 → 200/0", "POST", "/api/auth/login", 200, 0, session=sa2,
          json={"username": user_a, "password": pass_a})
    r.api("改密成功 → 200/0", "POST", "/api/auth/password", 200, 0, session=sa,
          json={"oldPassword": pass_a, "newPassword": "newpass1234"})
    r.api("改密后当前会话仍有效（D-40）→ 200/0", "GET", "/api/auth/me", 200, 0, session=sa)
    r.api("改密后其他设备已被踢 → 401/1002", "GET", "/api/auth/me", 401, 1002, session=sa2)
    r.api("用新密码登录 → 200/0", "POST", "/api/auth/login", 200, 0, session=sa2,
          json={"username": user_a, "password": "newpass1234"})
    r.api("用旧密码登录 → 400/1001", "POST", "/api/auth/login", 400, 1001,
          json={"username": user_a, "password": pass_a})

    r.section("[3] POST /api/auth/logout")
    resp = r.api("带 sid 登出 → 200/0", "POST", "/api/auth/logout", 200, 0, session=sa)
    r.check("登出响应清掉 Cookie", sa.cookies.get_dict().get("sid") in (None, ""),
            f"本地 jar 里 sid={sa.cookies.get_dict().get('sid', '（已无）')}")
    r.api("登出后 me → 401/1002", "GET", "/api/auth/me", 401, 1002, session=sa)
    r.api("重复登出（幂等）→ 200/0", "POST", "/api/auth/logout", 200, 0, session=sa)
    r.api("不带 Cookie 登出（幂等且不碰 DB）→ 200/0", "POST", "/api/auth/logout", 200, 0)

    r.section("[6] DELETE /api/auth/account")
    resp = r.api("密码错 → 400/1001", "DELETE", "/api/auth/account", 400, 1001, session=sa2,
                 json={"password": "not-the-password"})
    field(r, resp, "password", "      ↑ data.field 指名参数")
    r.api("注销成功 → 200/0", "DELETE", "/api/auth/account", 200, 0, session=sa2,
          json={"password": "newpass1234"})
    r.api("注销后 me → 401/1002", "GET", "/api/auth/me", 401, 1002, session=sa2)
    # 账号已被硬删除：用新密码登录仍应是 400/1001（用户不存在与口令错误同码同文案，防枚举，§6.3）
    r.api("注销后账号确实不存在 → 400/1001", "POST", "/api/auth/login", 400, 1001,
          json={"username": user_a, "password": "newpass1234"})



if __name__ == "__main__":
    sys.exit(main())
