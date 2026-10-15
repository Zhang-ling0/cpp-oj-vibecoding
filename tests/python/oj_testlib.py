#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CppOJ 接口自动化测试的公共库：HTTP 请求、断言、SQL、探针实例四件套。

被测服务是仓库里的 ``./build/oj``（监听地址见 ``.env`` 的 ``OJ_LISTEN``），契约真源是
``API.md``，验收流程见仓库根的《基于 curl 的接口自动化测试文档.md》。
本库是 ``tests/run_tests.sh`` 与 ``tests/run_problemsync_test.sh`` 的 Python 对应物，
两份脚本共同的期望口径（**HTTP 状态码 + 响应体里的 ``code`` 同时断言**）由
:func:`Reporter.api` 统一保证。

三条口径，缺一不可：

* **每个响应都断两件事**：HTTP 状态码与业务 ``code``。``[D-29]`` 要求两者同步，而它们出自
  两条不同的代码路径（``errors.h::http_status`` 与处理器里手写的 ``status``），
  只断一个就漏掉「401 的码配着 403 的状态」这类漂移。
* **失败必须打印响应体**。验收时满屏 ``code=null`` 的根因是响应根本不是 JSON（静态页或
  404 的 HTML），不打印前 400 字节就无从判断。
* **取不到的值显示 ``-`` 而不是崩掉**（对应 jq 的 ``// "-"``），并且 ``false`` / ``0`` /
  ``[]`` 这类假值必须照常显示 —— jq 的 ``//`` 会把 ``false`` 当空值，本库不用那套语义。

依赖：``requests``（``pip3 install -r tests/python/requirements.txt``）+ ``mysql`` 命令行
客户端（造测试数据用，验收脚本里只有它需要）。
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import Any, Mapping

try:
    import requests
except ImportError:  # pragma: no cover - 环境问题，给出可执行的修复提示
    sys.exit(
        "缺少依赖 requests。请先执行：pip3 install -r tests/python/requirements.txt"
    )

# tests/python/oj_testlib.py → tests/python → tests → 仓库根。
# ⚠️ 必须从仓库根执行：.env、problems/、static/、db/init/ 全都相对【当前工作目录】解析
#    （SPEC §2.1-A 第 2 条、§6.7.3）。这里兜个底，免得从别处调用时得到莫名其妙的失败。
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

__all__ = [
    "ROOT",
    "Resp",
    "Reporter",
    "Mysql",
    "ProbeServer",
    "load_dotenv",
    "dget",
    "dlen",
    "dtype",
    "djson",
    "jtype",
    "fmt",
]


# ---------------------------------------------------------------------------- 输出
_NO_COLOR = bool(os.environ.get("NO_COLOR")) or not sys.stdout.isatty()


def _paint(code: str, text: str) -> str:
    return text if _NO_COLOR else f"\033[{code}m{text}\033[0m"


# ------------------------------------------------------------------ .env / jq 等价件
_LINE = re.compile(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$")


def load_dotenv(path: str | None = None) -> dict[str, str]:
    """读 ``.env``。只做本项目需要的最小解析，不引第三方依赖。

    规则与 ``config.cpp`` 一致：``#`` 开头是注释；值可用引号包裹；引号后允许行内注释。
    已存在的进程环境变量优先（与服务的"环境变量覆盖 .env"同口径）。
    """
    path = path or os.path.join(ROOT, ".env")
    out: dict[str, str] = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw_lines = fh.readlines()
    except FileNotFoundError:
        return out
    for line in raw_lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = _LINE.match(line)
        if not m:
            continue
        key, value = m.group(1), m.group(2)
        if value[:1] in ("'", '"') and value[-1:] == value[:1] and len(value) >= 2:
            value = value[1:-1]
        else:
            value = value.split(" #", 1)[0].strip()
        out[key] = value
    return out


_TOKEN = re.compile(r"([^.\[\]]+)|\[(\d+)\]")


def _walk(obj: Any, path: str) -> Any:
    """按 jq 风格的路径取值：``data.list[0].slug``；取不到返回 ``None``。"""
    cur = obj
    for m in _TOKEN.finditer(path):
        key, idx = m.group(1), m.group(2)
        if key is not None:
            if not isinstance(cur, Mapping) or key not in cur:
                return None
            cur = cur[key]
        else:
            i = int(idx)
            if not isinstance(cur, (list, tuple)) or i >= len(cur):
                return None
            cur = cur[i]
    return cur


def dget(src: Any, path: str, default: Any = "-") -> Any:
    """从 :class:`Resp` 或已解析的 dict 里取值，取不到给 ``default``。"""
    body = src.body if isinstance(src, Resp) else src
    value = _walk(body, path)
    return default if value is None else value


def dlen(src: Any, path: str) -> int | str:
    """jq 的 ``path|length``；取不到或不是容器时返回 ``-``。"""
    value = _walk(src.body if isinstance(src, Resp) else src, path)
    return len(value) if isinstance(value, (list, tuple, Mapping, str)) else "-"


def dtype(src: Any, path: str) -> str:
    """jq 的 ``path|type``，用 jq 的类型名（null/boolean/number/string/array/object）。"""
    return jtype(_walk(src.body if isinstance(src, Resp) else src, path))


def djson(src: Any, path: str) -> str:
    """jq 的 ``path|tojson``；取不到给 ``-``。"""
    value = _walk(src.body if isinstance(src, Resp) else src, path)
    return "-" if value is None else json.dumps(value, ensure_ascii=False, sort_keys=True)


def jtype(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, (list, tuple)):
        return "array"
    if isinstance(value, Mapping):
        return "object"
    return "unknown"


def fmt(value: Any) -> str:
    """把值渲染成一行文本（``None`` → ``-``）；对应 jq 的 ``tostring``。"""
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return value
    return str(value)


# -------------------------------------------------------------------------- 响应对象
@dataclass
class Resp:
    """一次请求的结果：HTTP 状态、业务 code、解析后的包络、原始文本、响应头。"""

    http: int | str
    code: Any
    body: Any
    raw: str
    headers: Mapping[str, str] = field(default_factory=dict)

    def __str__(self) -> str:
        return f"HTTP {self.http} / code {self.code}"


def _parse_json(text: str) -> Any:
    try:
        return json.loads(text)
    except (ValueError, TypeError):
        return None


# ---------------------------------------------------------------------------- 断言器
class Reporter:
    """计数 + 打印的断言器。``exit_code()`` 是给 shell 用的退出码（0 = 全绿）。"""

    LABEL_WIDTH = 58

    def __init__(
        self,
        title: str,
        base: str,
        *,
        timeout: float = 10.0,
        strict_envelope: bool = False,
    ) -> None:
        self.title = title
        self.base = base.rstrip("/")
        self.timeout = timeout
        self.strict_envelope = strict_envelope
        self.passed = 0
        self.failed = 0
        self.envelope_warned = 0
        # 不带 Cookie 的请求共用这一个会话：登录等「写 Cookie」的用例一律显式传别的会话，
        # 所以这里永远不会带上任何身份，正好等价于 curl 不带 -b。
        self.anon = requests.Session()

    # -- 打印 -----------------------------------------------------------------
    def section(self, text: str) -> None:
        print(f"\n{_paint('1', text)}")

    def info(self, text: str) -> None:
        print(f"        {text}")

    def note(self, text: str) -> None:
        print(f"  {_paint('2', text)}")

    # -- 断言 -----------------------------------------------------------------
    def request(
        self,
        method: str,
        path: str,
        *,
        session: requests.Session | None = None,
        **kwargs: Any,
    ) -> Resp:
        """发一个请求并解析包络，**不做任何断言、不计数**（只取字段时用它）。

        关键字参数原样交给 ``requests``：``json=`` / ``data=`` / ``headers=`` / ``params=``。
        连接失败不当异常抛出，而是把 ``http`` 置成 ``ERR``，由断言那一侧统一报错。
        """
        sess = session if session is not None else self.anon
        try:
            raw_resp = sess.request(method, self.base + path, timeout=self.timeout, **kwargs)
            http, text, hdrs = raw_resp.status_code, raw_resp.text, raw_resp.headers
        except requests.RequestException as exc:
            http, text, hdrs = "ERR", f"{type(exc).__name__}: {exc}", {}
        body = _parse_json(text)
        code: Any = "null"
        if isinstance(body, Mapping) and body.get("code") is not None:
            code = body["code"]
        return Resp(http, code, body, text, hdrs)

    def api(
        self,
        label: str,
        method: str,
        path: str,
        want_http: int | str,
        want_code: Any,
        *,
        session: requests.Session | None = None,
        note: str = "",
        **kwargs: Any,
    ) -> Resp:
        """发一个请求，同时断言 HTTP 状态码与响应体里的 ``code``。"""
        resp = self.request(method, path, session=session, **kwargs)
        self._envelope(label, resp)
        self._record(label, str(resp.http) == str(want_http), str(resp.code) == str(want_code),
                     resp, want_http, want_code, note)
        return resp

    def static(
        self,
        label: str,
        path: str,
        want_http: int | str,
        *,
        session: requests.Session | None = None,
        note: str = "",
    ) -> Resp:
        """静态资源：HTML 没有 ``code``，只断状态码。"""
        resp = self.request("GET", path, session=session)
        resp.code = "-"
        self._record(label, str(resp.http) == str(want_http), True, resp, want_http, "-", note)
        return resp

    def check_eq(self, label: str, got: Any, want: Any, *, note: str = "") -> bool:
        """标量断言（比字段、比长度、比数量）。"""
        got_s, want_s = fmt(got), fmt(want)
        ok = got_s == want_s
        tail = f"（期望 {want_s}）" if not ok else ""
        mark = _paint("32", "PASS") if ok else _paint("31", "FAIL")
        suffix = f" {note}" if note else ""
        print(f"  {mark} {label:<{self.LABEL_WIDTH}} {got_s}{tail}{suffix}")
        self.passed += 1 if ok else 0
        self.failed += 0 if ok else 1
        return ok

    def check(self, label: str, ok: bool, detail: str = "") -> bool:
        mark = _paint("32", "PASS") if ok else _paint("31", "FAIL")
        print(f"  {mark} {label:<{self.LABEL_WIDTH}} {detail}")
        self.passed += 1 if ok else 0
        self.failed += 0 if ok else 1
        return ok

    # -- 包络检查（API.md §1.2：code / data / message 三键，data 永远是对象）----------
    def _envelope(self, label: str, resp: Resp) -> None:
        body = resp.body
        if not isinstance(body, Mapping):
            reason = "响应不是 JSON 对象（多半是静态页或 404 的 HTML）"
        elif not {"code", "data", "message"} <= set(body):
            reason = f"包络三键不全，实际 {sorted(body)}"
        elif not isinstance(body["data"], Mapping):
            reason = f"data 不是对象（实际 {jtype(body['data'])}）"
        else:
            return
        self.envelope_warned += 1
        if self.strict_envelope:
            self.check(f"[包络] {label}", False, reason)
        else:
            print(f"  {_paint('33', 'WARN')} [包络] {label}：{reason}"
                  f"（加 --strict 让它算失败）")

    # -- 内部 -----------------------------------------------------------------
    def _record(
        self,
        label: str,
        http_ok: bool,
        code_ok: bool,
        resp: Resp,
        want_http: Any,
        want_code: Any,
        note: str,
    ) -> None:
        ok = http_ok and code_ok
        mark = _paint("32", "PASS") if ok else _paint("31", "FAIL")
        suffix = f" {note}" if note else ""
        if ok:
            print(f"  {mark} {label:<{self.LABEL_WIDTH}} HTTP {resp.http} / code {fmt(resp.code)}{suffix}")
        else:
            print(f"  {mark} {label:<{self.LABEL_WIDTH}} HTTP {resp.http} / code "
                  f"{fmt(resp.code)}（期望 {want_http} / {want_code}）{suffix}")
            print(f"        响应体：{resp.raw[:400]}")
        self.passed += 1 if ok else 0
        self.failed += 0 if ok else 1

    def summary(self) -> int:
        color = "32" if self.failed == 0 else "31"
        print(f"\n=== 结果：通过 {self.passed}，失败 {self.failed} ===")
        if self.failed == 0 and self.envelope_warned:
            print(f"（另有 {self.envelope_warned} 条包络告警未计入失败，加 --strict 可使其失败）")
        return 0 if self.failed == 0 else 1

    def exit_code(self) -> int:
        return self.summary()


# ------------------------------------------------------------------------------- SQL
class Mysql:
    """走 ``mysql`` 命令行的极薄封装。

    不引 ``pymysql`` / ``mysql-connector``：验收脚本只用它造几行数据和核对一个 id，
    而本机的依赖清单里本来就有 ``mysql`` 客户端（文档 §2 的前置条件之一）。
    参数默认取自 ``.env``，与被测服务连的是同一个库。
    """

    def __init__(
        self,
        *,
        host: str = "127.0.0.1",
        port: int = 3306,
        user: str = "oj",
        password: str = "oj",
        database: str = "oj",
        binary: str | None = None,
    ) -> None:
        self.binary = binary or shutil.which("mysql")
        if not self.binary:
            sys.exit("需要 mysql 客户端（造测试数据用）。检查命令：mysql --version")
        self.host, self.port = host, int(port)
        self.user, self.password, self.database = user, password, database

    @classmethod
    def from_env(cls, env: Mapping[str, str] | None = None) -> "Mysql":
        env = env if env is not None else load_dotenv()
        get = lambda k, d: os.environ.get(k) or env.get(k, d)  # noqa: E731
        return cls(
            host=get("MYSQL_HOST", "127.0.0.1"),
            port=get("MYSQL_PORT", "3306"),
            user=get("MYSQL_USER", "oj"),
            password=get("MYSQL_PASSWORD", "oj"),
            database=get("MYSQL_DB", "oj"),
        )

    def _run(self, sql: str) -> str:
        proc = subprocess.run(
            [self.binary, "-h", self.host, "-P", str(self.port), "-u", self.user,
             f"-p{self.password}", self.database, "-N", "-B", "-e", sql],
            stdout=subprocess.PIPE, text=True, cwd=ROOT,
            # 口令写在命令行会打一行 "Using a password on the command line" 警告，无害，丢掉
            stderr=subprocess.DEVNULL,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"mysql 退出码 {proc.returncode}：{sql[:120]}")
        return proc.stdout

    def exec(self, sql: str) -> None:
        """执行 DDL/DML，忽略输出。"""
        self._run(sql)

    def scalar(self, sql: str) -> str:
        """取单值（``-N -B`` 的输出，去掉尾随空行）。"""
        lines = [ln for ln in self._run(sql).splitlines() if ln.strip()]
        return lines[-1].strip() if lines else ""

    def rows(self, sql: str) -> list[list[str]]:
        return [ln.split("\t") for ln in self._run(sql).splitlines() if ln.strip()]


# ---------------------------------------------------------------------- 探针实例
class ProbeServer:
    """拉一个**独立**的服务实例，供「改文件 → 重启 → 读侧反映」这类用例使用。

    对应 ``tests/run_problemsync_test.sh`` 的隔离设计：端口、日志目录、judge 工作区全部
    独立（⚠️ judge 工作区必须独立 —— 启动第 6 步会扫残并清空工作区，两个实例共用会互相删），
    MySQL 仍是同一个库（题目本来就在库里，没有别的选择）。

    所有相对路径都相对**仓库根**，故本类固定以 ``cwd=ROOT`` 启动进程。
    """

    def __init__(
        self,
        *,
        env_path: str,
        log_path: str,
        base: str,
        binary: str = "./build/oj",
        listen: str = "127.0.0.1:18080",
        log_dir: str | None = None,
        workdir: str | None = None,
        ready_path: str = "/api/tags",
        start_timeout: float = 20.0,
    ) -> None:
        self.env_path = env_path
        self.log_path = log_path
        self.base = base.rstrip("/")
        self.binary = binary
        self.listen = listen
        self.log_dir = log_dir
        self.workdir = workdir
        self.ready_path = ready_path
        self.start_timeout = start_timeout
        self._proc: subprocess.Popen | None = None
        self._log_fh = None

    # -- env ------------------------------------------------------------------
    def write_env(self) -> None:
        """从真 ``.env`` 复制后覆盖三个键：端口、日志目录、judge 工作区。"""
        os.makedirs(os.path.dirname(self.env_path), exist_ok=True)
        overrides = {"OJ_LISTEN": self.listen}
        if self.log_dir:
            overrides["OJ_LOG_DIR"] = self.log_dir
        if self.workdir:
            overrides["OJ_JUDGE_WORKDIR"] = self.workdir
        with open(os.path.join(ROOT, ".env"), "r", encoding="utf-8") as fh:
            text = fh.read()
        for key, value in overrides.items():
            pattern = re.compile(rf"^{key}=.*$", re.MULTILINE)
            line = f"{key}={value}"
            text = pattern.sub(line, text) if pattern.search(text) else text.rstrip() + f"\n{line}\n"
        with open(self.env_path, "w", encoding="utf-8") as fh:
            fh.write(text)
        for extra in (self.log_dir, self.workdir):
            if extra:
                os.makedirs(extra, exist_ok=True)

    # -- 生命周期 --------------------------------------------------------------
    def start(self) -> None:
        self.stop()
        self.write_env()
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        self._log_fh = open(self.log_path, "a", encoding="utf-8")
        self._proc = subprocess.Popen(
            [self.binary, self.env_path], cwd=ROOT,
            stdout=self._log_fh, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + self.start_timeout
        while time.monotonic() < deadline:
            if self._proc.poll() is not None:
                break
            try:
                requests.get(self.base + self.ready_path, timeout=1.0)
                return
            except requests.RequestException:
                time.sleep(0.25)
        tail = ""
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as fh:
                tail = "".join(fh.readlines()[-20:])
        except OSError:
            pass
        self.stop()
        raise RuntimeError(f"探针实例启动失败，见 {self.log_path} 的最后 20 行：\n{tail}")

    def stop(self) -> None:
        proc, self._proc = self._proc, None
        if proc is not None and proc.poll() is None:
            proc.send_signal(signal.SIGINT)          # 与脚本一致：先礼后兵
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        if self._log_fh is not None:
            self._log_fh.close()
            self._log_fh = None

    def restart(self) -> None:
        self.start()

    def __enter__(self) -> "ProbeServer":
        self.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()

    def log_tail(self, lines: int = 20) -> str:
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as fh:
                return "".join(fh.readlines()[-lines:])
        except OSError:
            return ""


# --------------------------------------------------------------------- 小工具函数
def wait_for(base: str, path: str = "/api/tags", timeout: float = 10.0) -> bool:
    """等一个已在跑的服务就绪（题库同步要先跑完，所以要给它时间）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            requests.get(base.rstrip("/") + path, timeout=1.0)
            return True
        except requests.RequestException:
            time.sleep(0.25)
    return False


def write_json_file(path: str, payload: Mapping[str, Any]) -> None:
    """写题库 JSON（UTF-8、不转义中文，与 heredoc 版行为一致）。"""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
