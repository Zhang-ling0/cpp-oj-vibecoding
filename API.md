# CppOJ 接口交互文档（API.md）

| 项 | 值 |
|---|---|
| 文档版本 | v1.1（V2.7 全部端点接线 + curl 端到端验收通过后回写） |
| 生成日期 | 2026-10-14（v1.1 同日） |
| 规格来源 | `SPEC.md` v1.9 §6.3（API 契约）/ §6.4 / §6.5 / §6.6 / §10.1 |
| 代码来源 | `src/web/routes.cpp`、`src/web/auth_api.cpp`、`src/web/problem_api.cpp`、`src/web/submission_api.cpp`、`src/web/user_api.cpp`、`src/web/admin.cpp`、`src/web/session.cpp`、`src/common/response.cpp`、`src/common/errors.cpp` |
| 服务地址 | `http://<OJ_LISTEN>`，默认 `http://0.0.0.0:8080`（浏览器实际访问 `http://127.0.0.1:8080`） |
| 传输 | 明文 HTTP（首版不开 TLS，`CPPHTTPLIB_OPENSSL_SUPPORT` 关闭） |

> **本文档是「契约 + 实现现状」两份信息的合订本。**
> 契约部分来自 SPEC §6.3，是前端与后端共同遵守的约定；实现现状来自当前代码 —— 两者**不完全重合**，
> 因此每个端点都带一个状态标记，缺的那部分不是笔误而是进度：

| 标记 | 含义 |
|---|---|
| ✅ **已接线** | HTTP 路由已注册（`src/web/routes.cpp` 的 `register_routes`），可直接 `curl` 调用 |
| 🟡 **业务层已实现，未接线** | 业务逻辑 + SQL + 响应体序列化 + 单测都已落地（`web/problem_api.*`、`web/admin.*`），但 HTTP 路由尚未注册。**当前调用会走全局收口返回 `404 / code=1004`** |
| ⬜ **未开工** | 仅有 SPEC 契约，代码中尚无对应实现 |
| 🟠 **已接线，判题侧未接线** | 端点本身完整可用，但**依赖 judge 调度器（T2.7）**的那一步会回 `1009 / 503`。今天用 `curl` 只能验到它的失败分支（1001 / 1002 / 1004 / 1009 / 1011），201 与轮询要等判题器接线 |

---

## 目录

1. [通用约定](#1-通用约定)
2. [端点总览](#2-端点总览)
3. [认证与会话](#3-认证与会话)
4. [题目](#4-题目)
5. [提交](#5-提交)
6. [统计与个人概览](#6-统计与个人概览)
7. [管理](#7-管理)
8. [静态资源与全局收口](#8-静态资源与全局收口)
9. [实现状态矩阵与文档差异](#9-实现状态矩阵与文档差异)
10. [附录](#10-附录)

---

## 1. 通用约定

### 1.1 统一响应包络 `[D-29]`

**所有** `/api/*` 端点（无论成功还是失败）都返回同一个包络，无例外：

```json
{ "code": 0, "message": "ok", "data": { } }
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `code` | number | 业务码。`0` = 成功；`1xxx` = API 异常；`2xxx` = 判题业务结果（不是错误，见 §10.2） |
| `message` | string | 人可读文案。成功固定为 `"ok"`；失败取自错误码表（`src/common/errors.cpp` 的 `message()`） |
| `data` | object | 业务数据。**永远是对象**，不会为 `null`；无内容时为 `{}` |

三条硬约定（代码里由 `src/common/response.cpp` 独家保证）：

- **业务码与 HTTP 状态码必须同步**：HTTP 状态 == `errors.h::http_status(code)`。前端 `static/js/api.js` 检测到两者不同步会在控制台打 WARN。
- **`data` 永远是对象**：失败包络也带 `data`，前端可以无条件读 `data.field` / `data.reason`。
- **失败时的 `data` 有四种形态**（`fail(error, field, reason)`）：

  | 调用形态 | 响应 `data` |
  |---|---|
  | 只给错误码 | `{}` |
  | 给 `field`，`reason` 为空 | `{"field":"password"}` |
  | `field` 为空，只给 `reason` | `{"reason":"注册已关闭（OJ_ALLOW_REGISTER=0）"}` |
  | 两者都给 | `{"field":"password","reason":"密码长度需为 6–64 个字符"}` |

> **字节序提示**：包络由 `nlohmann::json`（内部为 `std::map`）序列化，三个键的实际输出顺序是 **字典序 `code` → `data` → `message`**，与 SPEC 示例里手写的 `code, message, data` 不同。JSON 对象无序，不影响任何客户端；`tests/response_test.cpp` 对此有逐字节断言，改动序列化方式会被测试挡下。

响应头：

```
Content-Type: application/json; charset=utf-8
```

### 1.2 业务码 ↔ HTTP 状态码对照表（唯一真源 `include/common/errors.h`）

| code | HTTP | `message` | 含义 | 典型场景 |
|---|---|---|---|---|
| `0` | 200 / **201** | `ok` | 成功 | 201 仅用于 `POST /api/auth/register` |
| `1001` | 400 | 参数校验失败 | 参数校验失败 | 用户名为空、密码过短、枚举值非法、请求体不是 JSON |
| `1002` | 401 | 未登录或会话已过期 | 未登录 | 未带 Cookie、会话过期、token 形状非法 |
| `1003` | 403 | 无权限 | 无权限 | 访问他人提交；非 admin 访问 `/api/admin/*` |
| `1004` | 404 | 资源不存在 | 资源不存在 | 题目/提交不存在；**路由不存在；静态文件不存在**；方法不被接受 |
| `1005` | 409 | 用户名已注册 | 冲突 | 用户名已注册（含并发撞唯一键 `1062`） |
| `1006` | 429 | 提交过于频繁，请稍后再试 | 请求过于频繁 | 单用户每分钟提交数超限 |
| `1007` | 429 | 提交过于频繁，请等待当前评测完成 | 配额超限 | 单用户同时评测数超限 |
| `1008` | 500 | 服务器内部错误 | 服务器内部错误 | DB 异常、未捕获异常、连接池取不到租约 |
| `1009` | 503 | 判题服务不可用，请稍后再试 | 判题服务不可用 | judge 调度器未启动 / 队列满 |
| `1010` | 403 | 注册已关闭 | 注册已关闭 | `OJ_ALLOW_REGISTER=0` 时的注册请求 |
| `1011` | 409 | 当前状态不允许该操作 | 当前状态不允许 | 重跑一条非 `failed`/`JE` 的提交；未登录点提交/收藏 |
| `2001`–`2005` | **200** | 编译错误 / 运行超时 / 内存超限 / 答案错误 / 运行错误 | 判题结论 | **不是 API 异常**，通过 `data.verdict` 表达（见 §10.2） |

### 1.3 鉴权与会话 `[D-23] [D-69]`

| 项 | 约定 |
|---|---|
| 机制 | 用户名密码登录 → 服务端签发 64 位小写 hex token → `Set-Cookie` 下发 |
| Cookie 名 | `sid` |
| 下发属性 | `sid=<token>; HttpOnly; SameSite=Lax; Path=/`（顺序固定，逐字对应 `session::make_set_cookie`） |
| 清除属性 | `sid=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT` |
| `Secure` | **首版不加**（明文 HTTP，加了 Cookie 永不下发）。启用 HTTPS 时必须同步补 `Secure` 并让全站走 HTTPS |
| 有效期 | 7 天（`.env` 的 `OJ_SESSION_TTL_DAYS`，config 层夹到 `[1,3650]`）。到期即「不存在」，`find_user` 的 SQL 自带 `expires_at > UTC_TIMESTAMP()` |
| 过期回收 | 惰性回收（§6.3.2）：**服务启动时一次** + **每次登录成功时与签发同事务一次**（`DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP()`）。无定时器、无后台线程 |
| CSRF 防线 | `SameSite=Lax` 是唯一且实际的防线：跨站的 `POST`/`DELETE` 不携带 Cookie，请求到不了鉴权层。**去掉 `SameSite` 之前必须先补显式防护** |

**鉴权层级**（`Auth` 列的取值）：

| 值 | 含义 |
|---|---|
| 否 | 匿名可访问（未登录也能读到全部数据） |
| 是 | 必须带有效 `sid`，否则 `1002` |
| 是（仅本人） | 还要求资源归属当前用户，否则 `1003`（**admin 也不放行**） |
| 是 + `role='admin'` | 先判登录（`1002`）再判角色（`1003`），顺序不可颠倒 |

> **匿名可读的范围** `[D-43]`：题面、列表、标签全部公开可读；提交、收藏、按进度/收藏筛选等个人数据相关的能力一律要求登录。

### 1.4 时间、命名与空值约定

| 项 | 约定 |
|---|---|
| 时间字段 | 一律 **UTC**（`[D-17]`）。服务端不下发本地化时间，前端负责转本地时区 |
| 时间格式 | MySQL `DATETIME` 透传的文本，如 `2026-10-14 20:15:03` |
| 字段命名 | JSON `data` 内为 **camelCase**；**唯一例外**是题目详情里的 `hint_md`（沿用 SPEC §6.3 的 snake_case 拼写，见 §4.3 备注） |
| SQL 列名 | snake_case（`problem_slug`、`case_passed` …），不出现在 API 里 |
| 空值语义 | **库里的 `NULL` 序列化为 JSON `null`，而不是 `""`**。两者前端含义不同：`null` = 「库里没有这个东西」/「还没有结论」，`""` = 「有但是空的」 |
| 路径参数 | slug 形如 `two-sum`，原样出现在路径中，做 URL 解码后精确匹配（不做大小写折叠） |

### 1.5 分页、排序、筛选通用规则

| 参数 | 适用端点 | 规则 |
|---|---|---|
| `page` | 所有列表端点 | **1-based 纯数字**。`page=0` / `page=-1` / `page=1abc` / `page=1.5` 一律 `1001`（不做静默夹取） |
| `size` | 所有列表端点 | ≥ 1 的纯数字。超过上限时**夹到上限而不是报错**，且响应的 `size` 回显**实际生效值** |
| 未知参数名 | 所有端点 | **忽略**（前端新增筛选不应让旧后端整体 400） |
| 同名参数重复 | 所有端点 | **最后一个生效** |
| 枚举值大小写 | 所有枚举参数 | 大小写不敏感，前后空白会被 trim（如 `difficulty=EASY` 合法） |
| 枚举值非法 | 所有枚举参数 | `1001` + `data.field` = 参数名。**不回退默认值**（静默回退会给出「看起来筛了、其实没筛」的结果） |

### 1.6 限流与配额 `[D-22]`

| 规则 | 阈值（`.env`） | 超限响应 |
|---|---|---|
| 单用户每分钟提交数 | 10（`OJ_RATE_MAX_SUBMITS` / `OJ_RATE_WINDOW_SECONDS=60`） | `1006` / 429 |
| 单用户同时评测中（pending+judging） | 2（`OJ_JUDGE_USER_GATE`） | `1007` / 429 |
| 全局同时评测中 | 4（`OJ_JUDGE_GATE`，= 2 × worker） | **入队不拒绝**，返回 `queueAhead` 告知排队位置 |
| 单次提交代码长度 | 64 KB（`OJ_MAX_CODE_BYTES`） | 前端拦截 + 后端二次校验（`1001`） |
| 用户名 / 密码长度 | 3–32 / 6–64 字节 | `1001` + `data.field` |

> 「同时评测数」配额是 **DB 派生量**（`COUNT(*) FROM submissions WHERE user_id=? AND status IN ('pending','judging')`），随账号注销自动归零；每分钟滑窗是**纯内存**的，注销时需显式清理 `[D-70]`。

### 1.7 全局错误收口（`src/web/routes.cpp`）

任何**没有命中业务处理器**的情况都在这里被改写成统一包络：

| 触发 | HTTP | code | `data` | 说明 |
|---|---|---|---|---|
| 路由不存在 | 404 | `1004` | `{}` | 「你要用的 API 没了」 |
| 路径存在但方法不对 | **404** | `1004` | `{}` | ⚠️ 实际是 404 而不是 405：httplib 0.60 只定义了 405 这个状态码枚举，**没有任何代码路径会回它**，`routes.cpp` 里 `case 405` 目前不可达。与 404 同码，对前端而言都是「不存在」 |
| 请求体超限 | 413 | `1001` | `{"reason":"请求体超出允许的大小"}` | ⚠️ 当前未设置 `payload_max_length`，httplib 默认 100 MB，故该分支实际不可达；64 KB 代码上限应由业务层（`OJ_MAX_CODE_BYTES`）拦截 |
| 其他 httplib 协议级拒绝 | 4xx/5xx | `1001` | `{}` | 坏请求头、URI 超限等。报 `1001` 而非 `1008`，避免前端弹「服务器内部错误」并开始重试；同时在 `app.log` 留一条 WARN |
| 处理器抛异常 | 500 | `1008` | `{}` | 单请求失败，进程继续服务；异常详情进 `app.log` ERROR |

**一条不能破的规则**：**只要响应体是本服务自己写过的，就绝不改写**（判据是 body 非空，而不是 status）。httplib 的 error handler 会对**任何 status ≥ 400** 的响应回调，包括业务错误本身 —— 按 status 收口会把未登录的 `auth/me` 从 `1002` 改写成 `1004`。这条规则由 `tests/routes_test.cpp` 钉死。

访问日志：每个请求一行 `METHOD /path → status`，**只记方法/路径/状态，不记请求体与 Cookie**（后两者是凭据）。

---

## 2. 端点总览

| # | Method | Path | Auth | 状态 | 作用 |
|---|---|---|---|---|---|
| 1 | POST | `/api/auth/register` | 否 | ✅ | 注册，签发 `{userId,username,role}`，**不登录** |
| 2 | POST | `/api/auth/login` | 否 | ✅ | 登录，签发会话 Cookie |
| 3 | POST | `/api/auth/logout` | 否 | ✅ | 注销当前浏览器会话（幂等） |
| 4 | GET | `/api/auth/me` | 是 | ✅ | 查询当前登录身份与角色 |
| 5 | POST | `/api/auth/password` | 是 | ✅ | 改密码，并踢掉其他设备的会话 |
| 6 | DELETE | `/api/auth/account` | 是 | ✅ | 注销账号（硬删除，级联清数据） |
| 7 | GET | `/api/tags` | 否 | ✅ | 取全部标签，按 `algorithm`/`source` 分组 |
| 8 | GET | `/api/problems` | 否 | ✅ | 题目列表：关键词/难度/标签/进度/收藏筛选 + 排序 + 分页 |
| 9 | GET | `/api/problems/{slug}` | 否 | ✅ | 题目详情：题面、提示、签名、模板、最近 5 次提交、下一题建议 |
| 10 | POST | `/api/problems/{slug}/favorite` | 是 | ✅ | 切换收藏 |
| 11 | POST | `/api/submissions` | 是 | 🟠 | 提交代码，返回 `submissionId` + 队列位置（judge 未接线 → `1009`） |
| 12 | GET | `/api/submissions/{id}` | 是（仅本人） | ✅ | 提交详情：逐用例结果 + 编译诊断 |
| 13 | GET | `/api/submissions` | 是（仅本人） | ✅ | 提交历史列表（筛选 + 分页） |
| 14 | GET | `/api/submissions/diff` | 是（两条均本人） | ✅ | 取两条提交的代码原文，供前端算并排 diff |
| 15 | DELETE | `/api/submissions/{id}` | 是（仅本人） | ✅ | 删除自己的提交 |
| 16 | POST | `/api/submissions/{id}/retry` | 是（仅本人） | 🟠 | 重跑失败的提交（**新增一条**记录；judge 未接线 → `1009`） |
| 17 | GET | `/api/stats` | 是 | ✅ | 4 个标量计数，顶栏等处取单值 |
| 18 | GET | `/api/me/overview` | 是 | ✅ | 概览条与「我的」tab2 的唯一数据源 |
| 19 | GET | `/api/admin/overview` | 是 + admin | ✅ | 只读管理面板：系统计数 + 用户清单 |
| 20 | GET | `/*` | 否 | ✅ | 静态资源（前端五页 + vendored 库） |

---

## 3. 认证与会话

> 对应 SPEC §6.3「认证」组。四个已接线端点的业务层在 `src/web/auth_api.cpp`，会话层在 `src/web/session.cpp`。

### 3.1 `POST /api/auth/register` ✅

创建账号。**不涉及提权**：新用户的 `role` 恒为 `"user"`，由 INSERT 语句写死；管理员身份只由 `.env` 的 `OJ_ADMIN_USERNAMES` 在启动同步时决定。

**请求**

| 位置 | 字段 | 类型 | 必填 | 约束 |
|---|---|---|---|---|
| Header | `Content-Type: application/json` | — | 是 | 请求体必须是 JSON **对象** |
| Body | `username` | string | 是 | 3–32 个字符；字符集限 `[A-Za-z0-9_-]` |
| Body | `password` | string | 是 | 6–64 **字节**；不得全为空白；不校验复杂度 |

> **用户名字符集为何收窄**（`include/web/auth_api.h`）：`users.username` 是 `utf8mb4_general_ci`（**大小写不敏感**）且 PAD SPACE 比较会忽略尾部空格。不限字符集就会出现「Bob 注册成功、bob 登录查到 Bob」的大小写别名，以及「明明注册了却说已存在」；混入不可见字符还会让 `.env` 的管理员名单按精确字符串匹配时静默不生效。

**四道闸门（顺序不可换）**

| 序 | 闸门 | 失败 |
|---|---|---|
| ① | `OJ_ALLOW_REGISTER` 是否为 1 | `1010`，**且不落库**（连库都不碰） |
| ② | 用户名校验 | `1001` + `data.field="username"` |
| ③ | 密码校验 | `1001` + `data.field="password"` |
| ④ | 用户名唯一（`SELECT` 判重 + `uk_username`） | `1005` |

> bcrypt（cost=10，本机约 60–100 ms）**只在四道都过之后**执行：放在校验之前等于让一个纯格式错误的请求也吃掉 100 ms。
> 并发注册时 `SELECT` 判重不构成保证，真正的保证是唯一索引；因此 MySQL 错误码 `1062` 也被映射成同一个 `1005`，而不是 `1008`。

**成功响应** `201 Created`

```json
{ "code": 0, "message": "ok", "data": { "userId": 7, "username": "alise", "role": "user" } }
```

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `userId` | number | `users.id`（自增主键） |
| `username` | string | 原样回显 |
| `role` | string | 恒为 `"user"` |

> 注册**不返回 Cookie、不建立会话** —— 成功后需再调 `POST /api/auth/login`。

**失败响应**

| code | HTTP | `data` | 触发 |
|---|---|---|---|
| `1001` | 400 | `{"field":"username","reason":"用户名长度需为 3–32 个字符"}` | 长度/字符集不合法 |
| `1001` | 400 | `{"field":"password","reason":"密码长度需为 6–64 个字符"}` | 长度不合法 |
| `1001` | 400 | `{"reason":"请求体不是合法 JSON：…"}` | body 非 JSON / 非对象 / 为空（**无 `field` 键**，见 §1.1 四种形态） |
| `1005` | 409 | `{}` | 用户名已注册 |
| `1010` | 403 | `{"reason":"本实例已关闭注册（OJ_ALLOW_REGISTER=0）"}` | 注册开关关闭 |
| `1008` | 500 | `{}` | DB 异常 / 连接池未就绪 / 熵源故障 |

**示例**

```bash
curl -i -X POST http://127.0.0.1:8080/api/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"username":"alise","password":"pass1234"}'
```

---

### 3.2 `POST /api/auth/login` ✅

校验口令 → 签发会话 → 下发 Cookie。**顺带触发过期会话回收**（与签发同事务）。

**请求**：`{ "username": string, "password": string }`，两个字段都必填。

> **登录不做参数闸门**：不查长度、不查字符集。登录失败一律 `1001`，若这里先按注册那套报「用户名长度不足」，就成了枚举接口的另一半。

**成功响应** `200 OK` + `Set-Cookie`

```
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Set-Cookie: sid=3f2a…（64 位小写 hex）; HttpOnly; SameSite=Lax; Path=/
```

```json
{ "code": 0, "message": "ok", "data": { "userId": 7, "username": "alise", "role": "admin" } }
```

`data` 与注册端点同构（`userId` / `username` / `role`）。

**失败响应**

| code | HTTP | `data` | 触发 |
|---|---|---|---|
| `1001` | 400 | `{"field":"password","reason":"用户名或密码错误"}` | 用户不存在 **或** 口令不匹配（**同码同文案**） |
| `1001` | 400 | `{"reason":"请求体不是合法 JSON：…"}` | body 非法 |
| `1008` | 500 | `{}` | DB 异常 |

> **防枚举**：用户不存在时也会对着一个固定的假哈希跑一次 bcrypt（cost=4，约 2 ms），把「查库即返回」与「跑满 100 ms」的时间差抹平；同时两种失败共用同一句文案。前端拿不到「这个用户名注册过」的信息。
> 登录成功时会在同一事务里先 `DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP()` 再 INSERT，避免并发登录互删会话。

---

### 3.3 `POST /api/auth/logout` ✅

删除本浏览器的会话行并清除 Cookie。**幂等**：没有 Cookie、Cookie 形状非法、会话早已过期，一律 `200`。

**请求**：无 Body。需要带 `Cookie: sid=…` 才会真正访问 DB；不带 `sid` 的请求**不取 DB 租约**，不占用串行化的全局互斥量。

**成功响应** `200 OK`

```
Set-Cookie: sid=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT
```

```json
{ "code": 0, "message": "ok", "data": { "message": "已登出" } }
```

> 清除 Cookie 那一头**无条件下发**（即使本来就没有 `sid`）：浏览器里可能存着一个形状非法的 `sid`，让浏览器自己删掉比留着强。
> 清除头的名字/路径/属性必须与下发头**逐字一致**，否则浏览器会当成另一个 Cookie 留着，表现为「服务端已登出、刷新又变回登录态」。

**失败响应**：`1008` / 500（仅 DB 异常一种）。

---

### 3.4 `GET /api/auth/me` ✅

查询当前登录身份。**顶栏每次加载都要问一次**，因此未登录不是错误弹窗而是可渲染的正常形态。

**请求**：无参数，读 `Cookie: sid=…`。

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok", "data": { "userId": 7, "username": "alise", "role": "admin" } }
```

**失败响应**

| code | HTTP | 触发 |
|---|---|---|
| `1002` | 401 | 三种情况同码：无 `Cookie` 头 / 会话已过期 / `sid` 形状非法（恰好 64 个 `[0-9a-f]`） |
| `1008` | 500 | DB 异常 |

> 前端 `static/js/api.js` 的 `currentUser()` 在**页面生命周期内只问一次**并缓存结果；登出时把缓存置空。

---

### 3.5 `POST /api/auth/password` ✅

改密码，并在**同一事务**里踢掉除当前会话外的全部会话（保留当前会话，改密后不丢失登录态 `[D-40]`）。

**请求**

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `oldPassword` | string | 是 | 必须与库中 bcrypt 哈希匹配，否则 `1001` + `field=oldPassword` |
| `newPassword` | string | 是 | 6–64 字节且非全空白，否则 `1001` + `field=newPassword` |

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok", "data": { "message": "密码已修改，其他设备的登录已失效" } }
```

**失败响应**：`1001`（`field` 区分 `oldPassword` / `newPassword`）、`1002`、`1008`。

**实现约束（SPEC §6.3.5，接线时必须遵守）**

1. 必须验旧密码，且 bcrypt 只在验旧通过后执行。
2. `UPDATE users SET password_hash=?` 与 `DELETE FROM sessions WHERE user_id=? AND token <> ?` 必须**同一事务**；`<>` 是要害，删掉自己那行等于「改完立刻被登出」。
3. 不接受也不返回用户名（改用户名会破坏 `OJ_ADMIN_USERNAMES` 的按名匹配）。
4. 成功后前端应静默重拉 `GET /api/auth/me`（`role` 真源是 `.env`，可能已被运维改过）。
5. 不单独加限流器：威胁模型是「已登录用户改自己的密码」，登录态 + 旧密码校验已足够。

---

### 3.6 `DELETE /api/auth/account` ✅

**硬删除**账号（`[D-57]`），不可恢复。二次确认弹窗必须逐项列出将被删除的数据量（提交 N 条 / 收藏 M 题 / 会话 K 个）—— 这是硬删除方案能成立的唯一前提。

**请求**

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `password` | string | 是 | 必须匹配，否则 `1001` + `field=password` |

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok", "data": { "message": "账号已注销" } }
```

**失败响应**：`1001`、`1002`、`1008`。

**实现约束（SPEC §6.3.7）**

1. 一条 `DELETE FROM users WHERE id=?` 完成整条 CASCADE 链：`users` → `sessions` / `submissions` / `user_problems`，`submissions` → `submission_case_results` / `submission_compile_errors`。
2. 不动 `problems` / `problem_cases` / `tags` —— 题库是全局资源。
3. 成功后清当前 Cookie，前端清 localStorage 里的 `draft:*` / `draftAt:*`，跳 `/login.html?deleted=1`。
4. 内存里的 60 秒滑窗计数器需显式清理（`ratelimit::forget(user_id)`）—— 「同时评测数」是 DB 派生量，随 CASCADE 自动归零。

---

## 4. 题目

> 对应 SPEC §6.3「题目」组 + §6.4 进度推导 + §6.6 推荐规则。读侧业务层：`src/web/problem_api.cpp`；
> 收藏的业务层：`src/web/user_api.cpp`。**本组四个端点均已接线。**

### 4.1 `GET /api/tags` ✅

取全部标签，按类别分组，供列表页筛选栏使用。

**请求**：无参数。

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "algorithm": ["哈希表", "双指针", "动态规划", "数组", "链表"],
    "source": ["高频面试题", "微软", "Google"]
  }
}
```

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `algorithm` | string[] | 算法主题标签，按名称升序 |
| `source` | string[] | 来源/面试频率标签，按名称升序 |

**失败响应**：`1008`（DB 异常或连接池未就绪）。

> 组内按 name 升序、组顺序固定（`algorithm` 在前、`source` 在后），前端不需要再排。库中出现未知 `category` 的标签会被跳过并在 `app.log` 打 WARN（不因一个脏数据让整个筛选栏空掉）。

---

### 4.2 `GET /api/problems` ✅

题目列表：多条件筛选 + 排序 + 分页，一次请求同时带回进度与收藏状态。

**Query 参数**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `keyword` | string | 空 | 子串匹配 `title` **或** `slug`；`%` `_` `\` 会被转义；trim 后为空 = 不筛；**上限 200 字符**（`utf8mb4_general_ci`，实际大小写不敏感） |
| `difficulty` | enum | 空 | `easy` / `medium` / `hard`；空值 = 不筛 |
| `tags` | string | 空 | 逗号分隔，**AND 语义（必须同时命中全部所选标签）**；单个名字 ≤ 64 字符，去重，**上限 20 个** |
| `progress` | enum | `none` | `none` / `attempted` / `solved`；过滤的是 §6.4 推导出的三态枚举值本身，故 `attempted` 与 `solved` **互斥** |
| `favorite` | bool | `0` | `1`/`true`/`yes`/`on` = 仅看收藏；`0`/`false`/`no`/`off`/空 = 不筛 |
| `sort` | enum | `default` | `default`（`sort_no ASC, id ASC`）/ `featured`（`is_featured DESC, sort_no ASC, id ASC`）/ `easiest`（`difficulty ASC, …`）/ `hardest`（`difficulty DESC, …`） |
| `page` | int | `1` | 1-based，纯数字 |
| `size` | int | `20` | ≥ 1；**超过 100 夹到 100**，响应的 `size` 回显实际值 |

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "list": [
      {
        "id": 12, "slug": "two-sum", "title": "两数之和", "difficulty": "medium",
        "tags": [ { "name": "哈希表", "category": "algorithm" },
                  { "name": "数组",   "category": "algorithm" },
                  { "name": "高频面试题", "category": "source" } ],
        "myProgress": "attempted", "isFavorite": true,
        "timeLimitMs": 3000, "memLimitKb": 131072, "caseCount": 12,
        "isFeatured": true, "sortNo": 10
      }
    ],
    "total": 47, "page": 1, "size": 20
  }
}
```

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `list[]` | array | 当前页题目 |
| `list[].id` / `.slug` / `.title` / `.difficulty` | number/string | 基础字段 |
| `list[].tags[]` | `{name, category}[]` | 按 `(category, name)` 升序（**顺序确定**，两次查询结果一致） |
| `list[].myProgress` | enum | `none` / `attempted` / `solved`，见 §10.3 |
| `list[].isFavorite` | bool | 匿名恒为 `false` |
| `list[].timeLimitMs` | number | CPU 时间限制 |
| `list[].memLimitKb` | number | **地址空间（`RLIMIT_AS`）口径**，比 LeetCode 的 RSS 口径更严，见 §10.5 |
| `list[].caseCount` | number | 题库用例数 |
| `list[].isFeatured` / `.sortNo` | bool/number | 精选置顶与题库编排序号 |
| `total` | number | 过滤后的总条数（不含分页），供前端算页数 |
| `page` / `size` | number | 回显**实际生效**的页码与页大小 |

**失败响应**

| code | HTTP | `data.field` | 触发 |
|---|---|---|---|
| `1001` | 400 | `keyword` | 关键词 > 200 字符 |
| `1001` | 400 | `difficulty` | 不是 easy/medium/hard |
| `1001` | 400 | `tags` | 单个标签名 > 64 字符，或标签数 > 20 |
| `1001` | 400 | `progress` | 不是 none/attempted/solved；**或匿名请求带了 progress/favorite** |
| `1001` | 400 | `favorite` | 不是 1/0/true/false；**或匿名请求带了 favorite** |
| `1001` | 400 | `sort` | 不是 default/featured/easiest/hardest |
| `1001` | 400 | `page` / `size` | 非纯数字或 < 1 |
| `1001` | 400 | （空） | 查询串里有不合法的 URL 转义（如 `%ZZ`、截断的 `%4`） |
| `1008` | 500 | `{}` | DB 异常 |

> **匿名 + 个人化筛选为什么是 `1001` 而不是静默忽略**：忽略掉会返回一个「看起来合法但不是用户要的东西」的列表，空列表更糟（用户会读成「题库里没有这类题」）。明确报错至少能让前端弹一句「登录后可按进度筛选」。
> **`tags` 为什么用 EXISTS 而不是 JOIN + DISTINCT**：JOIN 会把题乘以标签数，`LIMIT/OFFSET` 就在「乘完再数」的结果上取，第 2 页直接漏题；EXISTS 不改变行数。

---

### 4.3 `GET /api/problems/{slug}` ✅

题目详情。**匿名可读全部题面** `[D-43]`；登录时额外附带「你的最近 5 次提交」与「下一题建议」。

**Path 参数**

| 参数 | 说明 |
|---|---|
| `slug` | 题目标识，如 `two-sum`。精确匹配；URL 解码后查库 |

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "id": 12, "slug": "two-sum", "title": "两数之和", "difficulty": "medium",
    "statementMd": "## 题目描述\n\n给定一个整数数组 …",
    "hint_md": "先想想要不要重复遍历数组 …",
    "signature": "class Solution {\npublic:\n    std::vector<int> twoSum(...);\n};",
    "methodName": "twoSum", "returnType": "std::vector<int>",
    "harnessPrelude": null,
    "codeTemplate": "#include <vector>\n…",
    "params": [ { "name": "nums", "cppType": "std::vector<int>&" },
                { "name": "target", "cppType": "int" } ],
    "tags": [ { "name": "哈希表", "category": "algorithm" } ],
    "timeLimitMs": 3000, "memLimitKb": 131072, "caseCount": 12,
    "isFeatured": true, "sortNo": 10,
    "myProgress": "attempted", "isFavorite": true,
    "recentSubmissions": [
      { "id": 88, "status": "done", "verdict": "WA", "message": "第 3 个用例答案错误",
        "casePassed": 2, "caseTotal": 12, "totalMs": 41, "maxMemKb": 3072,
        "createdAt": "2026-10-14 19:02:11" }
    ],
    "nextRecommendation": {
      "id": 27, "slug": "reverse-linked-list", "title": "反转链表",
      "difficulty": "medium", "tag": "链表", "tagSolved": 2, "tagTotal": 5,
      "reason": "「链表」标签你通过 2/5，本题尚未通过"
    }
  }
}
```

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `statementMd` | string | 题面 Markdown 原文，前端渲染（**禁图片**，树/图题用字符画 `[D-48]`） |
| `hint_md` | string \| null | 方向提示。**`null` ⇒ 前端不渲染提示 tab**（不是渲染一个空 tab）`[D-47]`。⚠️ **这个键是 snake_case**，其余字段都是 camelCase —— 沿用 SPEC §6.3 的拼写，改名要同步改前端与验收用例 |
| `signature` / `methodName` / `returnType` | string | harness 调用的类/方法/返回类型；提交前核对用 `[D-28]` |
| `harnessPrelude` | string \| null | `__expr` 逃生舱所需的前置声明/头文件；无则 `null` |
| `codeTemplate` | string | 编辑器初始内容 |
| `params[]` | `{name, cppType}[]` | 形参，按 `param_idx` 升序（决定调用顺序） |
| `tags[]` | `{name, category}[]` | 同列表端点 |
| `myProgress` / `isFavorite` | enum / bool | 匿名时分别为 `none` / `false` |
| `recentSubmissions[]` | array | **登录才有内容**，最多 5 条，按 id 倒序；匿名为空数组 |
| `recentSubmissions[].*` | — | `id` `status` `verdict` `message` `casePassed` `caseTotal` `totalMs` `maxMemKb` `createdAt`；数值列在库里为 NULL 时下发 `null`（如还在排队） |
| `nextRecommendation` | object \| null \| **不下发** | 见下方说明 |

**`nextRecommendation` 的三种形态**（前端行为不同，不可混同）

| 形态 | 触发条件 | 前端表现 |
|---|---|---|
| **整个字段不下发** | 未登录 | 整张推荐卡不渲染（不是「登录后有推荐」的空承诺）`[D-43]` |
| 对象 | 已登录且有推荐 | 渲染推荐卡 + `reason` 文案 |
| `null` | 已登录但候选集为空（全部通过 / 无 algorithm 标签） | 改为展示完成度 |

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` / `slug` / `title` / `difficulty` | number/string | 目标题 |
| `tag` | string | **主标签** = 该题 algorithm 标签中用户通过率最低者，并列取 `tags.id` 最小 `[D-68]` |
| `tagSolved` / `tagTotal` | number | 该标签下你已通过 / 该标签下题目总数 |
| `reason` | string | **服务端算好的可解释文案，前端不得自己编**；与 `tag` 同源 `[D-59]` |

**失败响应**

| code | HTTP | 触发 |
|---|---|---|
| `1004` | 404 | slug 不存在、或该题被题库移除（两种情况在此层是同一件事，不区分） |
| `1008` | 500 | DB 异常 |

> **下一题建议失败不会让整个详情端点失败**：它是页面底部的一张卡，而题面是主体。宁可少一张卡，也不要用户读题时看到 1008；但失败会进 `app.log`。

---

### 4.4 `POST /api/problems/{slug}/favorite` ✅

切换收藏状态。

**请求**：无 Body。需登录（未登录 `1002`）。

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok", "data": { "isFavorite": true } }
```

**失败响应**：`1002`（未登录）、`1004`（slug 不存在）、`1008`。
> 语义上是**切换**（toggle）而非置位：调用一次即翻转，响应回切换**之后**的状态。

---

## 5. 提交

> 对应 SPEC §6.3「提交」组 + §5 判题流水线 + §6.3.6。**本组 6 个端点全部已接线**，业务层 `src/web/submission_api.cpp`。
> ⚠️ 其中 **2 个标 🟠**：`POST /api/submissions` 与 `POST /api/submissions/{id}/retry` 的最后一步是
> "入队判题"，而 judge 调度器（T2.7）尚未接线，故它们今天稳定回 `1009 / 503` 且【不落库】——
> 这样库里不会出现一批永远 `pending` 的死提交。四个读/写端点不依赖 judge，全部可用。

### 5.1 `POST /api/submissions` 🟠（judge 未接线）

提交代码判题。**立即返回**，前端用返回的 `submissionId` 启动轮询 `[D-10] [D-27]`。

**请求**

| 字段 | 类型 | 必填 | 约束 |
|---|---|---|---|
| `problemSlug` | string | 是 | 题目标识 |
| `language` | string | 否 | 默认 `"cpp"`（首版只支持 C++） |
| `code` | string | 是 | **≤ 64 KB**（`OJ_MAX_CODE_BYTES`），前端拦截 + 后端二次校验 |

**成功响应** `201 Created`

```json
{ "code": 0, "message": "ok",
  "data": { "submissionId": 91, "status": "pending", "queueAhead": 0 } }
```

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `submissionId` | number | 轮询用 |
| `status` | string | 固定 `"pending"`（刚入队） |
| `queueAhead` | number | **你前面还有几个 pending 任务**。`0` = 马上开跑；`n` ⇒ 前端显示「前面还有 n 个评测」`[D-54] [D-60]` |

> `queueAhead` 由 `SELECT COUNT(*) FROM submissions WHERE status='pending' AND id < ?` 得到，**不落库**。它**只回答自己的问题**，绝不返回全局队列长度（那会泄露其他用户的并发负载 `[D-60]`）。⚠️ 该等价关系依赖「队列 FIFO 且按 id 升序出队」，队列若改为优先级队列必须改成从调度器读真实位置（一条需要留注释的隐含契约）。

**失败响应**

| code | HTTP | 触发 |
|---|---|---|
| `1001` | 400 | `code` 超长 / `problemSlug` 缺失 |
| `1002` | 401 | 未登录 |
| `1004` | 404 | slug 不存在 |
| `1006` | 429 | 单用户每分钟提交 > 10 |
| `1007` | 429 | 单用户同时评测 > 2 |
| `1009` | 503 | judge 调度器未启动 / 队列满 ← **当前就是这个分支** |
| `1008` | 500 | DB 异常 |

> **当前状态（V2.7）**：judge 调度器（T2.7）尚未接线，本端点在校验通过后稳定回 `1009`，
> 且**一条行都不写**。判题器接线后只需把 `submission_api::judge_available()` 改成读调度器存活状态，
> 路由层与 SQL 一行都不用动。

**轮询节奏**：前端每 1.5 s 调一次 `GET /api/submissions/{id}`；`queueAhead` 为 `n>0` 时每秒递减 1（推算，不额外请求）。提交 id 存 sessionStorage，页面加载时自动恢复轮询 `[D-53]`。

---

### 5.2 `GET /api/submissions/{id}` ✅

提交详情。前端唯一的「复盘」数据源。

**归属校验**：取 session → 无 → `1002`；`SELECT … WHERE id=? AND user_id=?` → 无行 → `1003`。**归属写进 WHERE，不单独查再比**；admin 不放宽。

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "id": 91, "status": "done", "verdict": "WA",
    "message": "第 3 个用例答案错误",
    "casePassed": 2, "caseTotal": 12, "totalMs": 41, "maxMemKb": 3072,
    "code": "#include <vector>\n…",
    "compilerOutput": null,
    "compileErrors": [],
    "caseResults": [
      { "caseIdx": 0, "verdict": "AC", "passed": true,
        "argsText": "[2,7,11,15], target=9", "outputText": "[0,1]", "expectedText": "[0,1]",
        "timeMs": 3, "memKb": 2048, "errorMsg": null },
      { "caseIdx": 1, "verdict": "AC", "passed": true, "…": "…" },
      { "caseIdx": 2, "verdict": "TLE", "passed": false,
        "argsText": "…", "outputText": null, "expectedText": "…",
        "timeMs": null, "memKb": null, "errorMsg": "超出时间限制 (3000ms)" }
    ],
    "createdAt": "2026-10-14 20:01:02",
    "canRetry": false
  }
}
```

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `status` | enum | `pending` / `judging` / `done` / `failed` |
| `verdict` | enum \| null | 仅 `status='done'` 时非空：`AC` / `WA` / `TLE` / `MLE` / `RE` / `CE` / `JE` |
| `message` | string \| null | 顶层说明，如「第 3 个用例超时」「编译超时」「测试程序未能运行（平台错误），请重试或联系管理员」 |
| `casePassed` / `caseTotal` | number | 「通过 2/12」。`caseTotal = GREATEST(实跑明细行数, 题库用例数)`，**因此 CE 时是 12 而不是 0** `[D-65]` |
| `totalMs` / `maxMemKb` | number | 汇总列，与明细行同事务写入 |
| `code` | string | 代码全文，可回看/回填编辑器 |
| `compilerOutput` | string \| null | **原始编译器输出全文**，始终展示（未解析的 warning/note 都在里面） |
| `compileErrors[]` | array | 结构化诊断，见下 |
| `caseResults[]` | array | 逐用例结果，按 `case_idx` 升序 |
| `canRetry` | bool | `status='failed' && verdict='JE'` 时为 `true`，前端据此决定是否渲染「重跑」按钮 `[D-49]` |
| `createdAt` | string | UTC |

> **字段名来源**：外层字段名（`id` / `status` / `verdict` / `message` / `caseResults` / `compileErrors` / `compilerOutput` / `casePassed` / `caseTotal` / `totalMs` / `maxMemKb` / `code` / `createdAt` / `canRetry`）由 SPEC §6.3 端点表写死；下列两个数组的**元素字段名是按 DB 列名转 camelCase 的建议**，实现时需与前端一次性约定并回写本文档。

**`compileErrors[]` 元素**

| 字段 | 说明 |
|---|---|
| `file` | `user.cpp` / `harness` / `link` —— 见下方归属分流 |
| `line` / `col` | 1-based，`file='harness'` 或 `'link'` 时**不提供行高亮** `[D-64]` |
| `severity` | `error` / `warning` / `note` |
| `message` | 诊断文本 |

> **归属分流** `[D-64]`：`user.cpp` 是用户代码，正常行高亮；`tester.cpp` / `solution_decl.h` / `tester_util.h` 归一化为 `harness`，显示「这不是你的代码出错，通常说明该题配置有误」且**不给行号**；链接期 `undefined reference` 归一化为 `link`，给出「核对类名/方法名/参数类型/返回类型」的文案，同样不给行号。

**`caseResults[]` 元素**

| 字段 | 说明 |
|---|---|
| `caseIdx` | 0-based，快照自题库用例序号 |
| `verdict` | `AC` / `WA` / `TLE` / `MLE` / `RE` |
| `passed` | 是否通过 |
| `argsText` / `outputText` / `expectedText` | 输入 / 你的 stdout / 期望值（展示用序列化文本） |
| `timeMs` / `memKb` | **TLE/MLE 时为 `null`**（子进程被 SIGKILL，harness 来不及输出度量），不是 0 |
| `errorMsg` | 如「超出时间限制 (3000ms)」或 RE 的 stderr 摘要 |

**失败响应**：`1002`、`1003`（非本人）、`1004`（id 不存在）、`1008`。

---

### 5.3 `GET /api/submissions` ✅

提交历史列表（默认时间倒序）。

**Query 参数**

| 参数 | 说明 |
|---|---|
| `problemSlug` | 可选，按题目过滤 |
| `verdict` | 可选，按结论过滤（`AC`/`WA`/`TLE`/`MLE`/`RE`/`CE`/`JE`） |
| `page` | 1-based，默认 1 |
| `size` | 默认 **20** `[D-61]` |

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok",
  "data": { "list": [ { "id": 91, "problemSlug": "two-sum", "problemTitle": "两数之和",
                        "status": "done", "verdict": "WA", "casePassed": 2, "caseTotal": 12,
                        "totalMs": 41, "maxMemKb": 3072, "createdAt": "2026-10-14 20:01:02" } ],
            "total": 128, "page": 1, "size": 20 } }
```

**失败响应**：`1002`、`1001`（分页/枚举参数非法）、`1008`。仅返回本人数据。

> SPEC §6.3 只固定了 `{list:[…], total, …}` 这一层形状与四个 Query 参数；`list[]` 的元素字段名（`problemSlug` / `problemTitle` / …）为**建议值**，实现时需与前端一次性约定。

---

### 5.4 `GET /api/submissions/diff` ✅

取**两条**提交的代码原文，供前端在内存里算并排 diff `[D-42] [D-50]`。

**Query 参数**

| 参数 | 说明 |
|---|---|
| `from` | 较早那条的 id（必填，纯数字） |
| `to` | 较晚那条的 id（必填，纯数字） |

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "from": { "id": 12, "createdAt": "2026-10-13 21:10:00", "code": "class Solution {\n…" },
    "to":   { "id": 15, "createdAt": "2026-10-14 09:22:41", "code": "class Solution {\n…" }
  }
}
```

| `data` 字段 | 说明 |
|---|---|
| `from` / `to` | 各含 `id` / `createdAt` / `code`（原文，每份 ≤ 64 KB） |

**失败响应**

| code | HTTP | 触发 |
|---|---|---|
| `1001` | 400 | `from`/`to` 缺失、非数字，或 `from == to` |
| `1002` | 401 | 未登录 |
| `1003` | 403 | 任一条不属于当前用户（含 admin 查他人） |
| `1004` | 404 | id 不存在 |
| `1008` | 500 | DB 异常 |

> **为什么服务端不算 diff**：归属校验必须走 SQL，所以这次请求本来就要打一次 DB。既然已在服务端，把两段代码一起返回、由前端算 diff 才是对的分工；LCS 最坏 O(n·m) ≈ 4000×4000，64 KB 上限本身就是性能护栏，`diff.js` 在前端内存内完成（P95 < 100 ms）。
> **SPEC §6.3.6 示例里的 `hunks` 数组是前端计算结果，不是服务端字段** —— 服务端只返回 `from` / `to` 两段原文。
> 题目页底部「你的最近提交」勾选任意两条后调用本端点，展开后任一侧代码可一键回填编辑器。

---

### 5.5 `DELETE /api/submissions/{id}` ✅

删除自己的提交 `[D-49]`。

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok", "data": { "message": "提交记录已删除" } }
```

**失败响应**：`1002`、`1003`（非本人）、`1004`、`1008`。

> 实现只需 `DELETE FROM submissions WHERE id=? AND user_id=?`；逐用例明细与编译诊断由外键 CASCADE 连带删除，无需显式 DELETE。**admin 不放开**。

---

### 5.6 `POST /api/submissions/{id}/retry` 🟠（judge 未接线）

重跑一条**失败**的提交 `[D-58]`。

**前置条件**

| 条件 | 不满足 |
|---|---|
| 属于当前用户 | `1003` |
| `status='failed'` **且** `verdict='JE'` | `1011`「只有失败的提交可以重跑」 |

**成功响应** `201 Created`

```json
{ "code": 0, "message": "ok", "data": { "newSubmissionId": 95 } }
```

**失败响应**：`1002`、`1003`、`1004`、`1011`、`1008`，以及 judge 未接线时的 `1009`（§5.1 同款）。

> **当前状态（V2.7）**：judge 未接线时本端点仍会**完整校验**（归属 `1003` / 存在 `1004` /
> 状态 `1011` 照常报），只是**不新建那条记录**，最后回 `1009`。

> **重跑是新增一条记录，不覆盖原记录**：覆盖会破坏「提交历史不可变」，且要求先删明细再重写，凭空多一个失败点。`INSERT INTO submissions (…) SELECT … FROM submissions WHERE id=?` 与普通提交走**完全相同**的代码路径 —— 判题侧零改动。
> **代价**：重跑与手动重提在历史里无法区分（要区分需加 `retry_of` 自引用列，明确不做）。前端只在 `canRetry=true` 时渲染「重跑」按钮，避免用户点了才收 1011。

---

## 6. 统计与个人概览

> 对应 SPEC §6.5。两个端点均已接线，业务层 `src/web/user_api.cpp`；聚合口径与 `admin.cpp` 同源（§6.5 口径一致性契约）。

### 6.1 `GET /api/stats` ✅

轻量计数，供顶栏等「只要一个数字」的场景使用。

**成功响应** `200 OK`

```json
{ "code": 0, "message": "ok", "data": { "total": 128, "solved": 37, "attempted": 52, "favorites": 8 } }
```

| `data` 字段 | 说明 |
|---|---|
| `total` | 我的提交总数 |
| `solved` | 已通过的**题数**（存在至少一条 AC 提交的题） |
| `attempted` | 尝试过的题数（含只 CE 过的题） |
| `favorites` | 收藏的题数 |

**失败响应**：`1002`、`1008`。

> **为什么不用 `/api/me/overview` 顶替**：`/api/stats` 是 4 个标量 `COUNT`；`/api/me/overview` 要跑 `byTag` 的多表 `GROUP BY`，代价差一个数量级。顶栏不该为「一个已通过数」付聚合的钱。**但凡要展示分布的，一律走 overview** —— 单一数据源保证概览条与「我的」tab2 数值必然一致。

---

### 6.2 `GET /api/me/overview` ✅

概览条与「我的」tab2 的**唯一数据源** `[D-38] [D-39]`。

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "totals": { "solved": 37, "total": 50, "attempted": 52, "submissions": 128, "favorites": 8 },
    "byDifficulty": [ { "difficulty": "easy",   "total": 18, "solved": 15, "attempted": 17 },
                      { "difficulty": "medium", "total": 22, "solved": 16, "attempted": 25 },
                      { "difficulty": "hard",   "total": 10, "solved":  6, "attempted": 10 } ],
    "byTag":        [ { "name": "动态规划", "category": "algorithm", "total": 12, "solved": 3, "attempted": 9 },
                      { "name": "高频面试题", "category": "source",     "total": 30, "solved": 20, "attempted": 28 } ],
    "recent":       [ { "id": 91, "problemSlug": "two-sum", "problemTitle": "两数之和",
                         "verdict": "WA", "createdAt": "2026-10-14 20:01:02" } ]
  }
}
```

| `data` 块 | 说明 |
|---|---|
| `totals` | 已通过 X/N 等概览条数字 |
| `byDifficulty[]` | `difficulty` / `total`（该难度题数）/ `solved` / `attempted`；三条 `total` 之和 = 题目总数 |
| `byTag[]` | `name` / `category` / `total` / `solved` / `attempted`，按 `total DESC, name` 排序 |
| `recent[]` | 最近 3 条提交，供概览条展示 |

> **字段名来源**：`totals` / `byDifficulty` / `byTag` / `recent` 四个块名由 SPEC §6.3 写死；`byDifficulty[]` 与 `byTag[]` 的元素字段（`difficulty`/`name`/`category`/`total`/`solved`/`attempted`）直接对应 SPEC §6.5 的 SQL 别名；**`totals` 的内部字段名与 `recent[]` 的元素字段名在 SPEC 中未定义**，上表为建议值，实现时需与前端一次性约定。

**失败响应**：`1002`、`1008`。

> **头号陷阱（已在 SPEC 中列为「按标签聚合会重复计数」）**：`problem_tags` 是多对多。一道挂 3 个标签的题若先 JOIN 标签表再 `COUNT(*)` 会被**计三次**。正确写法是**先各自 `SELECT DISTINCT problem_id` 收敛成集合，再 JOIN**；`byDifficulty` 基表不 JOIN 标签表，故 `COUNT(*)` 即可。两者写法不同不是笔误。
> **口径一致性契约**（三处必须同源，SPEC §6.5）：`solved` = 存在**至少一条** AC 提交（不是「最后一次」）；`attempted` = 存在**至少一条任意状态**提交，**包含只有 CE 的题**。
> 所有聚合必须在 SQL 内完成，**不得**把 `submissions` 全表捞回应用层聚合（P95 < 200 ms 的硬要求）。

---

## 7. 管理

### 7.1 `GET /api/admin/overview` ✅

**只读**管理面板的数据源 `[D-37]`。业务层 `src/web/admin.cpp` 已实现并被单测覆盖，HTTP 路由已接线。

**授权守卫** `require_admin`：先判登录（无会话 → `1002`），再判角色（非 admin → `1003`）。顺序不可颠倒，否则「没登录」会被报成「无权限」。

**成功响应** `200 OK`

```json
{
  "code": 0, "message": "ok",
  "data": {
    "system": {
      "userCount": 5, "adminCount": 1, "problemCount": 50, "caseCount": 612,
      "submissionCount": 1284, "acCount": 736, "failedCount": 2,
      "judgingCount": 3, "queueLength": 1, "activeJudges": 2, "judgeAlive": true
    },
    "users": [
      { "username": "alise", "role": "admin", "createdAt": "2026-10-11 12:00:00",
        "submissionCount": 980, "acCount": 610, "solved": 34, "attempted": 41, "favorites": 9,
        "lastSubmitAt": "2026-10-14 20:01:02", "lastVerdict": "WA" }
    ]
  }
}
```

**`system` 块**

| 字段 | 来源 | 说明 |
|---|---|---|
| `userCount` / `adminCount` | DB | 用户总数 / 管理员数 |
| `problemCount` / `caseCount` | DB | 题目数 / `problem_cases` 真实行数 |
| `submissionCount` / `acCount` / `failedCount` | DB | 提交总数 / AC 提交数 / `status='failed'` 数 |
| `judgingCount` | DB | **未完成**提交数（`status IN ('pending','judging')`） |
| `queueLength` | **进程内** | 内存队列中还没开始的条数（不等于 DB 里 pending 的行数） |
| `activeJudges` | **进程内** | 当前评测并发数 |
| `judgeAlive` | **进程内** | 调度器是否存活；judge 未接线时为 `false` |

**`users[]` 块**（按用户 id 升序，即注册顺序）

| 字段 | 说明 |
|---|---|
| `username` / `role` / `createdAt` | 基础信息（UTC） |
| `submissionCount` / `acCount` | 提交总数 / AC 条数 |
| `solved` / `attempted` | 与 §6.5 **完全同口径**：AC 过的题同时计入两者（不是互斥） |
| `favorites` | 收藏题数 |
| `lastSubmitAt` / `lastVerdict` | 最近提交时间与结论；无提交时为 `null`；最新一条还在排队（`verdict IS NULL`）时 `lastVerdict` **也是 `null`**（不静默回退到上一条 AC） |

**失败响应**

| code | HTTP | 触发 |
|---|---|---|
| `1002` | 401 | 未登录或会话过期 |
| `1003` | 403 | 已登录但 `role != 'admin'` |
| `1008` | 500 | DB 异常 |

**实现约束（接线时必须遵守）**

1. **锁顺序：先取 scheduler 快照 → 释放其锁 → 再取 DB 租约**。反过来会引入一条 `DB mutex → scheduler lock` 的嵌套边，与 §2.3 的既有锁顺序冲突（与 judge 写回事务的 `scheduler → DB` 边构成死锁四要素）。
2. `system` 的 8 个 DB 计数合成**一条 SQL** 的标量子查询（串行化 DB 下，八次往返就是八次停顿）。
3. `users[]` **不是** per-user 相关子查询风暴，而是先把 `submissions` / `user_problems` 各 `GROUP BY user_id` 收敛成小表再 `LEFT JOIN users`。
4. **严格只读**：没有增删改、不能禁用用户、不能改角色、不能在线编辑题库、不能 impersonation、不能查看他人提交。角色只经 `.env` + 重启调整。

---

## 8. 静态资源与全局收口

### 8.1 `GET /*` ✅

`static/` 目录挂载到 `/`，提供五页前端与 vendored 三库：

| 路径 | 内容 |
|---|---|
| `/login.html` `/list.html` `/problem.html` `/history.html` `/admin.html` | 五页前端 `[D-36] [D-37]` |
| `/js/*.js` | 请求层与页面逻辑（`api.js` `auth.js` `list.js` `problem.js` `history.js` `poll.js` `overview.js` `diff.js` `admin.js` …） |
| `/css/*.css` `/vendor/**` | 样式与 vendored 的 marked / DOMPurify / CodeMirror（`D-73`，无运行时外网依赖） |

**行为要点**

- 静态挂载注册在**所有 API 路由之后**，httplib 优先精确匹配已注册路由，`/api/*` 不会被静态目录抢走。
- **静态文件不存在 ⇒ 返回 JSON 包络 `404 / code=1004`，不是 HTML 404 页**。这是与 SPEC §6.3 静态段（"返回 `404.html` 或重定向到 `list.html`"）的**实现差异**，见 §9.2。
- 响应 `Content-Type` 一律由 `write_body` 覆写为 `application/json; charset=utf-8`，避免出现「`Content-Type: text/html` 却收到一段 JSON」。
- **CORS 未开启**：前后端同源部署。前端 `api.js` 用 `credentials: 'same-origin'`。

### 8.2 全局收口速查

| 场景 | HTTP | 包络 |
|---|---|---|
| 访问不存在的 API 路径 | 404 | `{"code":1004,"message":"资源不存在","data":{}}` |
| 对存在的路径用错方法（如 `GET /api/auth/login`） | 405 | 同上（`code=1004`） |
| 访问不存在的静态文件 | 404 | 同上 |
| 处理器抛异常 | 500 | `{"code":1008,"message":"服务器内部错误","data":{}}`，详情进 `app.log` |
| 业务错误（4xx/5xx） | 原样 | **不被改写**（body 已由业务层写过） |

---

## 9. 实现状态矩阵与文档差异

### 9.1 端点实现状态

| 端点 | 路由接线 | 业务层 | 单测 | 可用性 |
|---|---|---|---|---|
| `POST /api/auth/register` | ✅ | ✅ `auth_api.cpp` | ✅ | **可用** |
| `POST /api/auth/login` | ✅ | ✅ | ✅ | **可用** |
| `POST /api/auth/logout` | ✅ | ✅ | ✅ | **可用** |
| `GET /api/auth/me` | ✅ | ✅ | ✅ | **可用** |
| `GET /*`（静态） | ✅ | — | ✅ `routes_test.cpp` | **可用** |
| `GET /api/tags` | ✅ | ✅ `problem_api.cpp` | ✅ | **可用** |
| `GET /api/problems` | ✅ | ✅ | ✅ | **可用** |
| `GET /api/problems/{slug}` | ✅ | ✅ | ✅ | **可用** |
| `GET /api/admin/overview` | ✅ | ✅ `admin.cpp` | ✅ | **可用** |
| `POST /api/auth/password` | ✅ | ✅ `user_api.cpp` | — | **可用**（curl 已验） |
| `DELETE /api/auth/account` | ✅ | ✅ | — | **可用** |
| `POST /api/problems/{slug}/favorite` | ✅ | ✅ | — | **可用** |
| `GET /api/stats` | ✅ | ✅ | — | **可用** |
| `GET /api/me/overview` | ✅ | ✅ | — | **可用** |
| 提交组读侧 4 个（列表/详情/diff/删除） | ✅ | ✅ `submission_api.cpp` | — | **可用** |
| `POST /api/submissions` | ✅ | ✅ | — | 🟠 走 `1009`（judge 未接线） |
| `POST /api/submissions/{id}/retry` | ✅ | ✅ | — | 🟠 走 `1009`（judge 未接线） |

**尚未接线的两块**：judge 调度器（T2.7：`src/judge/{queue,scheduler,worker,runner,generator}.*` 仍是空文件）与
限流（`include/web/ratelimit.h` 空文件）。前者挡住 `POST /api/submissions` 与 `/retry` 的 201 分支，
后者让 `1006 / 1007` 两个码暂时不可达。包络（`common::response.h`）与 Cookie → `ViewerContext`
（`web::session.h` 的 `authenticate()`）在本轮接线时已就位。

### 9.2 与 SPEC 的已知差异 / 开放问题

| # | 项 | SPEC | 当前实现 | 影响 |
|---|---|---|---|---|
| 1 | 静态文件 404 | 返回 `404.html` 或重定向到 `list.html` | JSON 包络 `404 / 1004` | 前端 `api.js` 的「先判 JSON 再判状态」逻辑正是为此写的；改回 HTML 需同步改前端 |
| 2 | 413 收口 | 请求体超限 → 1001 | 映射已写好，但**未设置 `payload_max_length`**，httplib 默认 100 MB | 分支实际不可达；64 KB 代码上限应由 `OJ_MAX_CODE_BYTES` 在业务层拦截 |
| 3 | 角色同步 | 启动时按 `OJ_ADMIN_USERNAMES` 双向同步 | **未接线（T3.11）** | 改 `.env` 并重启不生效；`role` 目前就是库里的既有值（启动回显里已打这句提示） |
| 4 | `queueAhead` 的全局闸门 | 全局 8 | 配置现值 **4**（`OJ_JUDGE_GATE`，= 2 × worker 2）`[D-74]` | 不改契约，只改配置；兜底墙钟余量由 1.46× 升到 2.9× |
| 5 | 限流 | `1006` / `1007` | `ratelimit.h` 空文件 | 提交组接线时必须补，否则配额形同虚设 |
| 6 | 方法不被接受 | 405 | 404（httplib 0.60 不产生 405） | 码仍是 `1004`，前端无感；`routes.cpp` 的 `case 405` 不可达 |
| 7 | 提交 / 重跑 | 201 + `submissionId` | 校验通过后回 `1009 / 503` 且不落库 | judge 调度器（T2.7）未接线；接上 `judge_available()` 即可 |

---

## 10. 附录

### 10.1 错误码 → HTTP 对照（可复制）

```
0    → 200 (201 仅注册)   1001 → 400   1002 → 401   1003 → 403
1004 → 404   1005 → 409   1006 → 429   1007 → 429   1008 → 500
1009 → 503   1010 → 403   1011 → 409   2001~2005 → 200（非错误）
```

### 10.2 判题结论（`verdict`）与 2xxx 码的关系

`2001`–`2005` **不是 API 异常**：HTTP 恒为 200，通过 `data.verdict` 表达。`CE / TLE / MLE / WA / RE` 的对应关系：

| verdict | 业务码 | 含义 |
|---|---|---|
| `AC` | `0` | 全部用例通过 |
| `WA` | `2004` | 答案错误 |
| `TLE` | `2002` | 运行超时（`RLIMIT_CPU` 超限 / CPU 停滞检测 / 兜底墙钟） |
| `MLE` | `2003` | 内存超限（`RLIMIT_AS` 超限） |
| `RE` | `2005` | 运行错误（非零退出码） |
| `CE` | `2001` | 编译错误（用户代码 / 平台 harness / 链接签名不匹配） |
| `JE` | — | **平台错误**（`@@OJ@@` 一行都没产出 / worker 写回失败），非用户问题 |

**顶层 verdict 收敛优先级**：`JE > TLE > MLE > RE > WA > AC`（基础设施错误 > 资源超限 > 运行崩溃 > 答案错）。

### 10.3 进度口径（`myProgress`）

```
CASE WHEN EXISTS(… verdict='AC')  THEN 'solved'
     WHEN EXISTS(… 任意提交)        THEN 'attempted'
     ELSE 'none' END
```

- **AC 优先**，因此「做过并通过的题」只出现在 `solved`，三个筛选值互斥。
- **「存在至少一条」而非「最后一次」**：某次蒙对了、后来改坏再失败，不会把已通过的题降级回 `attempted`。
- `attempted` **包含只有 CE 的题**（列表页筛选用、`byTag`/`byDifficulty` 聚合用，三处不得分叉）。
- 同一份 CASE 表达式在列表、详情、推荐三处**只有一处定义**。

### 10.4 派生列（`caseTotal` 等）为什么值得前端注意

| 字段 | 定义 | 场景 |
|---|---|---|
| `casePassed` | `COUNT(passed=1)` | CE 时为 0 |
| `caseTotal` | `GREATEST(实跑明细行数, 题库当前用例数)` | **CE 时是 12 而不是 0** ⇒ 状态行显示「编译错误 · 12 个用例未执行」 |
| `totalMs` / `maxMemKb` | `COALESCE(SUM(time_ms),0)` / `COALESCE(MAX(mem_kb),0)` | TLE/MLE 用例度量为 NULL，被聚合跳过；全被跳过时收口为 0 |

### 10.5 内存口径分裂（必须原样透传给用户）`[D-63]`

| 角色 | 口径 |
|---|---|
| **限制**（超了会死） | `RLIMIT_AS` = **虚拟地址空间**，默认 128 MB |
| **度量**（展示给用户） | `ru_maxrss` = **常驻集 RSS**，且是进程生命周期峰值 |

后果：用户可能看到「内存超限（地址空间 128MB）」，而明细行里的峰值 RSS 只有 30 MB —— **这是口径差异，不是 bug**。UI 必须分别标注（徽标写「内存超限 · 地址空间 128MB」，列表头写「峰值RSS(参考)」），两者不得并列成同一个数字。

### 10.6 curl 速验

**全量验收脚本**（20 个端点 + 错误分支，98 条断言；需 `jq` 与 `mysql` 客户端）：

```bash
make run            # 另开一个终端起服务
tests/run_tests.sh  # 逐条打印 PASS/FAIL，末尾给出通过/失败计数
```

它只做一件事：把本文档里的每个契约变成一条 `curl`，并**同时断言 HTTP 状态码与响应里的
`code`**（[D-29] 要求两者同步，而它们出自两条不同的代码路径）。提交读侧那几个端点需要库里有
提交行，脚本用 SQL 造出 judge 写回会写的那种行，再从 HTTP 侧验归属、分页、diff、删除、重跑。

下面是几条最常手工敲的命令：

```bash
BASE=http://127.0.0.1:8080

# 注册 → 201
curl -i -X POST $BASE/api/auth/register -H 'Content-Type: application/json' \
  -d '{"username":"alise","password":"pass1234"}'

# 登录 → 200 + Set-Cookie（把 Cookie 存下来复用）
curl -i -c /tmp/oj.cookie -X POST $BASE/api/auth/login -H 'Content-Type: application/json' \
  -d '{"username":"alise","password":"pass1234"}'

# 我是谁 → 200 / 1002
curl -s -b /tmp/oj.cookie $BASE/api/auth/me

# 登出 → 200 + 清 Cookie（幂等，可重复执行）
curl -i -b /tmp/oj.cookie -X POST $BASE/api/auth/logout

# 未登录的码（对照 §1.2）
curl -s $BASE/api/auth/me                                   # 1002 / 401
curl -s $BASE/api/problems                                   # 200 / 0（匿名可读，[D-43]）
curl -s $BASE/api/tags                                       # 200 / 0
curl -s $BASE/api/stats                                      # 1002 / 401（要登录）
curl -s -b /tmp/oj.cookie $BASE/api/me/overview              # 200 / 0
curl -s -b /tmp/oj.cookie $BASE/api/admin/overview           # 200 / 0（admin）/ 1003（非 admin）
curl -s -b /tmp/oj.cookie -X POST $BASE/api/submissions \
     -H 'Content-Type: application/json' \
     -d '{"problemSlug":"two-sum","code":"class Solution{};"}'   # 1009 / 503（judge 未接线）
curl -s $BASE/nonexistent.html                               # 1004 / 404（静态不存在）
```

### 10.7 相关源码索引

| 关注点 | 文件 |
|---|---|
| 路由注册、静态挂载、错误/异常收口 | `src/web/routes.cpp` / `include/web/routes.h` |
| 注册/登录/登出业务逻辑 | `src/web/auth_api.cpp` / `include/web/auth_api.h` |
| 会话签发/校验/回收、Cookie 属性 | `src/web/session.cpp` / `include/web/session.h` |
| 统一响应包络 | `src/common/response.cpp` / `include/common/response.h` |
| 业务码 → HTTP 映射与文案 | `src/common/errors.cpp` / `include/common/errors.h` |
| 题目列表/详情/推荐 | `src/web/problem_api.cpp` / `include/web/problem_api.h` |
| 改密/注销/收藏/统计/概览 | `src/web/user_api.cpp` / `include/web/user_api.h` |
| 提交组（新建/列表/详情/diff/删除/重跑 + `judge_available()`） | `src/web/submission_api.cpp` / `include/web/submission_api.h` |
| curl 端到端验收 | `tests/run_tests.sh`（单测：`tests/run_unit_tests.sh`） |
| 只读管理端点 | `src/web/admin.cpp` / `include/web/admin.h` |
| 前端请求层 | `static/js/api.js`、`static/js/auth.js` |
| 运行期配置（`.env` 键） | `include/common/config.h`、`.env` |