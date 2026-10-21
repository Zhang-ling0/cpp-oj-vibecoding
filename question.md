# CppOJ「看起来能用但实际跑不通」问题清单与解决方案

> 审计日期：2026-10-20
> 审计范围：普通用户前端数据来源、提交链路、判题引擎、管理面板、资源约束（2 核 / 1967MB）
> 审计方式：代码静态审计 + 活库数据核对（HEAD `97aef24` V3.8）

---

## 0. 总体结论

系统在「界面、路由、数据结构」三层都是完整的，但有三类断裂导致它跑不通：

1. **判题引擎整体缺失** —— 5 个源文件 + 5 个头文件全是 0 字节，提交链路被一个硬编码的 `false` 拦死。
2. **库里的"判题结果"是造出来的** —— 造数工具直接 INSERT 成 judge 写回后的样子，前端所有历史/verdict/逐用例明细都是假数据。
3. **配置项在说谎** —— 限流、配额、线程数三项被解析、被启动日志打印，但运行时零消费。

已确认存在的核心断裂（用户可见症状）：

| 症状 | 根因 | 位置 |
|---|---|---|
| 提交返回 1009「判题服务不可用」，不落库 | `judge_available()` 硬编码 `return false` | `src/web/submission_api.cpp:154` |
| 提交历史全是假的 | 造数工具伪造 verdict/明细 | `tools/reset_db.cpp:876-940`（**阶段 0 已修**：伪造插入已删除，三表恒 0 行 + 自检） |
| 管理面板只有只读卡片 | D-37 刻意只读，CRUD 未实现 | `src/web/admin.cpp` |
| 判题跑不起来 | `src/judge/{queue,scheduler,worker,runner,generator}.cpp` 与对应 `.h` 全 0 字节 | `src/judge/` |
| 限流/配额不生效 | `src/web/ratelimit.cpp` 0 字节，配置零消费 | 见 P0-1 |

---

## 1. 问题清单（按严重度）

### P0 · 阻断 / 配置在撒谎

#### P0-1 限流与配额三件套全部是摆设

- **证据**
  - `src/web/ratelimit.cpp` = 0 字节
  - `include/common/config.h:95` / `src/common/config.cpp:351-354` 解析 `OJ_RATE_WINDOW_SECONDS`、`OJ_RATE_MAX_SUBMITS`、`OJ_JUDGE_USER_GATE`
  - `src/common/config.cpp:578-579` 启动日志打印「单用户每 60s 最多 10 次提交」
  - grep `rate_max_submits|rate_window_seconds|judge_user_gate` 在 `src/`（除 config.cpp 外）**零命中**
  - 前端 `static/js/problem.js:386-387` 已写好 1006/1007 的话术，但永远触发不了
- **触发条件**：任何时候；判题引擎上线后风险被放大（无限连点提交可打满 2C/2G 的队列）
- **修法**：实现滑动窗口限流（→ 1006）+ 单用户并发评测闸（→ 1007），检查点置于 `handle_submission_create` 建行之前

#### P0-2 `OJ_HTTP_THREADS` 是死配置

- **证据**
  - `include/common/config.h:78` 解析为 `http_threads`
  - `src/web/routes.cpp:13` `#define CPPHTTPLIB_THREAD_POOL_COUNT 8`（编译期宏）
  - `include/web/routes.h:24-30` 注释自述「宏按 §2.3 固定 8，`config().http_threads` 留给 httplib 没有的用途」
- **触发条件**：运维改 `.env` 的 `OJ_HTTP_THREADS` → 完全无效，且 `dump_config` 仍打印该值
- **修法**：改用 httplib 的运行期 `set_thread_pool_count()` 读配置；2 核 2G 场景建议默认 4

#### P0-3 判题引擎缺失（核心断裂）

- **证据**：`src/judge/{queue,scheduler,worker,runner,generator}.cpp` 与 `include/judge/{queue,scheduler,worker,runner,generator}.h` 共 10 个文件全 0 字节；`src/db/dao.cpp`、`include/db/dao.h` 0 字节
- **触发条件**：任何提交 → `judge_available()` 返回 false → 1009 且不建行
- **修法**：见 §2 阶段 1

#### P0-4 库中判题结果是伪造的 ✅ 已修复（阶段 0，2026-10-21）

- **证据**：`tools/reset_db.cpp:876-940` 直接 INSERT 成 judge 写回后的样子（40 提交 / 95 明细 / 6 诊断）
- **触发条件**：`make run` 后打开「我的」页、题目页 tab3、概览条 —— 看到的全部 verdict 都不是判出来的
- **现状**：伪造插入逻辑（`Kind` / `SubSeed` / `kSubmissions` / `insert_submission` / `insert_case_results`）
  已整体删除；`oj-reset` 造完自检「提交三表必须 0 行」，非 0 直接失败；活库已清空并重跑题库同步

---

### P1 · 用户会直接撞上的逻辑错误

| # | 问题 | 证据 | 触发条件 | 修法 |
|---|---|---|---|---|
| P1-5 | **历史页空页死局**：后端不夹页码，前端注释却声称夹；删掉最后一页最后一条后停在空页，**分页器直接消失**，用户被锁死只能刷新 | 后端 `src/web/submission_api.cpp:234` `list.page = query.page` 原样回显；前端 `static/js/history.js:173` 假注释；`static/js/history.js:186` `renderPager` 仅在 `list.length > 0` 时执行 | 在最后一页删掉最后一条记录 | 后端把 `page` 夹回真实最大页（或前端收到空页且 `total>0` 时 `state.page = pages` 后重取）；同时改掉假注释 |
| P1-6 | **列表页同类假注释**：`state.page` 可 > `pages`，上一页按钮未按 `pages` 夹值 | `static/js/list.js:234-236` vs `src/web/problem_api.cpp:609`；`static/js/list.js:243` 无条件渲染 pager（故比 P1-5 轻） | 筛选变化后页码越界 | 同 P1-5：服务端夹页 + `renderPager` 按 `pages` 夹 `data-page` |
| P1-7 | **展开行点不动**：A 已展开时点 B，只收起 A 不打开 B，需再点一次 | `static/js/history.js:239-244`（注释称故意，但与直觉冲突） | 历史页连续查看多条详情 | 改为「点同一行=收起，点其它行=切换到该行」 |
| P1-8 | **概览条 TypeError 卡死**：`me` 请求失败而 `overview` 成功时 `user=null`，`user.username` 抛异常，永久停在「正在读取你的刷题进度…」 | `static/js/overview.js:112`；`static/js/list.js:57-58` 直接透传 | 首次加载时 `/api/auth/me` 网络抖动 | `escapeHtml(user?.username ?? '')`，并为 `null` 时不渲染问候行 |
| P1-9 | **删题 = 级联删光所有用户历史**：11 条外键全 `ON DELETE CASCADE`，loader 刻意不删正是因为此 | `db/init/01-schema.sql:24`；`src/problem/loader.cpp:313` | 管理台点击删除题目 | **已决策：改为「下线归档」**，不物理删（见 §3） |
| P1-10 | **`language` 无白名单**：POST `language=python` 直接落库 → 判题器按 C++ 编译 → 用户收到莫名 CE | `src/web/routes.cpp:545-547` 仅填默认值 | 手工构造请求 | 建行前白名单校验（首版只放行 `cpp`），非白名单 → 1001 |

---

### P2 · 冗余 / 低危

| # | 问题 | 证据 | 修法 |
|---|---|---|---|
| P2-11 | 两个**逐字重复**的受保护页面 lambda | `src/web/routes.cpp:1011`（history.html）与 `:1035`（admin.html） | 抽成 `serve_protected_page(path)` 工具函数 |
| P2-12 | 受保护页拿不到 `Cache-Control: no-cache`（只有静态挂载有）——恰是项目注释强调的高排查成本坑 | `src/web/routes.cpp:1074` 只作用于 `set_mount_point`；`:1051`/`:1075` 手动 `set_content` 无该头 | 两个 lambda 补 `res.set_header("Cache-Control", "no-cache")` |
| P2-13 | 死文件 | `static/js/diff.js` 0 字节且全项目无引用；`static/js/editor.js:11` 自述 `vendor/cm/` 5 个 CM 文件已死 | 删除 |
| P2-14 | 匿名打开题目页 tab3 显示「还没有提交记录」，应提示登录 | `src/web/problem_api.cpp:646` 仅 viewer 查询 → 匿名给空数组；`static/js/problem.js:181` | 匿名时渲染「登录后可看你的提交」 |

---

### 已验证无问题（避免重复排查）

- **参数名前后端一致**：`problemSlug` / `verdict` / `favorite` / `progress` / `page` / `size`
  - `src/web/submission_api.cpp:159-198`、`src/web/problem_api.cpp:455-486` 与前端 `static/js/list.js:203-214`、`static/js/history.js:161-166` 完全对应
- **匿名误带个人化参数会被拦**：`problem_api.cpp:490-501` 回 1001，前端 `list.js:29-36` 已置灰这两个控件
- **归属校验全写进 SQL WHERE**：`delete_submission` / `load_submission_diff` / `load_submission_detail` / `retry_submission` 均带 `AND user_id = ?`，`kNotOwned` → 1003
- **开放重定向防护**：`static/js/auth.js:80-86` 拒绝 `//`、`/\`、`/login.html` 自跳
- **Markdown 白名单 + DOMPurify**：`static/js/markdown.js`，无 `img`、无 `script`
- **管理面板权限流程正确**：`admin.cpp:67-73` 先登录后角色；`admin.js:70-75` 1003 先提示再 1.5s 跳回
- **轮询终态清理**：`poll.js:71-83` `done/failed` → `stop()` → 清 `sessionStorage`
- **静态资源 `Cache-Control: no-cache` 已覆盖**：`routes.cpp:1074`
- **编译闸门已实现**：`src/judge/compiler.cpp`（23.8KB 完整）+ `MALLOC_ARENA_MAX=1` 子进程设置
- **工作目录管理已实现**：`src/judge/workspace.cpp`（10KB 完整）
- **题库同步已实现**：`src/problem/loader.cpp`（25KB 完整）启动时 `load_all()` upsert 进库

---

## 2. 解决方案（四阶段）

> 决策：**先交付阶段 0+1（可用闭环），验收判题正确性后再做 2+3。**

### 阶段 0 · 清假数据

| 步骤 | 动作 | 位置 |
|---|---|---|
| 0.1 | 删除伪造 submissions / submission_case_results / submission_compile_errors 的插入逻辑，造数工具只产题目 + 用户 | `tools/reset_db.cpp:876-940` 及 `insert_submission` / `insert_case_results` |
| 0.2 | 活库清空：`TRUNCATE submissions, submission_case_results, submission_compile_errors, user_problems`，重跑 `problem::load_all` | 运行期 SQL |
| 0.3 | 对齐文档中依赖假数据的断言（固定行数 / 固定 verdict） | `基于curl的接口自动化测试文档.md`、`web自动化测试文档-paywright.md` |

### 阶段 1 · 判题引擎（5 新建 + 2 复用 + 4 接线）

| 步骤 | 文件 | 关键约束 |
|---|---|---|
| 1.1 | `include/judge/{queue,scheduler,worker,runner,generator}.h` | 现全 0 字节，头文件需一并建 |
| 1.2 | `src/judge/queue.cpp` | 内存队列 + condvar 唤醒；**FIFO 按 id 升序**（`create_submission` 的 `queueAhead` 已硬依赖此契约，见 `submission_api.cpp:401-410`） |
| 1.3 | `src/judge/scheduler.cpp` | `workers=2`、`gate=4`（`config.cpp:388` 硬校验 `gate == 2 × workers`）；fence 需满足 `gate×3000 < 3000×OJ_FENCE_MULTIPLIER + 5000` 即 `4×3000 < 35000` ✓；沿用 `.env` 现值，不改配置 |
| 1.4 | `src/judge/worker.cpp` | 取任务 → 编译 → 逐用例 → 写回；verdict 按 SPEC §5.6.2 判定表；**D-65**：CE 时 `case_total` = 题库用例数、`case_passed` = 0；**D-67**：JE 零明细行；复用已有 `compiler.cpp` 与闸门 |
| 1.5 | `src/judge/runner.cpp` | fork + `RLIMIT_AS = memLimitKb` + CPU 时间墙 + `MALLOC_ARENA_MAX=1`；fence = `OJ_FENCE_MULTIPLIER=10`；复用 `workspace.cpp` |
| 1.6 | `src/judge/generator.cpp` | 从题库生成 harness；输入形态已摸清：param 仅 `std::vector<int>&` / `int` / `std::string`，return 仅 `int` / `bool` / `std::vector<int>` / `std::string`，**无 `__expr` 用例** |
| 1.7 | `src/db/dao.cpp` + `include/db/dao.h` | 写回**单事务**（submissions + case_results + compile_errors）；启动恢复：pending/judging 超时 → JE，不留死轮询 |
| 1.8 | 接线 4 处 | ① `submission_api.cpp:154` `judge_available()` 改读调度器存活<br>② `main.cpp:363` 启动调度器<br>③ `main.cpp:332` 角色同步 T3.11 接线<br>④ `routes.cpp:847-851` 注入真实 StatusProvider 替代 `unwired_status()` |
| 1.9 | 顺手修 P1-5 / P1-6 / P1-8 | `history.js:186`、`list.js:234`、`overview.js:112` —— 属于「跑得起来但点不动」，与阶段 1 一起验收 |

**阶段 1 验收**：`make build JOBS=2 && make test` → `make run` → 提交 AC / CE / TLE / RE 各一条真代码，确认 verdict、逐用例表、编译诊断、历史页全部来自真数据；再跑 playwright 手册 M05–M12。

### 阶段 2 · 管理端 CRUD（D-37 变更，需在 SPEC.md / API.md 记录）

| 步骤 | 动作 |
|---|---|
| 2.1 | `src/web/ratelimit.cpp` 实现滑动窗口限流（→ 1006）+ 单用户并发评测闸（→ 1007），检查点置于 `handle_submission_create` 建行之前 —— 解决 P0-1 |
| 2.2 | `src/web/admin.cpp` 扩写：题目列表 / 新建 / 编辑 / **下线归档**（不物理删）、提交列表 / 删除 / 重判、用户改角色 / 停用、统计导出 |
| 2.3 | `problems/<slug>.json` 写回（文件仍是唯一真源 D-30），写完复用 `problem::load_all` 的单题 upsert 同步入库，**不需重启** |
| 2.4 | `static/admin.html` + `static/js/admin.js` 四 tab 管理台 |
| 2.5 | 修 P1-7 / P1-10 / P1-14（展开行切换、language 白名单、匿名 tab3 文案） |
| 2.6 | `OJ_HTTP_THREADS` 落地为运行期生效 —— 解决 P0-2 |

### 阶段 3 · 资源约束与收尾

| 步骤 | 动作 |
|---|---|
| 3.1 | 父进程 `MALLOC_ARENA_MAX=1`；HTTP 线程默认 4（2C2G）；`workers=2` / `gate=4` 按 `.env` |
| 3.2 | 抽 `serve_protected_page()` 消除重复 lambda（P2-11） |
| 3.3 | 受保护页补 `Cache-Control: no-cache`（P2-12） |
| 3.4 | 删 `static/js/diff.js`、`vendor/cm/` 死文件（P2-13） |
| 3.5 | 完整回归：`make build JOBS=2 && make test` + 起服务跑 playwright 手册全量 |

---

## 3. 已确认的三项决策

| 决策点 | 选择 | 影响 |
|---|---|---|
| 管理台删题策略 | **下线归档，不物理删** | 新增归档标记（或复用 status），列表 / 详情 / 提交入口对已归档题不可见，历史数据零风险；`loader.cpp:313` 的规避逻辑保持不变 |
| 三个摆设配置 | **全部实现** | 限流 / 配额在阶段 2 打通 1006/1007；`OJ_HTTP_THREADS` 改运行期生效；`dump_config` 打印与真实行为对齐 |
| 实施范围 | **先 0+1，验收后再 2+3** | 先交付「能真提交真判题」的闭环，你验证判题正确性后再动管理台与资源收尾 |

## 4. 资源与工程约束（不可违反）

- 机器：2 核 / 1967MB，可用内存约 315MB，swap 已用 1GB，mysqld 常驻
- 构建：`make build JOBS=2`（**JOBS 不可调大**）、`make test` 串行跑、`make run` 起 mysqld + `./build/oj`
- CMake 是唯一构建真源；**不支持同步编译**
- 判题并发沿用 `.env`：`OJ_JUDGE_WORKERS=2`、`OJ_JUDGE_GATE=4`、`OJ_JUDGE_USER_GATE=2`
- 文件仍是题目唯一真源（D-30），DB 是它的投影
- D-37「admin 严格只读」被显式修改，必须在 SPEC.md / API.md 记为变更

## 5. 开工前风险提示

- `src/web/routes.cpp` 在审计当天（2026-10-20 16:44）被改动过，仓库中同时存在 VS Code server 与两个 agent 进程。
- **若存在并发修改者，阶段 1 应先只动 `src/judge/`、`src/db/dao.*`、`tools/`，避开 `routes.cpp`**，待其停止后再执行 1.8 接线。

---

## 6. TODO 清单

### 阶段 0 · 清假数据 ✅ 已完成（2026-10-21）

- [x] **0.1** 改造 `tools/reset_db.cpp`：删除 `:876-940` 的伪造提交插入，只造题目 + 用户
- [x] **0.2** 活库清空 4 张表（submissions / submission_case_results / submission_compile_errors / user_problems），重跑题库同步
- [x] **0.3** 修正 curl 接口文档中依赖假数据的断言
- [x] **0.4** 修正 playwright 测试文档 M05–M12 中依赖假数据的断言
- [x] **0.5** `make build JOBS=2 && make test` 验证阶段 0 无回归（本机内存紧张，实际用 `JOBS=1` 串行编译；417 单测全绿 + `tests/run_tests.sh` 98/98）

> 阶段 0 实施时额外发现并修掉**两个让仓库完全编译不过**的既有破损（不在原清单里）：
> `include/web/admin.h` 首行多了一个 `q` 字符（commit `99cb5c8` 引入）；
> `src/web/routes.cpp:1024/1048` 两个受保护页面 lambda 用了 `options` 却没捕获。
> 阶段 1 开工前请先确认 `make build` 是绿的，否则接线工作无法验收。

### 阶段 1 · 判题引擎

- [x] **1.1** 建 `include/judge/{queue,scheduler,worker,runner,generator}.h` 五个头文件
- [x] **1.2** 实现 `src/judge/queue.cpp`（FIFO + condvar，按 id 升序）
- [x] **1.3** 实现 `src/judge/scheduler.cpp`（workers=2、gate=4、fence 校验）
- [x] **1.4** 实现 `src/judge/generator.cpp`（题库 → harness 生成）
- [x] **1.5** 实现 `src/judge/runner.cpp`（fork + RLIMIT_AS + 时间墙 + MALLOC_ARENA_MAX）
- [x] **1.6** 实现 `src/judge/worker.cpp`（编译 → 逐用例 → verdict 判定 → 单事务写回）
- [x] **1.7** 实现 `src/db/dao.cpp` + `include/db/dao.h`（写回事务 + 启动恢复置 JE）
- [x] **1.8** 接线①：`submission_api.cpp:154` `judge_available()` 改读调度器存活
- [x] **1.9** 接线②：`main.cpp:363` 启动判题调度器
- [x] **1.10** 接线③：`main.cpp:332` 角色同步 T3.11 接线
- [x] **1.11** 接线④：`routes.cpp` 注入真实 StatusProvider 替代 `unwired_status()`
- [x] **1.12** 修 P1-5：历史页空页死局（后端夹页 + 分页器兜底 + 改假注释）
- [x] **1.13** 修 P1-6：列表页页码越界与假注释
- [x] **1.14** 修 P1-8：概览条 `user=null` TypeError
- [x] **1.15** `make build JOBS=2 && make test`
- [x] **1.16** 端到端验收：提交 AC / CE / TLE / RE 各一条真代码，核对 verdict、逐用例表、编译诊断、历史页
- [x] **1.17** 跑 playwright 手册 M05–M12 回归

### 阶段 2 · 管理端 CRUD（判题验收通过后）

- [x] **2.1** 实现 `src/web/ratelimit.cpp` 滑动窗口限流（1006）
- [x] **2.2** 实现单用户并发评测闸（1007），接入 `handle_submission_create`
- [x] **2.3** `admin.cpp` 扩写题目管理 CRUD（新建 / 编辑 / 下线归档，不物理删）
- [x] **2.4** `admin.cpp` 扩写提交管理（列表 / 删除 / 重判）
- [x] **2.5** `admin.cpp` 扩写用户管理（改角色 / 停用）
- [x] **2.6** 实现 `problems/<slug>.json` 写回 + 单题同步入库（不重启）
- [x] **2.7** `admin.html` + `admin.js` 改四 tab 管理台
- [x] **2.8** 修 P1-7：历史展开行切换行为
- [x] **2.9** 修 P1-10：`language` 白名单校验
- [x] **2.10** 修 P1-14：匿名 tab3 文案
- [x] **2.11** 落地 `OJ_HTTP_THREADS` 运行期生效
- [x] **2.12** 在 SPEC.md / API.md 记录 D-37 变更（admin 由严格只读改为可写）
- [x] **2.13** `make build JOBS=1 && make test` + 管理台手工验收

> 阶段 2 实施时的额外发现与落地决策（不在原清单里，但必须留档）：
>
> 1. **新列迁移不能只改 `01-schema.sql`**：它是 `CREATE TABLE IF NOT EXISTS`（建新表才生效，
>    不给已有表加列），活库/测试库已有 `users`、`problems`。故新增列后同时在
>    `src/main.cpp` 加了幂等 `ensure_columns()`（查 information_schema → 按需 `ALTER TABLE`），
>    并在本次验收前手工对活库执行过一次等价 ALTER。**新库与老库两条路径都覆盖**。
> 2. **`users.disabled` 的登录闸放在口令比对【之后】**：放在前面会让登录接口变成
>    "该用户名存在且已停用"的枚举器（与 auth_api 既有防枚举口径一致）。
> 3. **P1-7 顺带修掉一个真 bug**：`history.js` 的 `toggleDetail()` 闭包引用了
>    `initHistoryPage()` 的局部 `state`（未作参数传入），展开任何一行详情都会
>    ReferenceError 停在"加载中…"。修行为的同时把 `state` 透传了进去。
> 4. **HTTP 线程池用固定 `httplib::ThreadPool(n)` 而不是 httplib 默认的动态池**：
>    默认允许涨到基数 × 4（32 线程 × 8MB 虚拟栈 = 256MB 虚拟地址），
>    在 1967MB + swap 已用 1GB 的机器上正是 SPEC §0.0 禁止的过量申请。
> 5. 编译全程 `JOBS=1`（与阶段 0/1 相同，机器内存不够 `JOBS=2`）。
> 6. **验收结果**：`make build JOBS=1` 绿；`make test` **417/417**；`tests/run_tests.sh` **147/147**
>    （原脚本里两条"judge 未接线 → 1009"是阶段 1 的过期断言，已改为验 201 + [D-58] 新增记录；
>    另新增 [21] 管理写侧 与 [22] 限流两节，覆盖 1006/1007 顺序、题目写回/归档、
>    提交删除/重判、改角色/停用与停用后登录 1011）。`OJ_HTTP_THREADS` 实测：改 4 →
>    启动日志与 dump_config 均打 4、进程线程数较 8 少 4（运行期生效，顺带修掉 main.cpp 里
>    写死"线程池 8"的启动日志）。
> 7. **管理台页面的浏览器级验收留给人工**（playwright-cli 未安装）：页面/脚本/接口三层已由
>    e2e + 静态 id 对账（admin.js 的 25 个 `querySelector('#…')` 与 admin.html 完全对应）覆盖。
>
### 阶段 3 · 资源与收尾

- [ ] **3.1** 父进程设置 `MALLOC_ARENA_MAX=1`，HTTP 线程默认 4
- [ ] **3.2** 抽 `serve_protected_page()` 消除重复 lambda（P2-11）
- [ ] **3.3** 受保护页补 `Cache-Control: no-cache`（P2-12）
- [ ] **3.4** 删除 `static/js/diff.js`、`vendor/cm/` 死文件（P2-13）
- [ ] **3.5** 全量回归：`make build JOBS=2 && make test` + playwright 手册全量
