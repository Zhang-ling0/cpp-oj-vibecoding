#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CppOJ 题库链路验收（增 / 改 / 写坏 / 修回 / 删文件）—— ``tests/run_problemsync_test.sh``
的 Python 对应物。

为什么单独一个脚本，而不是并进 :mod:`run_api_tests`：

* 题库同步只在**启动时**跑一次（SPEC D-30：改题必须重启），所以每一步都要重启服务；
* 它要改 ``problems/`` 下的文件 —— 与契约验收「只读 + 随机账号」的性质不同，混在一起会
  互相踩。契约脚本必须能随时安全地跑，故这里独立。

**隔离：脚本自己拉一个独立实例，绝不碰你正在跑的服务**::

    ./build/oj /tmp/oj-probe/probe.env     # OJ_LISTEN=127.0.0.1:18080
                                              # OJ_LOG_DIR=/tmp/oj-probe/logs
                                              # OJ_JUDGE_WORKDIR=/tmp/oj-probe/judge

* judge 工作区**必须独立**：启动第 6 步会扫残并清空工作区里的残留目录，两个实例共用
  会互相删对方的。
* MySQL 仍是同一个库（题目本来就在库里，没有别的选择）；探针题的行由脚本在收尾时删净。
* **只动专用探针题** ``problems/sync-probe-2026.json``，绝不碰 ``two-sum.json``。

用法（从仓库根目录）::

    python3 tests/python/run_problemsync_tests.py

可调项：``OJ_PROBE_DIR``（默认 ``/tmp/oj-probe``）、``OJ_PROBE_PORT``（默认 18080）、
``--strict``（把包络不合规也算失败）。
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from typing import Any, Mapping  # noqa: E402

from oj_testlib import (  # noqa: E402
    ROOT,
    Mysql,
    ProbeServer,
    Reporter,
    dget,
    dlen,
    write_json_file,
)

PROBE_SLUG = "sync-probe-2026"          # 带日期后缀：万一没清干净也不像真题


# ------------------------------------------------------------------ 探针题的三个版本
def probe_json_v1() -> dict:
    """初版：两个用例。"""
    return {
        "slug": PROBE_SLUG,
        "title": "探针题·初版",
        "difficulty": "easy",
        "is_featured": False,
        "sort_no": 99,
        "tags": {"algorithm": ["数组"]},
        "statement_md": "## 探针\n\n这是验收脚本造的临时题目。",
        "hint_md": "不用想。",
        "signature": "class Solution {\npublic:\n    int add(int a, int b);\n};",
        "method_name": "add",
        "params": [{"name": "a", "cpp_type": "int"}, {"name": "b", "cpp_type": "int"}],
        "return_type": "int",
        "code_template": "class Solution {\npublic:\n    int add(int a, int b) {\n        \n    }\n};",
        "time_limit_ms": 2000,
        "mem_limit_kb": 131072,
        "cases": [
            {"args": [1, 2], "expected": 3, "brief": "正数"},
            {"args": [-1, 1], "expected": 0, "brief": "含负数"},
        ],
    }


def probe_json_v2() -> dict:
    """改后：标题、难度、标签、用例数、时限都变了（验 UPDATE + 子表刷新 + id 不变）。"""
    return {
        "slug": PROBE_SLUG,
        "title": "探针题·改后",
        "difficulty": "hard",
        "is_featured": True,
        "sort_no": 98,
        "tags": {"algorithm": ["数组", "动态规划"]},
        "statement_md": "## 探针（已改）\n\n这一版改了标题、难度、标签和用例数。",
        "hint_md": "还是不用想。",
        "signature": "class Solution {\npublic:\n    int add(int a, int b);\n};",
        "method_name": "add",
        "params": [{"name": "a", "cpp_type": "int"}, {"name": "b", "cpp_type": "int"}],
        "return_type": "int",
        "code_template": ("class Solution {\npublic:\n    int add(int a, int b) {\n"
                          "        return a + b;\n    }\n};"),
        "time_limit_ms": 2500,
        "mem_limit_kb": 65536,
        "cases": [
            {"args": [1, 2], "expected": 3, "brief": "正数"},
            {"args": [-1, 1], "expected": 0, "brief": "含负数"},
            {"args": [0, 0], "expected": 0, "brief": "零"},
        ],
    }


def probe_json_broken() -> dict:
    """写坏：删掉 ``return_type``（json_schema 必填）→ 校验不通过 → 跳过入库 + 标记失效。"""
    payload = probe_json_v2()
    payload.pop("return_type")
    return payload


# ------------------------------------------------------------------------------ 主流程
def main() -> int:
    probe_dir = os.environ.get("OJ_PROBE_DIR") or os.path.join(
        os.environ.get("TMPDIR", "/tmp"), "oj-probe")
    probe_port = int(os.environ.get("OJ_PROBE_PORT", "18080"))
    probe_base = f"http://127.0.0.1:{probe_port}"
    probe_file = os.path.join(ROOT, "problems", f"{PROBE_SLUG}.json")

    db = Mysql.from_env()
    server = ProbeServer(
        env_path=os.path.join(probe_dir, "probe.env"),
        log_path=os.path.join(probe_dir, "server.log"),
        base=probe_base,
        binary=os.path.join(ROOT, "build", "oj"),
        listen=f"127.0.0.1:{probe_port}",
        log_dir=os.path.join(probe_dir, "logs"),
        workdir=os.path.join(probe_dir, "judge"),
    )

    print("=== 题库增/改/写坏/删 端到端验收 ===")
    print(f"仓库：{ROOT}")
    print(f"探针：problems/{PROBE_SLUG}.json（slug={PROBE_SLUG}）  实例：{probe_base}")

    if not os.path.exists(os.path.join(ROOT, "build", "oj")):
        print("没有 ./build/oj：先执行 make build JOBS=1", file=sys.stderr)
        return 2

    r = Reporter("题库链路", probe_base, strict_envelope="--strict" in sys.argv)
    try:
        run_all(r, db, server, probe_file)
    finally:
        server.stop()
        if os.path.exists(probe_file):
            os.remove(probe_file)
        pid = db.scalar(f"SELECT id FROM problems WHERE slug='{PROBE_SLUG}';")
        if pid:
            # 先删提交再删题：外键是 ON DELETE CASCADE，但显式写出来更清楚意图
            db.exec(f"DELETE FROM submissions WHERE problem_id={pid};"
                    f"DELETE FROM problems WHERE id={pid};")
        r.summary()
    return 0 if r.failed == 0 else 1


def run_all(r: Reporter, db: Mysql, server: ProbeServer, probe_file: str) -> None:
    # 下面几个取字段的小工具都走【不发断言】的 Reporter.request —— 否则每取一个字段
    # 就会多出一条重复的 200/0，条数冲得没法和文档对账。

    def detail_field(slug: str, field: str) -> Any:
        """读详情里的某字段（取不到就是 ``-``）；核对题库改动是否真反映到 HTTP 侧。"""
        return dget(r.request("GET", f"/api/problems/{slug}"), f"data.{field}")

    def list_field(slug: str, field: str) -> Any:
        """列表页里某 slug 的某字段（取不到就是 ``-``）。"""
        body = r.request("GET", "/api/problems").body
        data = body.get("data") if isinstance(body, dict) else None
        rows = (data or {}).get("list") or []
        for row in rows:
            if isinstance(row, dict) and row.get("slug") == slug:
                return row.get(field, "-")
        return "-"

    def tag_names_of(slug: str) -> list[str]:
        """详情里的 tags 是 ``[{category, name}, …]``，取名字列表。"""
        tags = dget(r.request("GET", f"/api/problems/{slug}"), "data.tags", default=[])
        return sorted(t["name"] for t in tags
                      if isinstance(t, Mapping) and "name" in t)

    def total() -> Any:
        return dget(r.request("GET", "/api/problems"), "data.total")

    def params_len(slug: str) -> Any:
        return dlen(r.request("GET", f"/api/problems/{slug}"), "data.params")

    # ---- 0. 基线：只有 two-sum ------------------------------------------------------
    r.section("[0] 基线（题库里只有 two-sum）")
    if os.path.exists(probe_file):
        os.remove(probe_file)
    server.start()
    r.check_eq("列表 total（基线，应为 1）", total(), "1")
    r.api("two-sum 详情可读", "GET", "/api/problems/two-sum", 200, 0)

    # ---- 1. 增 ---------------------------------------------------------------------
    r.section("[1] 增：新增 problems/{0}.json → 重启".format(PROBE_SLUG))
    write_json_file(probe_file, probe_json_v1())
    server.restart()
    r.check_eq("列表 total（应为 2）", total(), "2")
    r.api("新题详情 → 200/0", "GET", f"/api/problems/{PROBE_SLUG}", 200, 0)
    r.check_eq("新题 title", detail_field(PROBE_SLUG, "title"), "探针题·初版")
    r.check_eq("新题 difficulty", detail_field(PROBE_SLUG, "difficulty"), "easy")
    r.check_eq("新题 caseCount", detail_field(PROBE_SLUG, "caseCount"), "2")
    probe_id_v1 = db.scalar(f"SELECT id FROM problems WHERE slug='{PROBE_SLUG}';")
    r.info(f"库里的 problems.id = {probe_id_v1}")

    # ---- 2. 改 ---------------------------------------------------------------------
    r.section("[2] 改：改标题/难度/标签/用例数 → 重启")
    write_json_file(probe_file, probe_json_v2())
    server.restart()
    r.api("改后详情 → 200/0", "GET", f"/api/problems/{PROBE_SLUG}", 200, 0)
    r.check_eq("改后 title", detail_field(PROBE_SLUG, "title"), "探针题·改后")
    r.check_eq("改后 difficulty", detail_field(PROBE_SLUG, "difficulty"), "hard")
    r.check_eq("改后 caseCount（用例数 2→3）", detail_field(PROBE_SLUG, "caseCount"), "3")
    r.check_eq("改后 timeLimitMs", detail_field(PROBE_SLUG, "timeLimitMs"), "2500")
    r.check_eq("列表里的 title 同步更新", list_field(PROBE_SLUG, "title"), "探针题·改后")
    r.check_eq("列表里的 difficulty 同步更新", list_field(PROBE_SLUG, "difficulty"), "hard")
    probe_id_v2 = db.scalar(f"SELECT id FROM problems WHERE slug='{PROBE_SLUG}';")
    # ⚠️ id 不变 = 走的是 UPDATE 而不是「删行再插」（删行会 CASCADE 掉用户的提交历史，
    #   题库加载器刻意不这么做，见 loader.h【为什么不删"文件已删除"的题】）
    r.check_eq("problems.id 未变（证明是 UPDATE 不是删+插）", probe_id_v2, probe_id_v1)
    # 子表（用例/形参/标签）是整体刷新的：改后应同时含「数组」「动态规划」，顺序不保证
    r.check_eq("改后标签刷新（数组 + 动态规划）",
               "、".join(tag_names_of(PROBE_SLUG)), "、".join(sorted(["数组", "动态规划"])))

    # ---- 3. 写坏（校验不通过）-------------------------------------------------------
    r.section("[3] 写坏：删掉必填的 return_type → 重启")
    write_json_file(probe_file, probe_json_broken())
    server.restart()
    r.api("坏题详情仍然 200/0（行没被删）", "GET", f"/api/problems/{PROBE_SLUG}", 200, 0)
    r.check_eq("坏题 caseCount 被置 0", detail_field(PROBE_SLUG, "caseCount"), "0")
    r.check_eq("坏题 params 被清空", detail_field(PROBE_SLUG, "params"), "[]")
    # ⚠️ 预期：它【仍出现在列表页】。列表 SQL 是裸 FROM problems（problem_api.cpp:564），
    #   没有任何「失效题」过滤；loader.cpp 自己的注释也是这个口径（「会在列表页显示为
    #   case_count=0 的空题」），而 loader.h 里「列表页与缓存都不再出现它」与实现不符。
    r.check_eq("坏题仍在列表里（caseCount=0）", list_field(PROBE_SLUG, "caseCount"), "0")
    # 启动回显里那一行就是「跳过 N 题，标记失效 M 题」的实据（日志是追加的，只取最后一行）
    sync_lines = [ln for ln in server.log_tail(400).splitlines() if "题库同步" in ln]
    r.info("启动回显：" + (sync_lines[-1] if sync_lines else "（没找到题库同步那一行）"))

    # ---- 3b. 修回合法（证明「标记失效」是可恢复的，不是终态）-------------------------
    r.section("[3b] 修回合法 JSON → 重启（标记失效应当可恢复）")
    write_json_file(probe_file, probe_json_v2())
    server.restart()
    r.api("修回后详情 → 200/0", "GET", f"/api/problems/{PROBE_SLUG}", 200, 0)
    r.check_eq("用例数恢复（0→3）", detail_field(PROBE_SLUG, "caseCount"), "3")
    r.check_eq("形参恢复（[]→2 个）", params_len(PROBE_SLUG), 2)

    # ---- 4. 删文件（从【合法状态】删，才测得到"删文件对库做了什么"）------------------
    r.section(f"[4] 删文件：从合法状态删掉 {PROBE_SLUG}.json → 重启")
    os.remove(probe_file)
    server.restart()
    r.api("文件删掉后详情仍可读（行原样保留）", "GET", f"/api/problems/{PROBE_SLUG}", 200, 0)
    r.check_eq("用例没有被清空（删文件【不】触发 deactivate）",
               detail_field(PROBE_SLUG, "caseCount"), "3")
    r.check_eq("形参没有被清空", params_len(PROBE_SLUG), 2)
    r.check_eq("标题仍是删除前的那一版", detail_field(PROBE_SLUG, "title"), "探针题·改后")
    r.note("deactivate_problem 只在「JSON 校验不通过」时触发（loader.cpp:316）。")
    r.note("文件被删时连 slug 都读不到，加载器什么都不做 —— 这是刻意的：删 problems 行会 "
           "CASCADE 掉所有用户在该题上的提交历史（loader.h）。")
    r.note("代价是「删了文件的题仍出现在列表页」，这与【改坏】的处置不同，两者别混为一谈。")

    # ---- 5. 收尾：先证明"行确实留在库里"，再显式删行 → 回到基线 ----------------------
    r.section("[5] 收尾：显式删掉库里的探针行 → 重启（题库回到只剩 two-sum）")
    r.check_eq("删文件后 total 仍是 2（行没被删）", total(), "2")
    db.exec(f"DELETE FROM submissions WHERE problem_id IN "
            f"(SELECT id FROM problems WHERE slug='{PROBE_SLUG}');"
            f"DELETE FROM problems WHERE slug='{PROBE_SLUG}';")
    server.restart()
    r.check_eq("显式删行后 total 回到 1", total(), "1")
    r.api("two-sum 仍然可读", "GET", "/api/problems/two-sum", 200, 0)


if __name__ == "__main__":
    sys.exit(main())
