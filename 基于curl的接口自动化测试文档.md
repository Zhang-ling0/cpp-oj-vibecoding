# 基于 curl 的接口自动化测试文档

> 与 `API.md` 的分工：`API.md` 说「接口应该长什么样」，本文说「怎么反复验证它们真的长这样」。
> 本文里的每一条命令都可以直接复制执行，且**期望值**（HTTP 状态码 + 响应里的 `code`）都写在了旁边。

| 项 | 值 |
|---|---|
| 文档版本 | v1.0 |
| 生成日期 | 2026-10-15 |
| 适用代码版本 | `c423258` 之后（V2.7：全部端点接线 + 题库链路验收） |
| 被测服务 | `http://127.0.0.1:8080`（`.env` 的 `OJ_LISTEN`） |
| 契约来源 | `API.md`（错误码与响应体形状的唯一真源） |
| 脚本来源 | `tests/run_tests.sh`、`tests/run_problemsync_test.sh`、`tests/run_unit_tests.sh` |

---

## 目录

1. [三分钟上手](#1-三分钟上手)
2. [前置条件](#2-前置条件)
3. [三套脚本的分工](#3-三套脚本的分工)
4. [角色 × 端点 的期望码矩阵](#4-角色--端点-的期望码矩阵)
5. [错误码 → HTTP 速查](#5-错误码--http-速查)
6. [手动 curl 速查（按端点分组）](#6-手动-curl-速查按端点分组)
7. [一次性准备：管理员账号](#7-一次性准备管理员账号)
8. [题库的增 / 改 / 写坏 / 删文件](#8-题库的增--改--写坏--删文件)
9. [完整回归流程](#9-完整回归流程)
10. [故障排查](#10-故障排查)
11. [当前未覆盖的部分](#11-当前未覆盖的部分)

---

## 1. 三分钟上手

```bash
# 0) 构建（务必带 JOBS=1：本机 2 核 2G，并发编译会进 swap/OOM）
cd /home/ZL/project/cpp-oj-vibcoding
make build JOBS=1

# 1) 起服务（后台；make run 会顺带确保 mysqld 已启动）
(setsid nohup ./build/oj > /tmp/oj-run.log 2>&1 < /dev/null &)
sleep 5 && tail -3 /tmp/oj-run.log        # 看到 "HTTP 监听 http://0.0.0.0:8080" 即就绪

# 2) 跑接口验收（20 个端点 + 错误分支，98 条断言）
bash tests/run_tests.sh                    # 末尾输出 "通过 98，失败 0"

# 3) 跑题库链路验收（增/改/写坏/删文件 + 重启，30 条断言；自拉独立实例）
bash tests/run_problemsync_test.sh

# 4) 停服务
pkill -x oj
```

> ⚠️ **一定要从仓库根目录执行**。`.env`、`db/init/01-schema.sql`、`problems/`、`static/`
> 全都相对当前工作目录解析，在别处执行会得到莫名其妙的失败。
>
> ⚠️ **停服务只能用 `pkill -x oj`**。`pkill -f "build/oj"` 会连当前 shell 一起匹配上，
> 结果是命令把自己的父进程杀了、终端卡住（本次开发踩过）。

---

## 2. 前置条件

| 依赖 | 用途 | 检查命令 |
|---|---|---|
| `jq` | 从响应里取 `code` / `data.*` 做断言 | `jq --version` |
| `mysql` 客户端 | 造测试数据（提交行、用户）、核对密文 | `mysql --version` |
| `mysqld` | 被测服务的数据源 | `mysqladmin -h127.0.0.1 -uoj -poj ping` |
| `g++-12` | 构建 | `g++-12 --version` |
| 端口 8080 空闲 | 被测服务 | `ss -ltn \| grep 8080` |

数据库连接参数固定在 `.env`：`127.0.0.1:3306`，用户 `oj`，口令 `oj`，库 `oj`。

**资源约束（2 核 2G 的实测结论）**：

- 构建一律 `JOBS=1`（或 `make build JOBS=2`）。`tests/run_unit_tests.sh` 里写的是 `cmake --build -j`，
  内存不够时**不要**直接用它，用 `make build JOBS=1 && ctest --test-dir build -j1`。
- `ctest` 不要加 `-j`：417 条用例串行约 43 秒，并发只会和构建、mysqld 抢内存。
- 验收脚本本身很轻（只有 curl + jq + mysql），随便跑。

---

## 3. 三套脚本的分工

| 脚本 | 层面 | 会不会重启服务 | 会不会改 `problems/` | 用例数 | 期望结果 |
|---|---|---|---|---|---|
| `tests/run_unit_tests.sh` | 纯逻辑 + 业务层 SQL（gtest） | 否 | 否 | 417 | 全绿（1 条按设计跳过） |
| `tests/run_tests.sh` | **HTTP 契约**：路由、Cookie、包络、错误收口 | 否 | 否 | 98 | 通过 98，失败 0 |
| `tests/run_problemsync_test.sh` | **题库链路**：改文件 → 重启 → 读侧反映 | 是（自拉独立实例） | 是（专用探针题） | 30 | 通过 30，失败 0 |

三者的分工不是重叠而是**缺一不可**：路由接错、Cookie 没带上、错误码与 HTTP 状态不同步，
这三类问题只有 `curl` 能验；业务层 SQL 的口径只有 gtest 能钉；题库只在启动时同步，只有重启能验。

**`tests/run_tests.sh` 的可调项**

| 变量 | 默认 | 说明 |
|---|---|---|
| `OJ_BASE` | `http://127.0.0.1:8080` | 指向别的实例（例如跑在别的端口的服务） |

**`tests/run_problemsync_test.sh` 的隔离设计**（怕它影响你正在跑的服务）

它自己拉一个独立实例，端口、日志目录、judge 工作区全部独立：

```
./build/oj /tmp/oj-probe/probe.env     # OJ_LISTEN=127.0.0.1:18080
                                         # OJ_LOG_DIR=/tmp/oj-probe/logs
                                         # OJ_JUDGE_WORKDIR=/tmp/oj-probe/judge
```

- judge 工作区**必须独立**：启动第 6 步会扫残并清空工作区，两个实例共用会互相删。
- MySQL 仍是同一个库（题目本来就在库里，没有别的选择）；探针题的行由脚本在收尾时删净。
- 只动专用探针题 `problems/sync-probe-2026.json`，**绝不碰 `two-sum.json`**。

---

## 4. 角色 × 端点 的期望码矩阵

这张表是排错时最常用的：同一个请求，不同身份应该拿到什么码。

| 端点 | 匿名 | 普通用户 | 他人的资源 | admin |
|---|---|---|---|---|
| `POST /api/auth/register` | `201/0` | — | — | — |
| `POST /api/auth/login` | `200/0` 或 `400/1001` | — | — | — |
| `POST /api/auth/logout` | `200/0`（幂等） | 同左 | — | 同左 |
| `GET /api/auth/me` | `401/1002` | `200/0` | — | `200/0`（`role=admin`） |
| `POST /api/auth/password` | `401/1002` | `200/0`；旧密码错 `400/1001 field=oldPassword` | — | 同普通 |
| `DELETE /api/auth/account` | `401/1002` | `200/0`；密码错 `400/1001 field=password` | — | 同普通 |
| `GET /api/tags` | `200/0` | `200/0` | — | `200/0` |
| `GET /api/problems` | `200/0`；带 `progress`/`favorite=1` → `400/1001` | `200/0`（含 `myProgress`/`isFavorite`） | — | `200/0` |
| `GET /api/problems/{slug}` | `200/0`（**不下发** `nextRecommendation`） | `200/0`（带最近提交与推荐） | — | `200/0` |
| `POST /api/problems/{slug}/favorite` | `401/1002` | `200/0 {isFavorite}` | — | `200/0` |
| `POST /api/submissions` | `401/1002` | `503/1009`（judge 未接线，见 §11） | — | 同普通 |
| `GET /api/submissions` | `401/1002` | `200/0`（仅本人） | — | 同普通（**admin 也不放开**） |
| `GET /api/submissions/{id}` | `401/1002` | `200/0` | `403/1003` | 同普通 |
| `GET /api/submissions/diff` | `401/1002` | `200/0` | `403/1003` | 同普通 |
| `DELETE /api/submissions/{id}` | `401/1002` | `200/0` | `403/1003` | 同普通 |
| `POST /api/submissions/{id}/retry` | `401/1002` | 非 failed/JE → `409/1011`；failed/JE → `503/1009` | `403/1003` | 同普通 |
| `GET /api/stats` | `401/1002` | `200/0` | — | `200/0` |
| `GET /api/me/overview` | `401/1002` | `200/0` | — | `200/0` |
| `GET /api/admin/overview` | `401/1002` | `403/1003` | — | `200/0` |
| `GET /*`（静态） | `200`（不存在 → `404/1004`） | 同左 | — | 同左 |

两条容易记错的：

- **`admin` 不绕过「仅本人」**（API.md §5.2 / §5.5 / SPEC §6.3.1）。admin 读他人提交仍然是 `1003`。
- **未登录 `1002`、已登录但无权 `1003`，顺序不可颠倒**：`/api/admin/overview` 对匿名必须报 `1002`，
  报成 `1003` 会让人以为自己登录了却没权限。

---

## 5. 错误码 → HTTP 速查

断言时**两个都要查**：HTTP 状态码与响应体里的 `code` 必须一致（API.md `[D-29]`）。

```
code    HTTP   含义
0       200    成功（201 仅 POST /api/auth/register 与 …/retry）
1001    400    参数校验失败（data.field 指名参数）
1002    401    未登录或会话过期
1003    403    无权限
1004    404    资源不存在（含：路由不存在、静态文件不存在、方法不对）
1005    409    用户名已注册
1006    429    每分钟提交数超限（限流未实现，当前不可达）
1007    429    同时评测数超限（同上）
1008    500    服务器内部错误（详情进 logs/app.log 的 ERROR）
1009    503    判题服务不可用（judge 未接线时 POST 提交/重跑走这个）
1010    403    注册已关闭（OJ_ALLOW_REGISTER=0）
1011    409    当前状态不允许（重跑非 failed/JE 的提交）
2001-2005 200  判题结论（CE/TLE/MLE/WA/RE），不是 API 异常
```

统一包络（成功与失败都有 `data`，永远是对象）：

```json
{"code": 0, "data": {...}, "message": "ok"}
{"code": 1001, "data": {"field": "page", "reason": "page 必须是 ≥ 1 的纯数字"}, "message": "参数校验失败"}
```

> 三个键的输出顺序是字典序 `code → data → message`（nlohmann 内部是 `std::map`），
> 与 SPEC 示例里手写的顺序不同。JSON 无序，不影响任何客户端。

---

## 6. 手动 curl 速查（按端点分组）

准备（一次性）：

```bash
BASE=http://127.0.0.1:8080
U=demo$$                          # 用户名只允许 [A-Za-z0-9_-]
curl -s -X POST $BASE/api/auth/register -H 'Content-Type: application/json' \
  -d "{\"username\":\"$U\",\"password\":\"pass1234\"}"          # 201/0；重复注册 409/1005
curl -s -c /tmp/oj.jar -X POST $BASE/api/auth/login -H 'Content-Type: application/json' \
  -d "{\"username\":\"$U\",\"password\":\"pass1234\"}"          # 200/0 + Set-Cookie: sid=…
```

> **阶段 0 之后库里没有任何提交行**（`oj-reset` 不再伪造判题结果，见 P0-4）。
> 提交类端点（详情 / diff / 删除 / 重跑）要先拿到 `id` 才能验，取法二选一：
>
> 1. **判题引擎接线后**：真提交一条，再从列表取 id。
>    ```bash
>    SID=$(curl -s -b /tmp/oj.jar "$BASE/api/submissions" | jq -r '.data.list[0].id // empty')
>    ```
> 2. **接线前**：跑 `bash tests/run_tests.sh`（它自己用 SQL 造行，用完自清），
>    或手工插一条（与脚本同一写法）：
>    ```bash
>    SID=$(mysql -h127.0.0.1 -uoj -poj oj -N -e \
>      "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict, message,
>        case_passed, case_total, total_ms, max_mem_kb, created_at, finished_at)
>       SELECT u.id, p.id, 'cpp', 'SELECT 1;', 'done', 'AC', NULL, 1, 1, 1, 1024,
>              UTC_TIMESTAMP(), UTC_TIMESTAMP()
>       FROM users u, problems p WHERE u.username='${U}' AND p.slug='two-sum';
>       SELECT LAST_INSERT_ID();" 2>/dev/null | tail -1)
>    ```
>
> 空库（`SID` 为空）时，「不存在的 id」那几条断言照样可验：直接用 `999999999` 之类。

| 端点 | 命令 | 期望 |
|---|---|---|
| 我是谁 | `curl -s -b /tmp/oj.jar $BASE/api/auth/me` | `200/0`；不带 jar → `401/1002` |
| 登出 | `curl -s -b /tmp/oj.jar -X POST $BASE/api/auth/logout` | `200/0` + 清 Cookie；可重复执行 |
| 标签 | `curl -s $BASE/api/tags` | `200/0`，`data.algorithm` / `data.source` |
| 题库列表 | `curl -s "$BASE/api/problems?size=3"` | `200/0`，回显 `total/page/size` |
| 题库筛选 | `curl -s "$BASE/api/problems?difficulty=EASY&sort=featured"` | `200/0`（枚举大小写不敏感） |
| 筛选非法 | `curl -s "$BASE/api/problems?difficulty=nope"` | `400/1001`，`data.field="difficulty"` |
| 分页非法 | `curl -s "$BASE/api/problems?page=0"` | `400/1001`，`data.field="page"` |
| 分页夹取 | `curl -s "$BASE/api/problems?size=1000"` | `200/0`，`data.size` 回显 `100` |
| 匿名个性化 | `curl -s "$BASE/api/problems?progress=solved"` | `400/1001`（匿名不得按进度筛） |
| 题库详情 | `curl -s $BASE/api/problems/two-sum` | `200/0`；未登录时**没有** `nextRecommendation` 这个键 |
| 详情 404 | `curl -s $BASE/api/problems/nope` | `404/1004` |
| 收藏 | `curl -s -b /tmp/oj.jar -X POST $BASE/api/problems/two-sum/favorite` | `200/0 {isFavorite}`，**再调一次翻转** |
| 统计 | `curl -s -b /tmp/oj.jar $BASE/api/stats` | `200/0` 四个标量 |
| 概览 | `curl -s -b /tmp/oj.jar $BASE/api/me/overview` | `200/0`：`totals` + `byDifficulty` + `byTag` + `recent` |
| 管理面板 | `curl -s -b /tmp/oj.jar $BASE/api/admin/overview` | admin `200/0`；普通用户 `403/1003`；匿名 `401/1002` |
| 提交 | `curl -s -b /tmp/oj.jar -X POST $BASE/api/submissions -H 'Content-Type: application/json' -d '{"problemSlug":"two-sum","code":"class Solution{};"}'` | judge 未接线 → `503/1009` |
| 提交历史 | `curl -s -b /tmp/oj.jar "$BASE/api/submissions?verdict=AC&page=1"` | `200/0`，仅本人数据；**空库时 `data.total=0`、`data.list=[]`**（阶段 0 的正确状态） |
| 提交详情 | `curl -s -b /tmp/oj.jar $BASE/api/submissions/$SID`（`SID` 见上方取法） | `200/0`；别人的 `403/1003`；不存在 `404/1004` |
| diff | `curl -s -b /tmp/oj.jar "$BASE/api/submissions/diff?from=${SID}&to=${SID2}"` | `200/0`；`from==to` → `400/1001`（`SID2` 取另一条本人提交） |
| 删提交 | `curl -s -b /tmp/oj.jar -X DELETE $BASE/api/submissions/$SID` | `200/0`；别人的 `403/1003` |
| 重跑 | `curl -s -b /tmp/oj.jar -X POST $BASE/api/submissions/$SID/retry` | 非 failed/JE → `409/1011`；**judge 接线前 failed/JE → `503/1009`** |
| 改密码 | `curl -s -b /tmp/oj.jar -X POST $BASE/api/auth/password -H 'Content-Type: application/json' -d '{"oldPassword":"pass1234","newPassword":"newpass1234"}'` | `200/0`；旧密码错 → `400/1001 field=oldPassword` |
| 注销账号 | `curl -s -b /tmp/oj.jar -X DELETE $BASE/api/auth/account -H 'Content-Type: application/json' -d '{"password":"newpass1234"}'` | `200/0`，随后该账号登录得 `400/1001` |
| 静态 | `curl -s -o /dev/null -w '%{http_code}\n' $BASE/login.html` | `200`；不存在 → `404/1004` |
| 路由不存在 | `curl -s $BASE/api/nope` | `404/1004` |

---

## 7. 一次性准备：管理员账号

管理端点是只读的（SPEC §6.3.4：不做在线编辑题库、不做 impersonation），**管理员身份只能由
`.env` 的 `OJ_ADMIN_USERNAMES` 声明式指定**。但启动角色同步（T3.11）尚未接线，所以当前只能按
SPEC §6.3.1 给出的「唯一恢复路径」手动提权一次：

```bash
# ① 建号：走注册端点，bcrypt 在 auth_api.cpp:170 里跑，库里存的是密文
curl -s -X POST http://127.0.0.1:8080/api/auth/register -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin123"}'

# ② 提权（这一步会被将来 T3.11 落地后的启动同步覆盖）
mysql -h127.0.0.1 -uoj -poj oj -e "UPDATE users SET role='admin' WHERE username='admin';"

# ③ 核对密文：必须是 bcrypt 的固定形状 $2b$10$ + 共 60 字符
mysql -h127.0.0.1 -uoj -poj oj -e \
  "SELECT username, role, LEFT(password_hash,7) pfx, LENGTH(password_hash) len FROM users WHERE username='admin'\G"

# ④ 三个 curl 一起验：登录 200、me 的 role=admin、admin/overview 200
curl -s -c /tmp/admin.jar -X POST http://127.0.0.1:8080/api/auth/login \
  -H 'Content-Type: application/json' -d '{"username":"admin","password":"admin123"}'
curl -s -b /tmp/admin.jar http://127.0.0.1:8080/api/auth/me
curl -s -b /tmp/admin.jar http://127.0.0.1:8080/api/admin/overview
```

**反向验证**（证明真的在校验密文，而不是明文比对）：

```bash
curl -s -X POST http://127.0.0.1:8080/api/auth/login -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin1234"}'      # 必须 400/1001
```

注意事项：

- `admin/admin123` 是测试口令，8 位满足 §5.8 的 6–64 字节；明文会进 shell 历史，别用在真实环境。
- **`.env` 里的 `OJ_ADMIN_USERNAMES` 先保持注释**：T3.11 没接线，写了也不生效，
  是 API.md §9.2 第 3 条记的那个「改了却不生效」的坑。
- `static/admin.html` 现在是**占位页**（说明只有只读 API + 给出 curl 命令），
  完整的管理页面是 T4.14。

---

## 8. 题库的增 / 改 / 写坏 / 删文件

**没有题库写接口**。唯一的增删改路径是「改 `problems/*.json` → **重启**」（SPEC D-30：
改题必须重启；题库同步只在启动时跑一次）。下面的预期值已由
`tests/run_problemsync_test.sh` 实测钉死（30 条断言）。

| 动作 | 预期（重启后读侧表现） |
|---|---|
| **增**：新增 `problems/<slug>.json` | 列表 `total` +1；详情 `200/0`，字段与 JSON 一致 |
| **改**：改标题/难度/标签/用例数/时限 | 列表与详情同步反映新值；**`problems.id` 不变**（走 UPDATE，不是删+插）；子表（用例/形参/标签）整体刷新 |
| **写坏**：删掉 `return_type` 这类必填字段 | 该题**行不删**，但 `caseCount=0`、`params=[]`；**仍出现在列表页**；启动回显出现「跳过 1 题，标记失效 1 题」 |
| **修回**：把 JSON 改回合法 | 用例数、形参、标签全部恢复，`id` 仍不变 —— 「标记失效」是可恢复的，不是终态 |
| **删文件**：删掉 JSON | **题目原样保留**（用例、形参、标题全在）。`deactivate_problem` 只在「JSON 校验不通过」时触发；文件被删时连 slug 都读不到，加载器什么都不做 |

两条要记住的语义差异：

1. **「改坏」与「删文件」的处置完全不同**：前者清空子表并把 `case_count` 置 0，后者什么都不做。
   原因见 `loader.h`【为什么不删"文件已删除"的题】：`submissions` 的外键是 `ON DELETE CASCADE`，
   删一道题的行会连带删掉**所有用户在该题上的提交历史**。
2. **「校验失败的题不会出现在列表页」这句话当前不成立**。`GET /api/problems` 直接查 `problems` 表且
   没有失效标记列，那道题会以 `caseCount=0` 的空题继续显示（判题缓存里确实没有了）。
   已在 `loader.h`、SPEC §5.6.1、API.md §9.2 三处标注实测修正。

手工复现（务必用不与真题库撞名的 slug，测完删文件 + 删行）：

```bash
cp problems/two-sum.json problems/sync-probe-2026.json   # 改 slug 后
pkill -x oj; (setsid nohup ./build/oj > /tmp/oj-run.log 2>&1 < /dev/null &); sleep 5
curl -s "http://127.0.0.1:8080/api/problems/sync-probe-2026" | head -c 300
rm -f problems/sync-probe-2026.json
mysql -h127.0.0.1 -uoj -poj oj -e "DELETE FROM problems WHERE slug='sync-probe-2026';"
```

---

## 9. 完整回归流程

改完代码后按这个顺序跑，全部通过才算没破坏东西：

```bash
cd /home/ZL/project/cpp-oj-vibcoding

# ① 静态与单测（约 45 秒，串行）
make build JOBS=1
ctest --test-dir build --no-tests=error -j1            # 期望：417 tests passed

# ② HTTP 契约（约 10 秒；需要服务已起）
pkill -x oj; (setsid nohup ./build/oj > /tmp/oj-run.log 2>&1 < /dev/null &); sleep 5
bash tests/run_tests.sh                                # 期望：通过 98，失败 0

# ③ 题库链路（自拉独立实例，不影响 ② 的服务）
bash tests/run_problemsync_test.sh                     # 期望：通过 30，失败 0

# ④ 收工
pkill -x oj
```

判断标准的统一口径：**每条断言都同时校验 HTTP 状态码与响应体里的 `code`**，两者不一致就是 bug
（`[D-29]` 要求同步，而它们出自两条不同的代码路径）。

---

## 10. 故障排查

| 症状 | 原因 | 处理 |
|---|---|---|
| 同一个 URL 时而 200 时而 404 | **同时跑着两个 `oj` 进程**（httplib 用 `SO_REUSEPORT`，两个进程都监听 8080，内核把连接随机分给它们，而旧进程没有新路由） | `pgrep -a -x oj` 确认只有一个；`pkill -x oj` 后重启。**本项目踩过一次**，症状极具迷惑性 |
| `pkill -f "build/oj"` 之后终端卡死 | 该模式会匹配到执行它的 shell 自身 | 改用 `pkill -x oj` |
| 端口被占用，`oj` 起不来 | 上一个实例还在 | `ss -ltnp \| grep 8080`，`pkill -x oj` |
| curl 报 `Couldn't connect` | 服务没起 / 还在启动（题库同步要先跑完） | `tail /5 /tmp/oj-run.log`，或 `sleep 5` 再试 |
| 登录 200 但后续请求全 `1002` | Cookie 没存下来 | 登录那条 curl 必须带 `-c /tmp/oj.jar`，后续请求带 `-b /tmp/oj.jar` |
| 接口返回 500，看不到原因 | 详情在日志里 | `grep ERROR logs/app.log \| tail -20` |
| 验收脚本大量 `code=null` | 响应不是 JSON（多半是静态页或 404 HTML） | 看脚本打印的「响应体」前 300 字节 |
| jq 把 `false` 显示成 `-` | jq 的 `//` 把 `false` 也当空值 | 用 `jq 'try (.x) \| if . == null then "-" else tostring end'`（脚本里已这么写） |
| 「方法不对」期望 405 却拿到 404 | **httplib 0.60 根本不会产生 405**（只定义了这个状态码枚举），`routes.cpp` 的 `case 405` 不可达 | 期望改成 `404/1004`；码对前端无影响 |
| `mysql: [Warning] Using a password on the command line...` | 无害 | `... 2>/dev/null` 滤掉 |
| 脚本里 `mysql` 命令失败但没报错 | `-e` 里写了多条语句时只返回最后一条的结果 | 用 `;` 分隔并各自检查 |

日志位置（相对仓库根）：

```
logs/app.log        每个请求一行 "METHOD /path → status" + 全部 WARN/ERROR
logs/judge.log      判题日志（judge 未接线时为空）
/tmp/oj-run.log     启动回显（配置、建表、题库同步、监听地址）—— 启动失败看这里
/tmp/oj-probe/      题库验收脚本的独立实例：probe.env / server.log / logs / judge
```

---

## 11. 当前未覆盖的部分

写在这里是为了**别把"没测"误当成"测过了"**：

| 项 | 现状 | 影响 |
|---|---|---|
| judge 调度器（T2.7） | `src/judge/{queue,scheduler,worker,runner,generator}.*` 仍是空文件 | `POST /api/submissions` 与 `POST /api/submissions/{id}/retry` 校验通过后回 `503/1009` 且**不落库**；201 与轮询链路无法验收。接线时只需把 `submission_api::judge_available()` 改成读调度器存活状态 |
| 限流（T3.4） | `include/web/ratelimit.h` 是空文件 | `1006`（每分钟提交数）与 `1007`（同时评测数）两个码不可达 |
| 启动角色同步（T3.11） | `config.cpp` 已解析 `OJ_ADMIN_USERNAMES`，但没人消费它 | 改 `.env` 不生效，管理员只能按 §7 手动提权 |
| 管理页面（T4.14） | `static/admin.html` 是占位页；`static/js/admin.js`、`static/css/admin.css` 仍是 0 字节 | 只能用 curl 验管理端点 |
| 其余前端脚本 | `list.js` / `problem.js` / `history.js` / `overview.js` / `poll.js` / `diff.js` / `editor.js` / `markdown.js` 都是 0 字节（只有 `api.js` / `auth.js` / `common.js` 有内容） | 页面级功能无法端到端验 |

提交读侧的四个端点（列表 / 详情 / diff / 删除）与重跑的前置条件，验收脚本是用 **SQL 造出
judge 写回时会产生的那种行** 再从 HTTP 侧验的 —— 也就是说，**归属校验（`1003`）、分页、
`diff`、`canRetry`、`1011` 都被真实验证过**，唯独「代码真的被编译运行」这一步没有。

> **阶段 0 之后**：`oj-reset` 不再伪造提交行，所以**初始库的提交三表恒为 0 行**
> （`oj-reset` 造完会自检，非 0 直接失败）。上面这套 SQL 造行因此从"补数据"变成了
> "唯一的接线前验数手段"；判题引擎上线后则应改用真提交，本节的 SQL 造行只留给
> 边界用例（JE / failed）。

---

## 附录 A：相关文件

| 关注点 | 文件 |
|---|---|
| 接口契约（本文的期望值来源） | `API.md` |
| 规格（错误码语义、§6.3.1 角色、§5.6.1 题库处置） | `SPEC.md` |
| HTTP 契约验收 | `tests/run_tests.sh` |
| 题库链路验收 | `tests/run_problemsync_test.sh` |
| 单元测试入口 | `tests/run_unit_tests.sh` |
| 路由接线（本次新增的端点都在这里注册） | `src/web/routes.cpp` |
| 账号/收藏/统计/概览业务层 | `src/web/user_api.cpp` |
| 提交组业务层（含 `judge_available()`） | `src/web/submission_api.cpp` |
