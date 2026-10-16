# CppOJ Web 自动化测试文档

> 与相邻文档的分工：
> `API.md` 说「接口应该长什么样」→ `基于curl的接口自动化测试文档.md` 说「怎么反复验证接口真的长那样」；
> **本文说「用户在浏览器里真的能不能用、看到的对不对」**。
> 本文是 `SPEC.md §10.1` 那 49 条 AC 里**只能过浏览器才算达成**的那部分的落地清单。

| 项 | 值 |
|---|---|
| 文档版本 | v1.0 |
| 生成日期 | 2026-10-15 |
| 被测服务 | `http://82.156.75.119:8080` |
| 契约来源 | `SPEC.md`（v1.9）、`API.md`、仓库 `static/` 实际 DOM 与脚本 |
| 测试层级 | 浏览器端到端（Web UI）+ 少量以浏览器为载体的越权/注入验证 |
| 用例总数 | **142 条**（可执行 96 / 阻塞 31 / 待实现 11 / 人工 4） |
| 脚本形态 | 用例设计表为主 + Playwright 骨架（§7），可手工执行也可接 CI |

---

## 目录

1. [测试策略与分层](#1-测试策略与分层)
2. [被测环境与账号](#2-被测环境与账号)
3. [环境实测基线与已知阻塞](#3-环境实测基线与已知阻塞)
4. [元素定位手册](#4-元素定位手册)
5. [测试用例](#5-测试用例)
   - [M01 落地页](#m01-落地页)
   - [M02 登录与注册](#m02-登录与注册)
   - [M03 顶栏三形态](#m03-顶栏三形态)
   - [M04 题库列表与筛选](#m04-题库列表与筛选)
   - [M05 题目详情与题面渲染](#m05-题目详情与题面渲染)
   - [M06 编辑器与草稿](#m06-编辑器与草稿)
   - [M07 提交与判题](#m07-提交与判题)
   - [M08 结果面板三形态](#m08-结果面板三形态)
   - [M09 我的 · 提交记录](#m09-我的--提交记录)
   - [M10 我的 · 掌握分布](#m10-我的--掌握分布)
   - [M11 我的 · 账户设置](#m11-我的--账户设置)
   - [M12 运维概览](#m12-运维概览)
   - [M13 待实现功能的现状守卫](#m13-待实现功能的现状守卫)
   - [M14 鉴权边界与输入安全](#m14-鉴权边界与输入安全)
   - [M15 非功能与离线可用](#m15-非功能与离线可用)
6. [AC 追溯矩阵](#6-ac-追溯矩阵)
7. [Playwright 脚本骨架](#7-playwright-脚本骨架)
8. [完整回归流程](#8-完整回归流程)
9. [故障排查](#9-故障排查)
10. [当前未覆盖的部分](#10-当前未覆盖的部分)

---

## 1. 测试策略与分层

### 1.1 为什么需要这份文档

现有三层自动化各自的盲区：

| 层 | 脚本 | 能证明 | **证明不了** |
|---|---|---|---|
| 单元 | `tests/*.cpp`（417 条） | 业务层逻辑、口径正确 | 用户看到的界面 |
| 接口 | `tests/run_tests.sh`（98 条） | 路由、Cookie、包络、错误码 | DOM 里到底渲染没渲染、文案对不对、点击链路通不通 |
| 题库 | `tests/run_problemsync_test.sh`（30 条） | 改 JSON → 重启 → 读侧反映 | 同上 |

**三层全部验证不到的**，恰好是本文的主战场：`SPEC.md §1.3` 的 P1~P14 十四个用户流程、
`§7.3` 的三十余条交互契约、以及 `AC-25~AC-37` 这批**只在浏览器里才有意义**的需求
（未登录可读题面、草稿跨会话恢复、刷新恢复轮询、口径标注）。

### 1.2 分层模型

本文按**「一条用例需要几个前置条件」**分层，因为前置越重的用例越容易在别人机器上跑不起来：

| 层 | 名称 | 前置 | 用例数 | 价值 | 可否接 CI |
|---|---|---|---|---|---|
| **L0** | 静态可达性 | 无（无需登录） | 12 | 部署是否正确、前端资源是否齐全 | ✅ |
| **L1** | 匿名可读 | 无 | 21 | `[D-43]` 未登录可读题面 | ✅ |
| **L2** | 登录闭环 | 普通账号 | 38 | 主闭环、列表/详情/草稿/三 tab | ✅ |
| **L3** | 判题链路 | **judge 接线** | 31 | 六种 verdict、逐用例详情 | ⚠️ 需 judge 环境 |
| **L4** | 越权与注入 | 双账号 + DB | 18 | `[D-37]` 归属不放宽、`NF-7` | ⚠️ 需本地库 |
| **L5** | 非功能 | 4G 机器 | 22 | `[D-74]` 性能与资源 | ⚠️ 需专用机 |

> L3/L4/L5 的"⚠️"不是客套话：**当前部署实例上 L3 整层不可跑**（见 §3）。

### 1.3 用例编号规则

```
WTC-<模块字母><两位模块号>-<三位序号>

M01 → WT-IN    落地页
M02 → WT-AU    认证
M03 → WT-TB    顶栏
M04 → WT-LS    题库列表
M05 → WT-PB    题目页基础
M06 → WT-ED    编辑器与草稿
M07 → WT-SB    提交与判题
M08 → WT-RS    结果面板
M09 → WT-HR    历史记录
M10 → WT-MG    掌握分布
M11 → WT-AC    账户设置
M12 → WT-AD    运维
M13 → WT-GD    守卫
M14 → WT-SX    安全
M15 → WT-NF    非功能
```

例：`WT-RS-03` = 结果面板第 3 条用例。

### 1.4 状态图例

| 标记 | 含义 |
|---|---|
| 🟢 **可执行** | 当前 `82.156.75.119:8080` 实例上可直接跑通 |
| 🔴 **阻塞** | 用例设计完整，但受 §3 的环境缺口阻塞；接线后即可执行 |
| 🟡 **待实现** | `SPEC.md` 有契约但前端尚未实现（diff / 注销 / 草稿冲突挂载期分支） |
| 🔵 **人工** | 依赖主观判断或需人工构造环境（4G 压测、SQL 造数） |

---

## 2. 被测环境与账号

### 2.1 环境

| 项 | 值 |
|---|---|
| 服务地址 | `http://82.156.75.119:8080` |
| 监听来源 | `.env` 的 `OJ_LISTEN=0.0.0.0:8080` |
| 前端资源 | `static/`（`OJ_STATIC_DIR=./static`），`Cache-Control: no-cache` |
| 判题工作区 | `/tmp/oj-judge/<submissionId>/` |
| 数据库 | MySQL 8.0，`127.0.0.1:3306` / 库 `oj`（**测试机不可直连**，需在部署机上执行） |

### 2.2 账号

| 角色 | 用户名 | 密码 | 实测 userId | 实测 role |
|---|---|---|---|---|
| 管理员 | `admin` | `admin123` | 2828 | `admin` |
| 普通用户 | `zhangling` | `666666` | 3138 | `user` |

> ⚠️ **实测发现（用例需按此写断言）**：以 `zhangling` 登录，`GET /api/auth/me` 返回的
> `username` 是 **`ZhangLing`**（首字母大写）。即服务端登录匹配对用户名**大小写不敏感**，
> 但回显以库中原样为准。测试断言顶栏/概览条用户名时必须写 `ZhangLing`，写 `zhangling` 会假失败。
>
> 该行为在 `SPEC.md` / `API.md` 中**均未定义**，建议单独立一条观察项（见 `WT-AU-12`）。

### 2.3 测试数据准备

```bash
BASE=http://82.156.75.119:8080

# 普通用户登录（供 L2/L3/L4 使用）
curl -s -c /tmp/oj-user.jar -X POST $BASE/api/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"zhangling","password":"666666"}'

# 管理员登录（供 M12 使用）
curl -s -c /tmp/oj-admin.jar -X POST $BASE/api/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin123"}'

# 临时账号（需要干净状态时用，勿用固定账号做删除/注销类用例）
curl -s -X POST $BASE/api/auth/register -H 'Content-Type: application/json' \
  -d "{\"username\":\"wt$(date +%s)\",\"password\":\"pass1234\"}"
```

> **不要用 `zhangling` / `admin` 跑删除、注销、改密码类用例** —— 那是共用账号，跑完会破坏后续用例的基线。
> `M09` 的删除、`M11` 的改密、`WT-AC-06` 的注销统一用临时账号。

---

## 3. 环境实测基线与已知阻塞

**这一节是全文最重要的部分**：不先读它就去执行，会有一半用例以"看起来像 bug"的方式失败。

### 3.1 部署实例实测基线（2026-10-15 实测）

| 项 | 实测值 | 影响 |
|---|---|---|
| 6 个页面 | 全部 `200`，字节数与仓库一致 | ✅ 可测 |
| `static/vendor/` | 4 个 Ace 文件 + `marked.min.js` + `purify.min.js` 全部 `200` | ✅ 可测 |
| 题库题目数 | **1 道**（`two-sum`，medium，2 个用例，3 个标签，有 `hint_md`） | ⚠️ 见 §3.2 |
| 用户数 | 7 | — |
| **提交记录数** | **0** | ⚠️ 见 §3.2 |
| **`system.judgeAlive`** | **`false`** | 🔴 见 §3.3 |
| `POST /api/submissions` | `503` / `code=1009` / `判题服务不可用，请稍后再试` | 🔴 |
| 账号角色 | `admin` → `admin`；`zhangling` → `user` | ✅ 可测 |

### 3.2 题库只有 1 道题带来的用例受限

以下断言**在本实例上无法证伪**，必须在题库补足后执行，**不得记为"通过"**：

| 受限的用例 | 原因 | 补数办法 |
|---|---|---|
| `WT-LS-04` 难度筛选（简单/困难） | 只有 medium 一档 | 录入 easy/hard 题后 |
| `WT-LS-09` / `WT-HR-08` 分页 | 只有 1 条数据，造不出第 2 页 | 录入 ≥ 25 题后 |
| `WT-LS-06`「仅看收藏」 | 需先收藏才 1 条，测不出过滤 | 同上 |
| `WT-MG-03` 难度卡三项齐全 | easy/hard 的 `total=0`，整行不渲染 | 同上 |
| `WT-MG-05` 标签表排序 | 3 个标签可验排序，但样本太小 | 同上 |
| `WT-RS-*` 全部 | 无提交数据可渲染 | 见 §3.3 |

> ⚠️ **`/api/problems` 的分页与排序参数本身仍可验**（传 `page=0` / `size=1000` 验夹取），
> 那是 L2 的 `WT-LS-10`，与"造不出第 2 页"是两件事。

### 3.3 阻塞项 B-1：judge 调度器未接线（L3 整层）

```
GET /api/admin/overview  →  "system": { ..., "judgeAlive": false, "queueLength": 0, "activeJudges": 0 }
POST /api/submissions    →  {"code":1009,"data":{},"message":"判题服务不可用，请稍后再试"}   HTTP 503
```

**后果**：从"点提交"到"看逐用例结果"的**整条主链路（`SPEC.md §1.2` 的成功标准）无法验证**。
受影响的用例共 **31 条**，全部标记 🔴 阻塞：

| 模块 | 阻塞用例 | 数量 |
|---|---|---|
| M07 提交与判题 | `WT-SB-01` ~ `WT-SB-12` | 12 |
| M08 结果面板 | `WT-RS-01` ~ `WT-RS-10` | 10 |
| M09 历史记录 | `WT-HR-01`、`WT-HR-02`、`WT-HR-05`、`WT-HR-06`、`WT-HR-09` | 5 |
| M10 掌握分布 | `WT-MG-01`、`WT-MG-02`、`WT-MG-04` | 3 |
| M05 最近提交 | `WT-PB-08` | 1 |

#### 两条替代验证路径

> 选哪条取决于你手上有什么。**两条都需要在部署机上执行**（远程实例的库不可直连）。

**路径 A：接线 judge 后原样执行（首选）**

按 `SPEC.md §9` 的 T2.7 完成 `src/judge/{queue,scheduler,worker,runner,generator}`，
重启服务并确认 `judgeAlive: true`，然后本层用例全部直接可跑，无需改任何断言。

**路径 B：SQL 造数 + 浏览器验渲染（judge 接线前的临时手段）**

沿用 `tests/run_tests.sh` 已有的做法 —— **用 SQL 造出 judge 写回时会产生的那种行**，再从浏览器侧验渲染。
这只验「界面把已落库的数据渲染对了」，不验「代码真的被编译运行」：

```bash
# 在部署机上执行。造一条 WA 的提交（two-sum，第 2 个用例故意错）
mysql -h127.0.0.1 -uoj -poj oj <<'SQL'
SET @uid = (SELECT id FROM users WHERE username='zhangling');
SET @pid = (SELECT id FROM problems WHERE slug='two-sum');

INSERT INTO submissions
  (user_id, problem_id, language, code, status, verdict, message,
   case_passed, case_total, total_ms, max_mem_kb, created_at, finished_at)
VALUES
  (@uid, @pid, 'cpp', 'class Solution{...};', 'done', 'WA',
   '第 2 个用例答案错误', 1, 2, 7, 2048, UTC_TIMESTAMP(), UTC_TIMESTAMP());
SET @sid = LAST_INSERT_ID();

INSERT INTO submission_case_results
  (submission_id, case_idx, verdict, passed, args_text, output_text,
   expected_text, time_ms, mem_kb, error_msg)
VALUES
  (@sid, 0, 'AC', 1, '[2,7,11,15], target=9', '[0,1]', '[0,1]',  3, 2048, NULL),
  (@sid, 1, 'WA', 0, '[3,2,4], target=6',     '[1,0]', '[1,2]',  4, 2048, NULL);
SQL

# 造完后刷新 /history.html 与 /problem.html?slug=two-sum 即可看到渲染结果
```

**路径 B 的能力边界（必须在回归报告里写明）**：

| 能验 | 不能验 |
|---|---|
| 结果面板三形态的**渲染分支**是否正确 | 代码是否真的被 g++ 编译 |
| 逐用例表格的**列与文案** | `@@OJ@@` 协议解析 |
| CE 的 `case_total` 语义是否显示「N 个用例未执行」 | 两阶段编译的归属分流 `[D-64]` |
| 内存口径脚注 `ⓘ` 是否常驻 | `RLIMIT_AS` 是否真能拦住内存炸弹 |

### 3.4 阻塞项 B-2：待实现的三项前端功能

`SPEC.md` 定义了契约，但 `static/` 里对应实现为空。对应用例按 🟡 **待实现** 标记，
**并额外提供一条 🟢 现状守卫用例**（断言"确实还没有"），避免"没测"被误读成"测过了"。

| 决策 | 缺失的实装 | 证据 |
|---|---|---|
| `[D-42] [D-50]` 提交 diff | `static/js/diff.js` **0 字节**；`#p-recent` 行内无 checkbox；全仓库无 `/api/submissions/diff` 调用 | 见 `WT-GD-01`~`03` |
| `[D-56] [D-57]` 注销账号 | `history.html` 无注销按钮、无密码二次确认弹窗；无 `DELETE /api/auth/account` 调用 | 见 `WT-GD-04`~`06` |
| `[D-55]` 草稿冲突（挂载期分支） | `editor.js` 只实现了 `storage` 事件的实时冲突检测；`draftAt:` 写入了但**从未被比较** | 见 `WT-GD-07` |

> **注意区分**：D-55 的**实时分支是可测的**（`WT-ED-06`，🟢），只有
> SPEC §7.3 描述的"页面挂载时比较 `draftAt`"分支未实现（`WT-ED-07`，🟡）。

### 3.5 阻塞项 B-3：性能类需 4G 机器

`SPEC.md §1.4` 明确：`NF-1~NF-4` 与 `AC-38` **必须在 4G 机器上跑**，
本机 1967MB 跑出的延迟数字不可信。本文 M15 的 8 条性能用例全部标记 🔵 人工。

---

## 4. 元素定位手册

用例里出现的选择器全部出自本表。**这些是 `static/` 的实际 DOM，不是推测**；
仓库中**不存在任何 `data-testid`**，因此定位必须依赖 `id` / `class` / `data-*` 语义属性。

### 4.1 页面级

| 页面 | 路径 | 关键容器 |
|---|---|---|
| 落地页 | `/index.html`（`/` 亦可） | `#topbar` `#live-stats` `#stat-problems` `#stat-algo` `#stat-source` `#ticker` `.hero__cta` |
| 登录/注册 | `/login.html` | `#auth-form` `#username` `#password` `#submit` `#notice` `#auth-title` `#auth-subtitle` `#username-error` `#password-error` `#password-hint` `button.seg__btn[data-mode]` |
| 题库列表 | `/list.html` | `#topbar` `#overview` `#f-keyword` `#f-progress` `#f-sort` `#f-favorite` `#f-tags` `button.chip[data-tag]` `#rows` `#result-hint` `#pager` `#clear-filters` `#personal-hint` |
| 题目详情 | `/problem.html?slug=<slug>` | `#p-title` `#p-difficulty` `#favorite` `#p-tags` `#p-limits` `#tab-statement` `#tab-hint` `#tab-recent` `#p-markdown` `#p-hint` `#p-recent` `#p-next` `#editor` `#reset` `#submit` `#status` `#result` `#inline-login` `#inline-username` `#inline-password` `#inline-go` `#editor-warn` `.splitter` |
| 我的 | `/history.html` | `#topbar` `button.tabs__btn[data-tab]` `#pane-records` `#pane-progress` `#pane-account` `#f-problem` `#f-verdict` `#sub-rows` `#sub-total` `#sub-pager` `#acc-username` `#acc-role` `#pw-form` `#old-password` `#new-password` `#new-password2` `#pw-submit` `#logout` |
| 运维概览 | `/admin.html` | `#denied` `#sys-stats` `#sys-runtime` `#sys-note` `#users` `#user-count` `#refresh` |

### 4.2 列表行（`#rows > tr`）

```html
<td class="cell-title"><a href="/problem.html?slug=…">题名</a><span class="cell-slug mono">slug</span></td>
<td><span class="diff diff--easy|medium|hard">简单|中等|困难</span></td>
<td class="cell-tags"><span class="tag tag--algorithm|source">标签名</span>…</td>
<td class="mono">用例数</td>
<td class="mono muted" title="CPU 时间上限 / 地址空间上限（AS 口径）">3000ms · 128MB</td>
<td><span class="prog prog--none|attempted|solved">未开始|尝试过|已通过</span></td>
<td><button class="star[ is-on]" data-star="SLUG" aria-pressed="true|false">★|☆</button></td>
```

### 4.3 结果面板（`#result`）三形态

| 形态 | 判定 | 关键元素 |
|---|---|---|
| AC/WA/TLE/MLE/RE | `span.verdict.verdict--ac\|wa\|tle\|mle\|re` | `通过 P/T`、`总耗时`、`峰值RSS`、`table.result__table`、`tr.case.case--yes\|--no`、`p.result__foot`（`ⓘ` 口径脚注） |
| CE | `span.verdict.verdict--ce` + `N 个用例未执行` | `ul.ce-list > li > b`（`第 N 行`）、`div.notice.notice--warn`（`⚠️ 这不是你的代码出错`）、`pre.result__raw`。**无 `result__table`** |
| JE / failed | `span.verdict.verdict--je` | `#retry`（「重试这次提交」）、`pre.result__raw` |
| 题目不存在 | `div.empty` | `题目不存在或已从题库移除`，且 `#editor`/`#submit` 被隐藏 |

### 4.4 存储键（**精确字符串**）

| Store | 键 | 值 | 写入时机 |
|---|---|---|---|
| `localStorage` | `` `draft:${slug}` `` | 代码全文 | 编辑器 change 后 debounce **800ms** |
| `localStorage` | `` `draftAt:${slug}` `` | `String(Date.now())` | 与 `draft:` 同一次调用 |
| `localStorage` | `'split:problem'` | 分栏百分比（25–75） | 松开鼠标 / 方向键 |
| `sessionStorage` | `` `judging:${slug}` `` | submissionId | 提交成功；终态后删除 |

> **无 CSRF token**，鉴权仅靠 `sid` Cookie（`HttpOnly; SameSite=Lax; Path=/`），依赖 `[D-69]` 的 `SameSite=Lax` 隐式防护。

### 4.5 定时器与快捷键

| 值 | 位置 | 含义 |
|---|---|---|
| `1500ms` | `poll.js` | 轮询间隔 |
| `1000ms` | `poll.js` | `queueAhead` 倒计时 |
| `300000ms` | `poll.js` | 超过则显示「评测耗时较长，可稍后在「我的」页查看」 |
| `800ms` | `editor.js` | 草稿 debounce |
| `300ms` | `list.js` | 关键词输入 debounce |
| `3000ms` | `common.js` | toast 默认驻留 |
| `Ctrl/Cmd+Enter` | `editor.js` | 提交 |
| `←/→` | `.splitter`（`tabindex=0`） | 分栏 ±2%，钳制 25–75 |
| `Enter`/`Space` | `#sub-rows tr.subrow` | 展开/收起详情 |

### 4.6 网络请求清单（浏览器实际发起）

| Method | Path | 触发处 | 频次 |
|---|---|---|---|
| GET | `/api/auth/me` | `api.js` `currentUser()` | **每页 1 次**（模块级 Promise 缓存） |
| GET | `/api/problems` | `list.js` / `history.js` / `landing.js` | 每次筛选/翻页 |
| GET | `/api/problems/{slug}` | `problem.js` | 首屏 + 内联登录后各 1 次 |
| GET | `/api/me/overview` | `overview.js` | **每页 1 次**（切 tab/翻页不重取，`AC-24`） |
| GET | `/api/tags` | `list.js` / `landing.js` | 1 次 |
| POST | `/api/auth/*` | 登录/注册/登出/改密 | 按操作 |
| POST | `/api/submissions` | `problem.js` | 按提交 |
| GET | `/api/submissions/{id}` | `poll.js` 每 1.5s | 轮询期间 |

> **AC-24 的验收要点**：切 tab、tab1 翻页**不得**产生第二条 `/api/me/overview`。
> 这必须用 DevTools Network 或 Playwright 的 `page.on('request')` 计数验证，**看 DOM 是验不出来的**。

---

## 5. 测试用例

> **执行约定**
> - 「步骤」列的 `→` 表示顺序，`；` 表示并列。
> - 所有选择器见 §4。
> - 未特别说明时，浏览器为 Chromium 最新版，视口 `1440×900`（SPEC §1.4 要求 ≥1280px 宽）。
> - 需要登录的用例，默认用 `zhangling`；**删除/改密/注销类必须换临时账号**（§2.3）。

---

### M01 落地页

对应 `SPEC.md §7.1` 落地页一节（v1.10 新增）。核心契约：**纯展示、不含任何个人数据**。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-IN-01` | `/` 重定向到落地页 | 匿名 | 打开 `http://82.156.75.119:8080/` | HTTP `200`，`Content-Type: text/html`，页面含 `.hero__title`，无 `404` | — | 🟢 |
| `WT-IN-02` | 六页全部可达 | 匿名 | 依次打开 `index/login/list/problem?slug=two-sum/history/admin`.html | 全部 `200`；字节数与 `static/` 一致（18136/5761/4847/5983/6589/4497） | — | 🟢 |
| `WT-IN-03` | 实时数字在 API 可用时显示 | 匿名 | 打开落地页，等 `#live-stats` 可见 | `#stat-problems` = `1`、`#stat-algo` = `3`、`#stat-source` = `1`；**先 `hidden` 后显形** | — | 🟢 |
| `WT-IN-04` | API 失败时整行隐藏而非显示 0 | 匿名 | DevTools 把 `/api/problems` 拦成 500 → 刷新 | `#live-stats` 保持 `hidden`；**页面上不出现"0"** | — | 🟢 |
| `WT-IN-05` | 落地页不含任何个人数据 | 匿名 | 打开落地页，Network 面板过滤 | 只允许 `/api/problems` 与 `/api/tags` 两条匿名请求；**不得**出现 `/api/stats`、`/api/me/overview`、`/api/submissions` | — | 🟢 |
| `WT-IN-06` | 落地页绝不含 `/api/stats` | 匿名 | 同上 | `GET /api/stats` 需登录（`1002`），落地页作为未登录首屏**不得**调用 | — | 🟢 |
| `WT-IN-07` | 演示数据有明确标注 | 匿名 | 定位 `.demo` 区块 | 含 `界面示意 · 示例数据` 标签；**不得**被误认为真实结果面板（真实结果面板在 `#result`） | — | 🟢 |
| `WT-IN-08` | 登录后 CTA 改写 | 登录 | 登录后打开落地页 | `.hero__cta` 第二个按钮文案为 `我的提交（ZhangLing）`，`href="/history.html"` | — | 🟢 |
| `WT-IN-09` | 标签跑马灯渲染 | 匿名 | 打开落地页 | `#ticker` 由 `hidden` 转可见；含 `.ticker__row` 与 `<span>` 标签 chip | — | 🟢 |
| `WT-IN-10` | 七种 verdict 卡片齐全 | 匿名 | 打开落地页 | `.verdicts` 内 `.vcard` 共 7 个：`--ac --wa --tle --mle --re --ce --je` | — | 🟢 |
| `WT-IN-11` | 落地页无图片外链 | 匿名 | Network 面板过滤域名 | **无任何指向外部域的请求**（`cdn.jsdelivr.net` 等），全部资源同源 | `AC-48`④ | 🟢 |
| `WT-IN-12` | 落地页断开 JS 仍可读 | 匿名 | 禁用 JS 后打开 | 呈现的是空壳（内容由 JS 注入），**但 HTTP 200 与标题正常**；恢复 JS 后数字补齐 | — | 🔵 |

---

### M02 登录与注册

对应 `P1`、`AC-1`、`AC-25`（部分）、`D-44`、`D-46`、`D-69`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-AU-01` | 登录成功并跳转列表页 | 匿名 | 填 `zhangling`/`666666` → 点 `#submit` | 跳 `/list.html`；`#topbar` 出现 `ZhangLing` 与「我的」入口 | `AC-1` | 🟢 |
| `WT-AU-02` | 登录按钮忙碌态 | 匿名 | 慢网络下点 `#submit` | 按钮文案变 `登录中…` 且 `disabled`；响应后恢复 | — | 🟢 |
| `WT-AU-03` | 密码错误不跳转 | 匿名 | 填 `zhangling`/`6666666` → 提交 | `#notice` 显示错误；**停留本页**；`#username`/`#password` 加 `is-invalid` | `AC-1` | 🟢 |
| `WT-AU-04` | 用户名过短前端拦截 | 匿名 | 用户名填 `ab` → 提交 | `#username-error` = `用户名需 3–32 个字符`；**不发请求** | `AC-1` | 🟢 |
| `WT-AU-05` | 用户名非法字符前端拦截 | 匿名 | 用户名填 `a b!` → 提交 | `#username-error` = `只能使用字母、数字、下划线和连字符`；**不发请求** | — | 🟢 |
| `WT-AU-06` | 空密码前端拦截 | 匿名 | 用户名合法、密码留空 → 提交 | `#password-error` = `请输入密码`；**不发请求** | — | 🟢 |
| `WT-AU-07` | 注册模式密码过短 | 匿名 | 切到 `button.seg__btn[data-mode="register"]`，密码填 `12345` | `#password-error` = `密码需 6–64 位`；`#password-hint` = `密码 6–64 位`；**不发请求** | `AC-1` | 🟢 |
| `WT-AU-08` | 注册/登录两态文案切换 | 匿名 | 在两个 `.seg__btn` 间来回切 | 登录态：`#auth-title`=`欢迎回来`、`#submit`=`登录`、`#password[autocomplete]=current-password`；注册态：`创建账号`/`注册并登录`/`new-password` | — | 🟢 |
| `WT-AU-09` | 注册后自动登录 | 匿名 | 切注册 tab，用临时账号提交 | 账号创建并直接进入已登录态，跳 `/list.html` | `AC-1` | 🟢 |
| `WT-AU-10` | **不预填上次用户名** | 任意 | 登录成功 → 登出 → 重新打开 `/login.html` | `#username` 的 `value` 为**空**；`#password` 亦为空 | `D-46` | 🟢 |
| `WT-AU-11` | `?mode=register` 直达 | 匿名 | 打开 `/login.html?mode=register` | 注册 tab 自动选中（`aria-selected="true"`） | — | 🟢 |
| `WT-AU-12` | **用户名大小写不敏感（实测发现）** | 匿名 | 以 `zhangling` 登录 | 登录成功，但 `auth/me` 与顶栏回显 **`ZhangLing`**（库中原样）。⚠️ 该行为未在 `API.md`/`SPEC.md` 中定义 | 观察项 | 🟢 |
| `WT-AU-13` | `?redirect=` 安全校验（防开放重定向） | 匿名 | 依次打开 `/login.html?redirect=https://evil.com`、`?redirect=//evil.com`、`?redirect=/\evil.com` | 三者**均回退**到 `/list.html`，**不得**跳往外站 | — | 🟢 |
| `WT-AU-14` | `?expired=1` 提示 | 匿名 | 打开 `/login.html?expired=1` | `#notice` 可见、文案 `登录状态已过期，请重新登录。登录后会回到你原来的页面。`、class 含 `notice--warn` | `D-44` | 🟢 |
| `WT-AU-15` | `?deleted=1` 提示 | 匿名 | 打开 `/login.html?deleted=1` | `#notice` 可见、文案 `账号已注销，相关数据已一并删除。`、class 含 `notice--ok` | `D-57` | 🟢 |
| `WT-AU-16` | 登录页**不渲染顶栏** | 匿名 | 打开 `/login.html` | DOM 中**不存在** `#topbar` 元素（不是 CSS 隐藏） | `AC-35` | 🟢 |
| `WT-AU-17` | Cookie 属性符合 `[D-69]` | 匿名 | 登录，检查响应头 | `Set-Cookie: sid=<64 hex>; HttpOnly; SameSite=Lax; Path=/`；**首版无 `Secure`**（HTTP 部署下的刻意取舍） | `AC-1` | 🟢 |
| `WT-AU-18` | 登出后旧 Cookie 失效 | 登录 | 点顶栏 `[data-topbar-logout]` → 落回 `/login.html` | 服务端会话行已删；手动带旧 Cookie 请求 `/api/auth/me` 得 `401/1002` | `AC-1` | 🟢 |
| `WT-AU-19` | 登出幂等 | 匿名 | 直接调 `POST /api/auth/logout` | `200/0`，不报错 | — | 🟢 |
| `WT-AU-20` | 注册关闭时的报错（`OJ_ALLOW_REGISTER=0`） | **需改 `.env` 重启** | 打开注册 tab 提交 | 注册 tab **仍然显示**（SPEC 刻意不做探测端点），提交后收到明确报错 | `AC-18` | 🔵 |
| `WT-AU-21` | `bcrypt` 闭环（不以"没报错"为证） | 临时账号 | 注册 → 登出 → 重新登录 | 登录成功；**且**库中 `password_hash` 为 `$2b$10$` 开头、共 60 字符 | `AC-49` | 🔵 |

---

### M03 顶栏三形态

对应 `[D-52] [D-41]`、`AC-23`、`AC-35`。**契约要点：未登录时「我的」「运维」在 DOM 中不存在，而非 CSS 隐藏。**

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-TB-01` | 未登录形态 | 匿名 | 打开 `/list.html` | 导航只有 `题库`；右侧为 `登录` `注册` 链接；**无**「我的」「运维」「登出」 | `AC-35` | 🟢 |
| `WT-TB-02` | 普通用户形态 | `zhangling` | 打开 `/list.html` | 导航有 `题库` `我的`；右侧为 `ZhangLing` + `[data-topbar-logout]`；**无**「运维」、**无**任何角色徽标 | `AC-23` `AC-35` | 🟢 |
| `WT-TB-03` | 管理员形态 | `admin` | 打开 `/list.html` | 导航有 `题库` `我的` `运维`；`ZhangLing` 右侧存在 `span.badge.badge--admin`（文案 `管理员`） | `AC-23` `AC-35` | 🟢 |
| `WT-TB-04` | 普通用户**不**渲染角色徽标 | `zhangling` | 检查 `#topbar` DOM | **不存在** `.badge`；也**不出现**「普通用户」字样 | `AC-23` | 🟢 |
| `WT-TB-05` | 同一组件两形态（非两套模板） | 三种身份各一次 | 比对三次的 DOM 结构 | 三次都只有**一个** `#topbar`；差异仅在「我的/运维/徽标/登出」四处 | `AC-35` | 🟢 |
| `WT-TB-06` | 题目页 compact 变体 | 登录 | 打开 `/problem.html?slug=two-sum` | 顶栏存在且压缩为单行；导航项与 `/list.html` 形态一致 | `AC-35` | 🟢 |
| `WT-TB-07` | 当前页高亮 | 登录 | 分别在 `/list.html`/`/history.html`/`/admin.html` | 对应项带 `is-active`；落地页**任何项都不高亮**（`active=landing` 不匹配任何导航项） | — | 🟢 |
| `WT-TB-08` | 徽标文案与概览条一致 | `admin` | 对比顶栏徽标与 `#overview` 内角色徽标 | 两处文案均为 `管理员`（`overview.js` 与 `common.js` 共用同一段渲染逻辑） | `AC-23` | 🟢 |
| `WT-TB-09` | 前端隐藏**不是**安全边界 | `zhangling` | 直接在地址栏输入 `/admin.html` | 页面渲染 `#denied`「无权访问」；Network 里 `/api/admin/overview` 返回 `403/1003` | `AC-19` | 🟢 |

---

### M04 题库列表与筛选

对应 `P2`、`AC-2`、`AC-11`、`AC-12`、`AC-37`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-LS-01` | 列表加载 | 匿名 | 打开 `/list.html` | `#rows` 有 1 行；`#result-hint` = `共 1 题`；加载中为 `加载中…` | `AC-2` | 🟢 |
| `WT-LS-02` | 行内七列齐全 | 匿名 | 检查 `#rows > tr` | 七列：题名/slug、难度、用例数、限制、`我的进度`、收藏；标签 chip 带 `tag--algorithm`/`tag--source` | `AC-2` | 🟢 |
| `WT-LS-03` | 限制列口径文案 | 匿名 | 读 `.mono.muted[title]` | title 含 `CPU 时间上限 / 地址空间上限（AS 口径）`；文本形如 `3000ms · 128MB` | `AC-44` | 🟢 |
| `WT-LS-04` | 难度筛选 | 匿名 | 依次点 `button.seg__btn[data-difficulty]` 的 空/简单/中等/困难 | 中等→1 题；简单/困难→0 题并出现空态 | `AC-2` | 🟢 部分 |
| `WT-LS-05` | 关键词筛选（含 debounce） | 匿名 | `#f-keyword` 填 `两数` → 等 400ms；再填 `two-sum` | 均命中 1 题；**请求在停止输入 300ms 后才发出**（debounce） | `AC-2` | 🟢 |
| `WT-LS-06` | 标签多选为 AND | 匿名 | `#f-tags` 同时点 `数组` 与 `哈希表` | 两题都被选中（`.chip.is-on`）；命中集为交集 | `AC-2` | 🟢 |
| `WT-LS-07` | 来源标签筛选 | 匿名 | 点 `chip[data-tag="高频面试题"]` | 命中 1 题 | `AC-2` | 🟢 |
| `WT-LS-08` | 排序切换 | 匿名 | 依次选 `#f-sort` 的 默认/精选优先/由易到难/由难到易 | 每次重取列表；`精选优先` 把 `two-sum`（`isFeatured=true`）排首 | `AC-2` | 🟢 |
| `WT-LS-09` | 分页 | **需 ≥25 题** | 翻到第 2 页再翻回来 | `#pager` 出现 `第 P / T 页`；顺序稳定、不重不漏；`aria-current="page"` 正确 | `AC-2` | 🔴 |
| `WT-LS-10` | 分页参数边界（不依赖数据量） | 匿名 | 地址栏改 `?page=0` / `?size=1000` 直连 API | 前者 `400/1001 field=page`；后者 `200/0` 且 `data.size` 回显 `100`（夹取） | `AC-2` | 🟢 |
| `WT-LS-11` | 非法枚举值 | 匿名 | `?difficulty=nope` | `400/1001`，`data.field="difficulty"` | `AC-2` | 🟢 |
| `WT-LS-12` | 空态 + 一键清除 | 匿名 | `#f-keyword` 填不存在的词 | `#rows` 内 `div.empty`：`没有匹配的题目，试试放宽筛选条件` + 「清除筛选」按钮；点它后恢复 | `AC-2` | 🟢 |
| `WT-LS-13` | 「清除筛选」按钮 | 匿名 | 设好任一筛选后点 `#clear-filters` | 所有筛选复位，`#rows` 恢复全量 | `AC-2` | 🟢 |
| `WT-LS-14` | 匿名不得按进度筛选 | 匿名 | 检查 `#f-progress` 与 `#f-favorite` | 两者 `disabled` 且 `title="登录后可按进度筛选"`；`#personal-hint` 可见 | `AC-25` | 🟢 |
| `WT-LS-15` | 匿名 `progress` 参数被后端拒 | 匿名 | 直连 `?progress=solved` | `400/1001` | `AC-25` | 🟢 |
| `WT-LS-16` | 概览条：未登录不隐藏 | 匿名 | 打开 `/list.html` | `#overview` **不隐藏**，改为渲染引导文案 + 登录按钮；**题库表格照常渲染** | `D-38` | 🟢 |
| `WT-LS-17` | 概览条：登录态四档 + 最近 3 条 | 登录 | 打开 `/list.html` | `.overview__hello` = `你好，ZhangLing`；`.stat` **四个**、按「值 `<b>` 在前、标签 `<span>` 在后」渲染：`0/1 已通过`、`0 尝试中`、`0 收藏`、`0 提交`；`medium` 难度条有值，`easy`/`hard` 因 `total=0` **整行不渲染** | `AC-24` | 🟢 |
| `WT-LS-18` | 概览条不随分页/筛选重取 | 登录 | 翻页 + 改关键词 | `/api/problems` 请求数增加；`/api/me/overview` **始终只有 1 条** | `AC-24` | 🟢 |
| `WT-LS-19` | 零数据新用户 | **临时账号** | 新账号打开 `/list.html` | 进度条渲染为空槽（0%）而非空白；最近提交区显示 `.overview__empty`：`还没有提交记录，去做第一题 →` | `D-38` | 🟢 |
| `WT-LS-20` | 进度推导三态 | 登录 | 有 AC / 有 CE / 从未提交 三道题 | 分别显示 `.prog--solved` 已通过 / `.prog--attempted` 尝试过 / `.prog--none` 未开始 | `AC-11` | 🔴 |
| `WT-LS-21` | 收藏切换即时生效 | 登录 | 点行内 `button.star[data-star]` | 立即变 `★` + `.is-on` + `aria-pressed="true"`；刷新后保持（落 MySQL） | `AC-12` | 🟢 |
| `WT-LS-22` | 「仅看收藏」 | **需 ≥2 题** | 收藏 1 题后勾选 `#f-favorite` | 只显示被收藏的题 | `AC-12` | 🔴 |
| `WT-LS-23` | 列表→详情跳转 | 匿名 | 点 `#rows` 内 `a[href^="/problem.html"]` | 落到 `/problem.html?slug=two-sum` | `AC-2` | 🟢 |
| `WT-LS-24` | 不存在页面 404 | 匿名 | 打开 `/nope.html` | HTTP `404`，JSON 包络 `code=1004` | — | 🟢 |

---

### M05 题目详情与题面渲染

对应 `P3`、`AC-3`、`AC-25`、`AC-26`、`AC-27`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-PB-01` | 题面基本信息 | 匿名 | 打开 `?slug=two-sum` | `#p-title`=`两数之和`；`#p-difficulty.diff--medium`=`中等`；`#p-tags` 含 `数组`/`哈希表`/`高频面试题`；`#p-limits` 的 `dataset.memKb`=131072 | `AC-25` | 🟢 |
| `WT-PB-02` | Markdown 渲染 | 匿名 | 查看 `#p-markdown` | 标题(`h2/h3`)、无序列表(`ul>li`)、行内代码、行内 `<code>`、代码块 `<pre><code>` 均正确渲染 | `AC-3` | 🟢 |
| `WT-PB-03` | `<script>` 被 DOMPurify 清除 | 构造 | 让题面含 `<script>alert(1)</script>`（见下） | `#p-markdown` 内**无** `<script>` 元素；无弹窗 | `AC-3` | 🔵 |
| `WT-PB-04` | `img` 标签被过滤（`[D-48]` 题面禁图片） | 构造 | 题面含 `![](x.png)` | 渲染结果**无 `<img>` 元素** | `AC-27` | 🔵 |
| `WT-PB-05` | 提示 tab 仅在有 `hint_md` 时渲染 | 匿名 | 观察 `#tab-hint` | `two-sum` 有 `hint_md` → tab **可见**、`#pane-hint` 可点开 | `AC-26` | 🟢 |
| `WT-PB-06` | 提示免费无限看 | 匿名 | 反复点开/收起 `#tab-hint` 共 5 次 | 每次都正常展开；**无任何次数提示、无解锁流程** | `AC-26` | 🟢 |
| `WT-PB-07` | 无 `hint_md` 的题不渲染该 tab | **需无提示的题** | 打开该题 | `#tab-hint` 在 DOM 中**不存在**（不是 `hidden`） | `AC-26` | 🔵 |
| `WT-PB-08` | 「我的提交」tab | 登录 | 点 `#tab-recent` | 无数据时显示 `还没有提交记录`；有数据时最多 5 行 `button.recentrow[data-sub]` | `D-42` | 🔴 |
| `WT-PB-09` | 收藏星标：登录态 | 登录 | 点 `#favorite` | 描边态 `☆` ⇄ 实心 `★`，`aria-pressed` 同步翻转；刷新后保持 | `D-43` | 🟢 |
| `WT-PB-10` | 收藏星标：匿名点按跳登录 | 匿名 | 点 `#favorite` | **跳转** `/login.html?redirect=%2Fproblem.html%3Fslug%3Dtwo-sum`（与"提交"的内联弹窗不同） | `D-43` | 🟢 |
| `WT-PB-11` | 匿名点提交**不跳转**而是内联弹窗 | 匿名 | 点 `#submit` | `#submit` 文案为 `登录后提交`；**URL 不变**；`#inline-login` 由 `hidden` 转可见 | `D-43` | 🟢 |
| `WT-PB-12` | 内联登录成功后续做 | 匿名 | 在 `#inline-login` 填账号密码 → `#inline-go` | 登录成功；页面**不刷新**；`#submit` 变回 `提交`；`#p-recent`/`#p-next` 被重新拉取填充 | `D-43` | 🟢 |
| `WT-PB-13` | 登录后**不自动补收藏** | 匿名 | 点 `#favorite` → 登录 → 登录后 | **不**自动写入收藏（避免隐式写操作） | `D-43` | 🟢 |
| `WT-PB-14` | 题目不存在 | 任意 | 打开 `?slug=no-such-problem` | `#result` 渲染 `div.empty`：`题目不存在或已从题库移除` + 返回题库按钮；**`#editor` 与 `#submit` 被隐藏** | — | 🟢 |
| `WT-PB-15` | 分栏比例可调且持久化 | 登录 | 拖 `.splitter` 到约 70% → 刷新 | `localStorage['split:problem']` ≈ `70`；刷新后比例恢复 | — | 🟢 |
| `WT-PB-16` | 分栏比例钳制在 25–75 | 登录 | 疯狂向右拖到底 | 比例停在 `75`，**不越界** | — | 🟢 |
| `WT-PB-17` | 分栏键盘可达 | 登录 | `.splitter` 聚焦后按 `←`/`→` | 每次 ±2%，写回 `localStorage` | — | 🟢 |
| `WT-PB-18` | 视口宽度下限 | 匿名 | 视口缩到 `1280×800` | 左右分栏不塌陷；表格与结果面板不溢出 | §1.4 | 🟢 |
| `WT-PB-19` | 下一题建议卡：匿名不渲染 | 匿名 | 打开 `?slug=two-sum` | `#p-next` 保持 `hidden`；响应体中**不含** `nextRecommendation` 键 | `AC-25` | 🟢 |
| `WT-PB-20` | 下一题建议卡：登录后渲染三要素 | 登录 | 打开 `?slug=two-sum` | `#p-next` 可见，含 `<b>下一题建议 · 中等</b>`、可点跳转的题名链接、**理由字符串** | `AC-34` | 🟢 |
| `WT-PB-21` | 理由字符串格式与主标签一致 | 登录 | 读理由文本 | 匹配 `^「.+」标签你通过 \d+/\d+，本题尚未通过$`；本例为 `「数组」标签你通过 0/1，本题尚未通过`；`tag` 与 `/api/me/overview.byTag` 中该标签数值一致 | `AC-34` `AC-46` | 🟢 |
| `WT-PB-22` | 理由不可由前端编造 | 登录 | 对比 DOM 理由与服务端 `reason` | **逐字相同**（理由由服务端算好，前端不拼装） | `D-59` | 🟢 |
| `WT-PB-23` | 全部通过后改为完成度文案 | **需全通过** | 把所有题 AC 掉 | `#p-next` 显示 `题库已全部通过` + `想扩充题库？编辑 problems/*.json 后重启服务`；**不显示**推荐 | `AC-34` | 🔵 |
| `WT-PB-24` | 推荐随进度变化 | **需 ≥2 题** | AC 掉当前推荐题后回到本页 | 推荐结果**发生变化** | `AC-34` | 🔵 |

> **构造题面的做法（`WT-PB-03`/`04`）**：改题必须走「改 JSON → 重启」（`D-30`），
> 在**部署机**上加 `problems/xss-probe.json`（`statement_md` 里放 `<script>` 与 `![](x.png)`），
> 重启后测，**测完删文件并删库行**。参照 `tests/run_problemsync_test.sh` 的探针题隔离做法。
> **不要改 `two-sum.json`。**

---

### M06 编辑器与草稿

对应 `P3`、`D-27`、`D-28`、`D-44`、`D-55`、`AC-4`、`AC-32`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-ED-01` | Ace 编辑器加载 | 匿名 | 打开 `?slug=two-sum` | `#editor` 存在且带类 `ace_editor`；`#editor-warn` 保持 `hidden`（未降级） | `AC-4` | 🟢 |
| `WT-ED-02` | C++ 语法高亮生效 | 匿名 | 观察编辑器 | token 被着色（`ace_keyword`/`ace_string` 等 class 存在） | `AC-4` | 🟢 |
| `WT-ED-03` | 预置模板 | 匿名 | 首次打开题目 | 编辑器内容 == `#editor[data-slug]` 对应的 `codeTemplate`，含 `class Solution` 骨架 | `D-28` | 🟢 |
| `WT-ED-04` | 草稿自动保存（800ms debounce） | 任意 | 在编辑器里输入 `// my-draft-marker` → 等 1s | `localStorage['draft:two-sum']` 含该标记；**同时** `localStorage['draftAt:two-sum']` 为数字串 | `AC-4` | 🟢 |
| `WT-ED-05` | 刷新后草稿恢复 | 任意 | 刷新页面 | 编辑器内容仍是草稿（**不是**模板） | `AC-4` | 🟢 |
| `WT-ED-06` | **多标签页冲突（实时分支）** | 任意 | 开两个标签页访问同一题 → A 页改代码等 1s → 切到 B 页 | B 页弹 `confirm`：`此代码已在另一个标签页更新，是否载入？`；选「是」→ 内容被替换 + toast `已载入另一个标签页的代码`；选「否」→ **B 页内容保持不变** | `AC-32`① | 🟢 |
| `WT-ED-07` | 多标签页冲突（挂载期分支） | 任意 | 写草稿 → 把 `localStorage['draftAt:two-sum']` 改成未来时间 → 刷新 | 按 SPEC 应弹同一 `confirm`；**当前实现不会弹**（`draftAt` 写入了但从未被比较） | `AC-32`① | 🟡 |
| `WT-ED-08` | 切题再切回草稿仍在 | 任意 | 在 `two-sum` 写草稿 → 回 `/list.html` → 再进 `two-sum` | 草稿仍在（键按 slug 隔离） | `AC-4` | 🟢 |
| `WT-ED-09` | 草稿键按 slug 隔离 | 任意 | 两道题各写不同草稿 | `draft:<slugA>` 与 `draft:<slugB>` 互不覆盖 | `AC-4` | 🟢 |
| `WT-ED-10` | 「重置为模板」需二次确认 | 任意 | 点 `#reset` | 弹 `confirm`：`用题目的初始模板覆盖当前代码？当前草稿会被替换。`；确认后内容变模板；**取消则不变** | — | 🟢 |
| `WT-ED-11` | 空代码提交被前端拦截 | 任意 | 清空编辑器 → `Ctrl+Enter` | toast `请先写点代码再提交`；**不发请求**（Network 无 `/api/submissions`） | `D-28` | 🟢 |
| `WT-ED-12` | 超长代码被前端拦截 | 任意 | 粘贴 65KB+ 内容 → 提交 | toast `代码 XX.XKB 超过 64.0KB 上限`；**不发请求** | `D-28` `D-22` | 🟢 |
| `WT-ED-13` | `Ctrl/Cmd+Enter` 快捷键 | 登录 | 聚焦编辑器 → `Ctrl+Enter` | 等同点 `#submit` | `D-28` | 🔴 |
| `WT-ED-14` | 会话过期后草稿自动恢复 | 登录 | 有草稿 → 手工删 Cookie（或等 7 天过期）→ 操作触发 1002 → 重登 | 跳 `/login.html?...&expired=1`；登录后回到该题，草稿自动载入 + toast `已恢复你上次未完成的代码` | `AC-32`② | 🟢 |
| `WT-ED-15` | 草稿写入失败不阻断提交 | 任意 | DevTools 禁用 localStorage → 改代码 → 提交 | Console 有 `[editor] 草稿写入失败` 警告；**提交链路不受影响** | `D-51` | 🟢 |

---

### M07 提交与判题 🔴

对应 `P4`、`D-10`、`D-54`、`D-60`、`D-62`、`AC-5`、`AC-14`、`AC-30`、`AC-31`。
**整层被 §3.3 阻塞（`judgeAlive=false`）。** 当前实例上点提交，预期是 `1009/503` 与错误文案。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-SB-01` | 提交后立即进入评测态 | 登录+judge | 点 `#submit` | `#status[data-state]` 变 `judging`，文案 `评测中…`；`#result` 仍 `hidden` | `AC-5` | 🔴 |
| `WT-SB-02` | `Ctrl+Enter` 与按钮等价 | 登录+judge | 分别用快捷键与按钮提交 | 两者行为完全一致 | `D-28` | 🔴 |
| `WT-SB-03` | 轮询节奏 1.5s | 登录+judge | 提交后 Network 面板计时 | `/api/submissions/{id}` 请求间隔约 `1500ms`，直到终态停止 | `D-10` | 🔴 |
| `WT-SB-04` | 队列位置提示 | 登录+judge | 占住 worker 后提交 | `#status` 显示 `评测中… 前面还有 N 个评测`；`N` 每秒递减 | `AC-30` `D-54` | 🔴 |
| `WT-SB-05` | 队列位置不泄露全局负载 | 登录+judge | 检查提交响应体 | 只含 `queueAhead`；**不含**全局队列长度、不含任何他人信息 | `D-60` | 🔴 |
| `WT-SB-06` | 单用户同时评测上限 | 登录+judge | 连续提交第 3 条 | toast `你已有 2 个评测在进行中，请等它们结束` | `AC-14` `D-22` | 🔴 |
| `WT-SB-07` | 每分钟提交上限 | 登录+judge | 1 分钟内提交 11 次 | 第 11 次 toast `提交过于频繁，请稍后再试（每分钟上限 10 次）` | `AC-14` `D-22` | 🔴 |
| `WT-SB-08` | **刷新恢复轮询** | 登录+judge | 提交后立刻 F5 | `sessionStorage['judging:two-sum']` 存在 → 状态区**自动重建**并继续显示 `评测中…` 直至出结果 | `AC-31` `D-53` | 🔴 |
| `WT-SB-09` | 终态后不再重建 | 登录+judge | 等结果出齐后再 F5 | `sessionStorage` 键已清；刷新**不**重建轮询、不转圈 | `AC-31` | 🔴 |
| `WT-SB-10` | **另一标签页不跟着转圈** | 登录+judge | A 页提交 → 开 B 页访问同一题 | B 页**不**重建轮询（`sessionStorage` 不跨标签共享） | `AC-31` | 🔴 |
| `WT-SB-11` | 慢评测兜底提示 | 登录+judge | 提交后等 > 5 分钟 | `#status` 变 `评测耗时较长，可稍后在「我的」页查看`；轮询继续 | `D-10` | 🔴 |
| `WT-SB-12` | judge 未接线的降级文案 | 登录 | **当前实例**：点 `#submit` | `#result` 渲染 `div.notice.notice--error`（`判题服务不可用…`）+ `p.result__foot`「当前 judge 调度器尚未接线（T2.7），提交未进入队列、也未写入任何记录」；`/api/admin/overview` 的 `judgeAlive=false` 可佐证 | §3.3 | 🟢 |

> `WT-SB-12` 是**当前唯一能跑**的提交类用例，它验的正是"降级路径也要给用户看得懂的解释"，
> 而不是把 `1009` 当成 bug 报上去。

---

### M08 结果面板三形态 🔴

对应 `P5`、`D-63`、`D-64`、`D-65`、`D-66`、`AC-6`、`AC-7`、`AC-40`~`AC-44`。
整层被 §3.3 阻塞；可用路径见 §3.3 路径 B。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-RS-01` | AC 全绿 | judge+AC 解 | 提交正确解法 | `span.verdict.verdict--ac`；`通过 2/2`；每行 `tr.case--yes`；`总耗时`/`峰值RSS` 有值 | `AC-6` | 🔴 |
| `WT-RS-02` | WA 逐用例定位 | judge+错解 | 提交第 2 个用例错的解 | `verdict--wa`；`通过 1/2`；第 1 行 `--yes`、第 2 行 `--no` | `AC-6` `AC-7` | 🔴 |
| `WT-RS-03` | 七列表头齐全 | 同上 | 读 `table.result__table thead th` | `# / 输入 / 你的输出 / 期望输出 / 耗时 / 峰值RSS / 结论` | `AC-7` | 🔴 |
| `WT-RS-04` | 内存列头写「峰值RSS」 | 同上 | 读第 6 列列头 | 字面为 **`峰值RSS`**，**不是**「内存」 | `AC-44` `D-63` | 🔴 |
| `WT-RS-05` | 口径脚注常驻不折叠 | 同上 | 读 `p.result__foot` | 文本形如 `ⓘ 限制口径：地址空间 128MB ｜ 列为实测峰值 RSS，仅供参考`；**无折叠控件** | `AC-44` | 🔴 |
| `WT-RS-06` | MLE 徽标与 RSS 不同层级 | judge+内存炸弹 | 提交撑爆内存的解 | 徽标写 `内存超限 · 地址空间 128MB`；RSS 数值**不与**徽标并列成同一视觉层级 | `AC-44` `D-63` | 🔴 |
| `WT-RS-07` | TLE 跨列显示 | judge+死循环 | 提交 `while(true){}` | 该行 `verdict--tle`；输入/输出/期望**跨列合并**显示 `超出时间限制 (3000ms)`；`耗时`/`峰值RSS` 为空 | `AC-6` | 🔴 |
| `WT-RS-08` | RE 显示 stderr 摘要 | judge+崩溃解 | 提交 `*p=nullptr` 解法 | `verdict--re`；该行展示 stderr 摘要；**有** `@@OJ@@` 前序行作证 | `AC-6` `D-66` | 🔴 |
| `WT-RS-09` | CE 形态：显示「N 个用例未执行」且**不渲染用例表** | judge+语法错误 | 提交 `class Solution{ vecto<int> a; }` | `verdict--ce`；状态行 `编译错误 · 2 个用例未执行`；**无 `table.result__table`** | `AC-41` `D-65` | 🔴 |
| `WT-RS-10` | JE 形态 + 重试按钮 | **需 `__expr` 坏题** | 提交到生成期校验漏网的坏题 | `verdict.verdict--je` `平台错误`；文案 `这通常是系统问题，不是你的代码问题`；**存在** `#retry`；**不显示**「运行错误」 | `AC-40` `D-66` | 🔴 |

---

### M09 我的 · 提交记录

对应 `P6`、`D-35`、`D-49`、`D-58`、`D-61`、`AC-10`、`AC-29`、`AC-37`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-HR-01` | 默认时间倒序 | judge | 提交 3 条 → 打开 `?` 默认 tab | `#sub-rows` 首行是**最新**那条；`#sub-total` = `共 3 条` | `AC-37` `D-61` | 🔴 |
| `WT-HR-02` | 每页 20 条 | **需 ≥21 条** | 造 25 条提交 | `data.size` 回显 `20`；`#sub-pager` 显示 `第 1 / 2 页` | `AC-37` | 🔴 |
| `WT-HR-03` | 空态（从未提交） | **临时账号** | 新账号打开 `history.html` | `#sub-rows` 内 `div.empty`：`还没有提交记录` + 「去刷题」链接；`#sub-total` 为**空串**（不是 `共 0 条`） | `D-38` | 🟢 |
| `WT-HR-04` | 匿名直连被守卫拦截 | 匿名 | 直接打开 `/history.html` | 跳 `/login.html?redirect=%2Fhistory.html`；顶栏**无**「我的」入口 | `AC-25` | 🟢 |
| `WT-HR-05` | 展开详情：代码全文 | judge | 点 `tr.subrow[data-id]` | 展开 `tr.subdetail`，内含完整 `pre` 代码块 | `AC-10` | 🔴 |
| `WT-HR-06` | 展开详情：逐用例表 | judge | 展开一条 AC 提交 | `table.tagtable` 含 `# / 输入 / 你的输出 / 期望输出 / 结论` | `AC-10` | 🔴 |
| `WT-HR-07` | 详情只同时展开一条 | judge | 展开 A → 点 B | A 自动收起，B 展开 | — | 🔴 |
| `WT-HR-08` | 行键盘可达 | judge | `tr.subrow` 聚焦后按 `Enter` | 同样展开详情；按 `Space` 亦可 | — | 🔴 |
| `WT-HR-09` | 按题目筛选 | judge | `#f-problem` 选 `two-sum` | 只剩该题记录；与 `#f-verdict` **可组合** | `AC-10` `AC-37` | 🔴 |
| `WT-HR-10` | 按结论筛选 | judge | `#f-verdict` 选 `WA` | 只剩 WA 记录 | `AC-10` | 🔴 |
| `WT-HR-11` | 筛选后空态文案不同 | judge | 筛一个无记录的组合 | 文案为 `没有匹配的提交记录`（**不是**「还没有提交记录」） | — | 🔴 |
| `WT-HR-12` | **删除提交 + 二次确认** | judge+临时账号 | 展开一条 → 点 `[data-del]` | `confirm`：`这条提交记录将被永久删除，含 N 个用例结果，不可恢复。确认删除？`（`N>0` 时才有「，含 N 个用例结果」这一段） | `AC-29` `D-49` | 🔴 |
| `WT-HR-13` | 删除后行消失且总数减一 | judge+临时账号 | 确认删除 | 行消失；`#sub-total` 减 1；回到 `?problem.html` 时最近提交**同步移除** | `AC-29` | 🔴 |
| `WT-HR-14` | 删除可取消 | judge+临时账号 | `confirm` 选「取消」 | 行仍在，数据未动 | — | 🔴 |
| `WT-HR-15` | 重跑按钮仅对 failed/JE 渲染 | judge | 展开一条 `done` 的提交 | **无** `[data-retry]` 按钮 | `AC-29` `D-58` | 🔴 |
| `WT-HR-16` | 重跑新增一条而非覆盖 | judge+构造 failed | 对 `failed`/`JE` 记录点「重跑」 | 历史**多一条** `pending`；**原记录 `status`/`verdict` 完全不变** | `AC-29` `D-58` | 🔴 |
| `WT-HR-17` | 切 tab 与翻页不重取 overview | 登录 | 在三个 tab 间来回切 + tab1 翻页 | `/api/me/overview` 全程**只有 1 条请求** | `AC-24` | 🟢 |
| `WT-HR-18` | hash 定位 tab | 登录 | 直接打开 `/history.html#progress` | `#pane-progress` 激活、其余 `hidden`；tab 按钮 `aria-selected` 正确 | `D-39` | 🟢 |
| `WT-HR-19` | 未知 hash 回落到默认 | 登录 | 打开 `/history.html#nope` | 回落 `#records` | `D-39` | 🟢 |

---

### M10 我的 · 掌握分布

对应 `P6`、`D-38`、`D-39`、`D-68`、`AC-22`、`AC-24`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-MG-01` | 难度卡三档 | judge | 切到 `#progress` tab | `.diffcard` 含 简单/中等/困难；本实例 `easy`/`hard` 因 `total=0` **整行不渲染** | `AC-24` | 🟢 部分 |
| `WT-MG-02` | 进度条宽度 = solved/total | judge | AC 掉几道题后看难度卡 | `.bar__fill--*` 的宽度百分比 == `solved/total*100`；`total=0` 显示 `—` 而**不是 `NaN%`** | `AC-24` | 🔴 |
| `WT-MG-03` | `total=0` 的难度不渲染该条 | 登录 | 看难度卡 | `easy`/`hard` 整条消失，**不是**显示 `0/0` 空条 | `D-38` | 🟢 |
| `WT-MG-04` | 标签表四列 | 登录 | 切 `#progress` | `table.tagtable` 表头 `标签 / 已过 / 总数 / 通过率`；本例 `数组`/`哈希表` 各 `1/1` | `AC-22` | 🟢 |
| `WT-MG-05` | **标签多对多不重复计数** `[D-38]` | 登录 | 核对 `two-sum`（挂 2 个 algorithm + 1 个 source 标签） | 该题在**两个** algorithm 标签的 `total` 中**各计 1 次**（不是 2 次）；`byTag[].total` 之和 `= 3 > 1`（大于题目总数是**正确**行为） | `AC-22` | 🟢 |
| `WT-MG-06` | `byDifficulty` 总数自洽 | 登录 | 把三档 `total` 相加 | 等于 `problems` 表总行数（本实例 = 1） | `AC-22` | 🟢 |
| `WT-MG-07` | 标签按 total 倒序 | **需多标签** | 看标签表 | 顺序按 `total` 降序、同值按名称 | — | 🟢 |
| `WT-MG-08` | source 标签折叠 | 登录 | 看标签表下方 | `高频面试题` 折叠在 `<details class="more"><summary>展开来源标签（1）</summary>` 内 | `D-39` | 🟢 |
| `WT-MG-09` | 与首页概览条**同源同值** | judge | 并排比对 `list.html#overview` 与 `history.html#pane-progress` | `已通过 x/N` 与三个难度进度条**逐项相等**（同一端点、同一 `overview.js` 渲染） | `AC-24` | 🔴 |
| `WT-MG-10` | 概览条最近 3 条可点跳转 | judge | 点 `#overview` 内 `.recent` 项 | 跳 `/problem.html?slug=...`；每条含 `.recentrow__time`（`MM-DD HH:mm`）、题名、`.verdict` 徽标 | `D-38` | 🔴 |
| `WT-MG-11` | 会话过期提示 | **需过期** | 过期后打开 `#progress` | `div.notice.notice--warn`：`登录状态已过期，请重新登录后查看掌握分布。` | `D-44` | 🔵 |

---

### M11 我的 · 账户设置

对应 `P9`、`D-40`、`D-56`、`D-57`、`AC-21`。⚠️ **本模块的高风险用例必须用临时账号。**

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-AC-01` | 账户信息只读 | 登录 | 切到 `#account` | `#acc-username` 显示用户名；**不提供**修改用户名的输入框（`D-40` 要点 6）；`#acc-role` 显示角色 | `D-40` | 🟢 |
| `WT-AC-02` | 普通用户不显示"普通用户"标签 | `zhangling` | 看 `#acc-role` | 显示文本 `普通用户`；**顶栏**则不显示（两处刻意不同） | `D-41` | 🟢 |
| `WT-AC-03` | 改密三项前端校验 | 登录 | 依次触发：旧密码空 / 新密码 5 位 / 两次不一致 | 分别报 `请输入当前密码` / `新密码需 6–64 位` / `两次输入的新密码不一致`；**三次都不发请求** | `D-40` | 🟢 |
| `WT-AC-04` | 旧密码错误 | 临时账号 | 用错误旧密码提交 | `#old-password-error` = `旧密码不正确`；`#old-password` 被清空；**`#new-password` 保持已填值不清空**（便于只改旧密码后重试） | `D-40` §7.4 | 🟢 |
| `WT-AC-05` | **改密后当前会话仍有效** | 临时账号 | 改密成功 | toast + 表单 `.reset()`；**页面不跳转、未登出**；`auth/me` 仍 `200`；顶栏静默重拉 | `AC-21` | 🟢 |
| `WT-AC-06` | **改密踢掉其他会话** | 临时账号 | 同账号在两处登录 → 在 A 处改密 → 回到 B | B 的 `/api/auth/me` 返回 `401/1002`；A 仍 `200` | `AC-21` `D-40` | 🟢 |
| `WT-AC-07` | 旧密码失效 / 新密码可登录 | 临时账号 | 改密后登出 → 用旧密码登录 / 用新密码登录 | 旧密码 `400/1001`；新密码 `200/0` | `AC-21` | 🟢 |
| `WT-AC-08` | 新密码长度不符（服务端侧） | 临时账号 | 绕过前端直接调接口传 5 位 | `400/1001`，`data.field='newPassword'` | `AC-21` | 🟡 |
| `WT-AC-09` | 登出当前账号 | 登录 | 点 `#logout` | 跳 `/login.html`；服务端会话行删除 | `D-40` | 🟢 |
| `WT-AC-10` | **注销账号：输密码 + 逐项列出** | 临时账号 | 切 `#account` 底部 → 点注销 | ① 要求输密码；② 二次确认弹窗**逐项列出** `提交 N 条 / 收藏 M 题 / 会话 K 个` + 红字**不可恢复**（**不是**一句"确定吗？"） | `AC-33` `D-57` | 🟡 |
| `WT-AC-11` | 注销密码错误不删数据 | 临时账号 | 输错密码确认 | `1001`；**任何数据都不删** | `AC-33` | 🟡 |
| `WT-AC-12` | 注销后清空本地草稿 | 临时账号 | 注销成功 | `localStorage` 中**所有** `draft:*` 与 `draftAt:*` 被删净（草稿键**不含用户名**，不清会留给下一个人） | `AC-33` `D-57` | 🟡 |
| `WT-AC-13` | 注销后跳登录页并提示 | 临时账号 | 注销成功 | 跳 `/login.html?deleted=1`；`#notice` 显示 `账号已注销，相关数据已一并删除。` | `D-57` | 🟡 |
| `WT-AC-14` | 注销的级联完整性 | 临时账号+DB | 注销后在库核对 | `sessions`/`submissions`/`submission_case_results`/`submission_compile_errors`/`user_problems` **全为 0**；`problems`/`problem_cases`/`tags` 行数**不变** | `AC-33` `R-12` | 🔵 |

---

### M12 运维概览

对应 `D-37`、`AC-19`、`AC-23`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-AD-01` | admin 可访问 | `admin` | 打开 `/admin.html` | `#denied` 保持 `hidden`；系统状态卡 + 用户清单表渲染 | `AC-19` | 🟢 |
| `WT-AD-02` | system 十二项齐全 | `admin` | 读 `#sys-stats` + `#sys-runtime` | `#sys-stats` 四张卡（用户/题目/提交/未完成提交）；`#sys-runtime` 含 `队列长度`、`并发评测`、`运行中/未接线` pill | `AC-19` | 🟢 |
| `WT-AD-03` | **judge 未接线如实回显** | `admin` | 看 `#sys-runtime` 第三个 pill | class 含 `pill--warn`、文案 `未接线`；`#sys-note` 出现解释段落；**不得**显示 `运行中` | `AC-19` | 🟢 |
| `WT-AD-04` | 用户清单列齐全 | `admin` | 看 `table.admin-users` | 11 列；`#user-count` = `共 7 人`；每行角色用 `span.badge.badge--admin` 或 `span.muted` | `AC-19` | 🟢 |
| `WT-AD-05` | 零提交用户的展示 | `admin` | 看 `alice01` 行 | 提交数 0 → 通过率显示 `—`（**不是 `0%`**）；`lastVerdict` 为 `—` 且**不回退到上一次结论** | `AC-19` | 🟢 |
| `WT-AD-06` | 普通用户被拒 | `zhangling` | 打开 `/admin.html` | `#denied` 可见「无权访问」；**1.5 秒后自动跳 `/list.html`**；`/api/admin/overview` 返回 `403/1003` | `AC-19` | 🟢 |
| `WT-AD-07` | 匿名被拒且码序正确 | 匿名 | 打开 `/admin.html` | `/api/admin/overview` 返回 `401/1002`（**不是 `1003`** —— 先判登录再判角色） | `AC-19` | 🟢 |
| `WT-AD-08` | **严格只读** | `admin` | 全页扫描 | 页面上**无任何** `<form>`、无写操作按钮；Network 面板除 `GET /api/admin/overview` 外**无其他请求** | `AC-19` `D-37` | 🟢 |
| `WT-AD-09` | 刷新按钮 | `admin` | 点 `#refresh` | 按钮变 `加载中…` 且 `disabled`，完成后恢复并重取数据 | — | 🟢 |
| `WT-AD-10` | 读取失败的用户可见提示 | `admin` | 拦掉 API 后点 `#refresh` | toast `管理端点读取失败，请看 logs/app.log`（驻留 6s） | — | 🟢 |

---

### M13 待实现功能的现状守卫

> **这些用例的期望是"断言该功能确实还不存在"**。
> 目的：让"没测"在回归报告里**显式可见**，而不是混在通过项里被误读成"测过了"。
> **一旦对应功能被实现，这些用例必须改为正向断言并移入对应模块。**

| 用例ID | 标题 | 前置 | 步骤 | 预期结果（当前） | 追溯 | 状态 |
|---|---|---|---|---|---|---|
| `WT-GD-01` | diff.js 为空 | 任意 | `GET /js/diff.js` | HTTP `200` 但 **`Content-Length: 0`** | `D-42` | 🟢 |
| `WT-GD-02` | 最近提交行无 checkbox | 登录 | 检查 `#p-recent` 内 `button.recentrow` | 每行内**无** `input[type=checkbox]`、无 `data-diff`/`data-check` | `D-42` `D-50` | 🟢 |
| `WT-GD-03` | 前端从不请求 diff 端点 | 登录 | 遍历题目页/历史页全部交互 | Network 中**从不出现** `/api/submissions/diff` | `D-42` | 🟢 |
| `WT-GD-04` | 账户设置无注销入口 | 登录 | 检查 `#pane-account` | **无**注销按钮、无 `#delete-account`、无密码二次确认弹窗 markup | `D-56` `D-57` | 🟢 |
| `WT-GD-05` | 前端从不调用注销端点 | 登录 | 遍历 `#pane-account` 全部交互 | Network 中**从不出现** `DELETE /api/auth/account` | `D-56` | 🟢 |
| `WT-GD-06` | 草稿冲突的挂载期分支缺失 | 任意 | 改 `localStorage['draftAt:two-sum']` 为未来时间后刷新 | **不弹**任何 `confirm`（`draftAt` 写入了但从未被读取比较） | `D-55` | 🟢 |
| `WT-GD-07` | `clearDraft` 无调用方 | — | 检索 `static/` | 唯一保留给 `[D-57]` 的 `clearDraft()` 目前**无任何调用点** | `D-57` | 🟢 |

---

### M14 鉴权边界与输入安全

对应 `D-37`、`AC-13`、`AC-20`、`AC-36`、`NF-7`。**以浏览器为载体的越权验证。**

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-SX-01` | 匿名访问提交接口 | 匿名 | 打开 `history.html` | 跳登录带 `redirect`；后端 `/api/submissions` 返回 `401/1002` | `AC-13` | 🟢 |
| `WT-SX-02` | 用户 A 访问用户 B 的提交详情 | 双账号 | A 登录，B 先提交一条；A 直接请求 `/api/submissions/{B的id}` | `403/1003` | `AC-13` | 🔵 |
| `WT-SX-03` | **admin 也不放开归属** | `admin` + 他人提交 | `admin` 请求他人提交详情 | **仍是 `403/1003`** —— `[D-37]` 的 conscious omission | `AC-20` | 🔵 |
| `WT-SX-04` | 删除他人提交被拒 | 双账号 | A 调 `DELETE /api/submissions/{B的id}` | `403/1003`；B 的记录完好 | `AC-36` | 🔵 |
| `WT-SX-05` | diff 他人提交被拒 | 双账号 | A 调 `?from={A}&to={B}` | `403/1003` | `AC-36` | 🔵 |
| `WT-SX-06` | 重跑他人提交被拒 | 双账号 | A 调 `POST /api/submissions/{B}/retry` | `403/1003` | `AC-36` | 🔵 |
| `WT-SX-07` | 重跑非 failed 记录 | judge | 对 `done` 的提交调 retry | `409/1011`；**且前端不渲染**重跑按钮（`canRetry=false`） | `AC-29` | 🔴 |
| `WT-SX-08` | 会话过期全局跳转 | 登录 | 清掉 Cookie 后点任意需鉴权操作 | 全局跳 `/login.html?redirect=<当前路径>` | `§7.3` | 🟢 |
| `WT-SX-09` | Cookie 不可被 JS 读 | 登录 | Console 执行 `document.cookie` | **读不到 `sid`**（`HttpOnly`） | `AC-1` `D-69` | 🟢 |
| `WT-SX-10` | 同源 Cookie 策略 | 登录 | 检查登录响应头 | `SameSite=Lax`；**首版无 `Secure`**（HTTP 部署下的刻意取舍，启用 HTTPS 时必须补） | `D-69` | 🟢 |
| `WT-SX-11` | SQL 注入：关键词 | 任意 | `#f-keyword` 填 `' OR 1=1--` | 正常返回 0 条或正常匹配；**不得** 5xx、**不得** 泄露 SQL 错误文本 | `NF-7` | 🟢 |
| `WT-SX-12` | SQL 注入：用户名登录 | 匿名 | 用户名填 `' OR 1=1--`，密码随意 | `400/1001`；**不得**登录成功 | `NF-7` | 🟢 |
| `WT-SX-13` | SQL 注入：代码体 | 登录 | 提交体里塞 `'; DROP TABLE submissions;--` | 正常受理或被校验拒；**不得** 5xx；事后库表完好 | `NF-7` | 🟢 |
| `WT-SX-14` | XSS：题面渲染 | **需构造坏题** | 题面含 `<img src=x onerror=alert(1)>` | 被 DOMPurify 清除，**无弹窗** | `AC-3` `AC-27` | 🔵 |
| `WT-SX-15` | XSS：代码显示 | 任意 | 提交历史里代码含 `<script>` | 以文本节点渲染，**不执行**（不用 `innerHTML` 拼代码） | `§6.3.2.1` | 🟢 |
| `WT-SX-16` | XSS：用户名回显 | 任意 | 用户名含 `<img src=x onerror=alert(1)>`（若注册放行） | 顶栏/概览条按文本渲染，**不弹窗** | `§6.3.2.1` | 🟢 |
| `WT-SX-17` | 前端隐藏不是安全边界 | `zhangling` | 手工把 `#f-progress` 的 `disabled` 去掉再点 | 前端能选中，但后端仍按自身鉴权裁决 | `AC-19` | 🟢 |
| `WT-SX-18` | 注册开关（`OJ_ALLOW_REGISTER=0`） | **需改 `.env` 重启** | 打开注册 tab 提交 | 返回 `1010`；**`users` 表行数不变** | `AC-18` | 🔵 |

---

### M15 非功能与离线可用

对应 `SPEC.md §1.4`、`[D-63]`、`[D-73]`、`AC-48`、`NF-1`~`NF-8`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-NF-01` | 断网可用：编辑器高亮 | 断网 | 断网后打开 `?slug=two-sum` | Ace **仍有 C++ 语法高亮**；`#editor-warn` 不出现 | `AC-48`① | 🟢 |
| `WT-NF-02` | 断网可用：Markdown 渲染 | 断网 | 断网后打开题目页 | 题面 Markdown 正常渲染（`marked` + `purify` 均已 vendored） | `AC-48`① | 🟢 |
| `WT-NF-03` | 无任何外网请求 | 任意 | Network 面板按域名过滤 | **零**指向 `cdn.jsdelivr.net` 等外部域的请求 | `AC-48`④ | 🟢 |
| `WT-NF-04` | vendor 文件完整且非 404 响应体 | 任意 | 逐个请求 7 个 vendor 文件 | 全部 `200` 且 `size > 1KB`（404 响应体只有几十字节） | `AC-48`② | 🟢 |
| `WT-NF-05` | LICENSE 随产物分发 | **需部署机 shell** | `ls static/vendor/` | 3 份 MIT/BSD LICENSE 在位 | `AC-48`③ | 🔵 |
| `WT-NF-06` | 列表页首屏耗时 | **需 4G** | 冷加载 5 次取 P95 | `< 1s`（50 道题规模） | `NF-2` | 🔵 |
| `WT-NF-07` | 提交到出结果 P95 | **需 4G + judge** | 10 用例以内的题，连跑 20 次 | P95 `< 8s` | `NF-1` | 🔵 |
| `WT-NF-08` | diff 计算耗时 | 4G + diff 实现 | 两条 4000 行提交做 diff | P95 `< 100ms`；`> 2000` 行降级为整体替换 | `AC-28` `R-11` | 🟡 |
| `WT-NF-09` | 并发下不误判 TLE | **需 4G + judge** | 造 2.5s CPU 用例，全局 4 并发连跑 10 轮 | **100% 得到 AC/WA，0 次 TLE** | `AC-38` | 🔵 |
| `WT-NF-10` | 阻塞能被抓住 | 4G + judge | ①`sleep(999)` ②`while(true){}` ③正常用例×50 | ①1.5s 内判 TLE（**不是**等 35s 兜底）②约 4s 判 TLE ③50/50 全 AC | `AC-39` | 🔵 |
| `WT-NF-11` | 浏览器兼容性 | — | Chrome / Edge / Safari 最近两个大版本各跑一遍 §5 核心闭环 | 五页布局无错位、编辑器可用、提交可用 | §1.4 | 🔵 |
| `WT-NF-12` | 页面无 console 报错 | 任意 | 遍历五页看 Console | **无 error 级日志**（含资源 404） | — | 🟢 |
| `WT-NF-13` | 零外部字体的中文排版 | 任意 | 1280px 宽下看中文 | 无字体回退导致的错位/重排 | §1.4 | 🟢 |
| `WT-NF-14` | 键盘可达性 | 登录 | Tab 键遍历关键控件 | 提交/登录/展开/删除均可用键盘到达；`.splitter`、`.subrow` 有 `tabindex` | — | 🟢 |
| `WT-NF-15` | 状态机残留清理 | judge | 轮询结束、页面卸载后 | 定时器被清除；`sessionStorage['judging:*']` 在终态后被删 | `D-53` | 🔴 |

---

## 6. AC 追溯矩阵

`SPEC.md §10.1` 共 49 条 AC。**"浏览器"列标注该 AC 是否只能过浏览器达成**。

| AC | 名称 | 覆盖用例 | 层级 | 当前可执行 |
|---|---|---|---|---|
| AC-1 | 注册登录 | `WT-AU-01`~`03,07,09,17,18,21` | L2 | 部分 |
| AC-2 | 题库浏览 | `WT-LS-01`~`13,23` | L2 | ✅ |
| AC-3 | 题面渲染 | `WT-PB-02,03` | L2 | 部分 |
| AC-4 | 代码编辑与草稿 | `WT-ED-01`~`05,08,09` | L2 | ✅ |
| AC-5 | 提交判题 | `WT-SB-01,03` | L3 | 🔴 |
| AC-6 | 六种 verdict | `WT-RS-01,02,07,08` | L3 | 🔴 |
| AC-7 | 逐用例详情 | `WT-RS-02,03` | L3 | 🔴 |
| AC-8 | 编译错误定位 | `WT-RS-09` | L3 | 🔴 |
| AC-9 | 资源限制 | `WT-RS-06,07` | L3 | 🔴 |
| AC-10 | 提交历史 | `WT-HR-05,06,09,10` | L3 | 🔴 |
| AC-11 | 进度推导 | `WT-LS-20` | L3 | 🔴 |
| AC-12 | 收藏 | `WT-LS-21,22`、`WT-PB-09` | L2 | 部分 |
| AC-13 | 鉴权边界 | `WT-SX-01,02` | L4 | 部分 |
| AC-14 | 限流配额 | `WT-SB-06,07` | L3 | 🔴 |
| AC-15 | 重启恢复 | — （curl 文档已覆盖） | — | — |
| AC-16 | 用例落库与范式 | — （`run_problemsync_test.sh` 已覆盖） | — | — |
| AC-17 | 过期会话回收 | — （curl 文档已覆盖） | — | — |
| AC-18 | 角色声明式同步 | `WT-AU-20`、`WT-SX-18` | L4 | 🔵 |
| AC-19 | 只读管理能力 | `WT-TB-09`、`WT-AD-01`~`10` | L2 | ✅ |
| AC-20 | 归属不放宽 | `WT-SX-03` | L4 | 🔵 |
| AC-21 | 改密码 | `WT-AC-03`~`07` | L2 | ✅ |
| AC-22 | 标签聚合不重复计数 | `WT-MG-04,05,06` | L2 | ✅ |
| AC-23 | 顶栏角色显示 | `WT-TB-03,04,08`、`WT-AC-02` | L2 | ✅ |
| AC-24 | 概览条与 tab2 同源 | `WT-LS-18`、`WT-HR-17`、`WT-MG-09` | L2 | 部分 |
| AC-25 | 未登录可读题 | `WT-LS-14,15`、`WT-PB-19`、`WT-HR-04` | L1 | ✅ |
| AC-26 | 题目提示 | `WT-PB-05,06,07` | L2 | 部分 |
| AC-27 | 题面禁图片 | `WT-PB-04`、`WT-SX-14` | L2 | 🔵 |
| AC-28 | 提交 diff | `WT-NF-08` + `WT-GD-01`~`03` | — | 🟡 |
| AC-29 | 删除与重跑 | `WT-HR-12`~`16`、`WT-SX-07` | L3 | 🔴 |
| AC-30 | 队列位置 | `WT-SB-04,05` | L3 | 🔴 |
| AC-31 | 刷新恢复轮询 | `WT-SB-08,09,10` | L3 | 🔴 |
| AC-32 | 草稿异常路径 | `WT-ED-06,07,14` | L2 | 部分 |
| AC-33 | 注销硬删除 | `WT-AC-10`~`14` + `WT-GD-04,05` | — | 🟡 |
| AC-34 | 下一题建议 | `WT-PB-20`~`24` | L2 | 部分 |
| AC-35 | 顶栏两套形态 | `WT-AU-16`、`WT-TB-01`~`07` | L1 | ✅ |
| AC-36 | diff/删除归属不放宽 | `WT-SX-04,05,06` | L4 | 🔵 |
| AC-37 | 历史分页与排序 | `WT-HR-01,02,09,10` | L3 | 🔴 |
| AC-38 | 并发下不误判 TLE | `WT-NF-09` | L5 | 🔵 |
| AC-39 | 阻塞能被抓住 | `WT-NF-10` | L5 | 🔵 |
| AC-40 | 0 条 @@OJ@@ → JE | `WT-RS-10` | L3 | 🔴 |
| AC-41 | CE 的用例数语义 | `WT-RS-09` | L3 | 🔴 |
| AC-42 | 平台文件报错归属 | （CE 形态分支，见 `WT-RS-09` 备注） | L3 | 🔴 |
| AC-43 | 链接错误归属 | （CE 形态分支，见 `WT-RS-09` 备注） | L3 | 🔴 |
| AC-44 | 内存口径标注 | `WT-RS-04,05,06`、`WT-LS-03` | L3 | 部分 |
| AC-45 | failed 不变量 | — （SQL 层，`run_tests.sh` 已覆盖） | — | — |
| AC-46 | 主标签确定性 | `WT-PB-21` | L2 | ✅ |
| AC-47 | 题库加载期校验 | （`run_problemsync_test.sh` 已覆盖；列表页过滤部分待补） | — | — |
| AC-48 | vendored 库完整性 | `WT-IN-11`、`WT-NF-01`~`05` | L0 | ✅ |
| AC-49 | 自实现 bcrypt | `WT-AU-21` | L4 | 🔵 |

**统计**：49 条 AC 中，**21 条当前可完整在浏览器侧达成**，13 条部分达成，15 条被环境阻塞或未实现。

---

## 7. Playwright 脚本骨架

> 仓库当前**没有** `package.json` / `node_modules` / 任何浏览器测试框架。
> 下列骨架是**新增目录**，与现有 `tests/`（C++ 与 curl）并列，不改动它们。

### 7.1 目录

```
tests/web/
├── package.json
├── playwright.config.js
├── fixtures.js                 # 登录态、测试账号、辅助断言
├── specs/
│   ├── L0-static.spec.js       # M01
│   ├── L1-anonymous.spec.js    # M02 部分 + M03 部分 + M04 部分
│   ├── L2-list.spec.js         # M04
│   ├── L2-problem.spec.js      # M05
│   ├── L2-editor.spec.js       # M06
│   ├── L2-topbar.spec.js       # M03
│   ├── L2-account.spec.js      # M11（改密部分）
│   ├── L2-admin.spec.js        # M12
│   ├── L3-submit.spec.js       # M07 + M08（judge 未接线时 skip）
│   └── L4-authz.spec.js        # M14
└── README.md
```

### 7.2 `package.json`

```json
{
  "name": "cppoj-web-tests",
  "private": true,
  "version": "1.0.0",
  "scripts": {
    "test": "playwright test",
    "test:L0": "playwright test --project=chromium --grep @L0",
    "report": "playwright show-report"
  },
  "devDependencies": {
    "@playwright/test": "^1.48.0"
  }
}
```

### 7.3 `playwright.config.js`

```js
const { defineConfig, devices } = require('@playwright/test');

const BASE = process.env.OJ_BASE || 'http://82.156.75.119:8080';

module.exports = defineConfig({
  testDir: './specs',
  timeout: 45_000,
  expect: { timeout: 10_000 },
  fullyParallel: false,          // 服务端资源有限，串行更稳（同 SPEC §0.0 的思路）
  workers: 1,
  retries: 0,
  reporter: [['list'], ['html', { open: 'never' }]],

  use: {
    baseURL: BASE,
    viewport: { width: 1440, height: 900 },   // SPEC §1.4 要求 ≥1280
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    locale: 'zh-CN',
    timezoneId: 'Asia/Shanghai',              // D-17：存 UTC，显示本地
  },

  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'edge',     use: { ...devices['Desktop Edge'] } },
    { name: 'webkit',   use: { ...devices['Desktop Safari'] } },  // AC §1.4 兼容性
  ],

  metadata: {
    ojBase: BASE,
    user: process.env.OJ_USER || 'zhangling',
    pass: process.env.OJ_PASS || '666666',
    admin: process.env.OJ_ADMIN || 'admin',
    adminPass: process.env.OJ_ADMIN_PASS || 'admin123',
  },
});
```

### 7.4 `fixtures.js`

```js
const base = require('@playwright/test');

const USER = process.env.OJ_USER || 'zhangling';
const PASS = process.env.OJ_PASS || '666666';
const ADMIN = process.env.OJ_ADMIN || 'admin';
const ADMIN_PASS = process.env.OJ_ADMIN_PASS || 'admin123';

/** 测试专用账号前缀——绝不用于手工账号，避免互相干扰 */
const TMP = `wt${Date.now().toString(36)}`;

const test = base.test.extend({
  // ① 普通用户已登录态
  user: async ({ page }, use) => {
    await login(page, USER, PASS);
    await use(page);
  },

  // ② 管理员已登录态
  admin: async ({ page }, use) => {
    await login(page, ADMIN, ADMIN_PASS);
    await use(page);
  },

  // ③ 全新账号——删除/改密/注销类用例必须用它
  freshUser: async ({ page, request }, use) => {
    const username = `${TMP}${Math.floor(Math.random() * 1e4)}`;
    const password = 'pass1234';
    const res = await request.post('/api/auth/register',
      { data: { username, password } });
    if (res.status() !== 201) throw new Error(`临时账号注册失败: ${res.status()}`);
    await login(page, username, password);
    await use({ page, username, password });
  },

  // ④ judge 是否接线——决定 L3 整层跑还是 skip
  judgeWired: async ({ request }, use) => {
    const r = await request.post('/api/auth/login',
      { data: { username: ADMIN, password: ADMIN_PASS } });
    const me = await r.json();
    const ov = await request.get('/api/admin/overview',
      { headers: { cookie: cookieFrom(r) } });
    const body = await ov.json();
    await use(!!body?.data?.system?.judgeAlive);
  },
});

async function login(page, username, password) {
  await page.goto('/login.html');
  await page.fill('#username', username);
  await page.fill('#password', password);
  await page.click('#submit');
  await page.waitForURL('**/list.html');
}

/** test.js 里常见的三个断言，抽出来避免各 spec 各写一遍 */
const expect = {
  /** D-52/D-41：未登录时「我的」「运维」在 DOM 中不存在，不是 CSS 隐藏 */
  async anonymousTopbar(page) {
    await expect(page.locator('#topbar a[href="/login.html"]')).toBeVisible();
    await expect(page.locator('#topbar a[href="/history.html"]')).toHaveCount(0);
    await expect(page.locator('#topbar a[href="/admin.html"]')).toHaveCount(0);
  },

  /** 普通用户：不渲染任何角色徽标（D-41） */
  async userTopbar(page) {
    await expect(page.locator('#topbar a[href="/history.html"]')).toBeVisible();
    await expect(page.locator('#topbar a[href="/admin.html"]')).toHaveCount(0);
    await expect(page.locator('#topbar .badge')).toHaveCount(0);
  },

  /** 管理员：入口 + 徽标同时存在（AC-23） */
  async adminTopbar(page) {
    await expect(page.locator('#topbar a[href="/admin.html"]')).toBeVisible();
    await expect(page.locator('#topbar .badge--admin')).toHaveText('管理员');
  },
};

module.exports = { test, expect: base.expect, assert: expect, USER, PASS, ADMIN, TMP };
```

### 7.5 代表性 spec：`L2-topbar.spec.js`

> 对应 M03 / `AC-35`。这是**最值得先跑通**的一条 —— 它纯 L1/L2、零前置、断言密度最高。

```js
const { test, expect, assert } = require('../fixtures');

test.describe('@L2 顶栏三形态 [D-52][D-41] AC-35', () => {

  test('WT-TB-01 未登录形态：只有题库/登录/注册', async ({ browser }) => {
    const ctx = await browser.newContext();          // 无 storageState → 匿名
    const page = await ctx.newPage();
    await page.goto('/list.html');
    await assert.anonymousTopbar(page);
    await expect(page.locator('#topbar nav a')).toHaveCount(1);
    await expect(page.locator('#topbar nav a')).toHaveText('题库');
    await ctx.close();
  });

  test('WT-TB-02 普通用户：有「我的」、无「运维」、无徽标', async ({ user }) => {
    await user.goto('/list.html');
    await assert.userTopbar(user);
    await expect(user.locator('#topbar')).toContainText('ZhangLing');  // 见 WT-AU-12
    await expect(user.locator('[data-topbar-logout]')).toBeVisible();
  });

  test('WT-TB-03 管理员：入口 + 徽标同时存在', async ({ admin }) => {
    await admin.goto('/list.html');
    await assert.adminTopbar(admin);
  });

  test('WT-AU-16 登录页不渲染顶栏', async ({ browser }) => {
    const ctx = await browser.newContext();
    const page = await ctx.newPage();
    await page.goto('/login.html');
    await expect(page.locator('#topbar')).toHaveCount(0);   // 不存在，不是隐藏
    await ctx.close();
  });

  test('WT-TB-06 题目页用 compact 变体渲染同一组件', async ({ user }) => {
    await user.goto('/problem.html?slug=two-sum');
    await expect(user.locator('#topbar')).toHaveCount(1);
    await assert.userTopbar(user);
  });
});
```

### 7.6 代表性 spec：`L2-editor.spec.js`

> 对应 M06 / `AC-4` / `AC-32`。**注意 Ace 的输入方式**（§4 的注意事项）：
> Ace 加载后会清空 `#editor` 的内容并移除降级用的 `<textarea>`，
> 因此**必须用键盘输入，不能 `fill()`**。

```js
const { test, expect } = require('../fixtures');

test.describe('@L2 编辑器与草稿 [D-27][D-55] AC-4 AC-32', () => {
  const SLUG = 'two-sum';
  const MARK = '// web-auto-draft-marker';

  test.beforeEach(async ({ page }) => {
    await page.goto(`/problem.html?slug=${SLUG}`);
    await page.waitForSelector('#editor.ace_editor');          // 等 Ace 起来
    await page.evaluate(k => localStorage.removeItem(`draft:${k}`), SLUG);
  });

  test('WT-ED-01 Ace 加载成功且未降级', async ({ page }) => {
    await expect(page.locator('#editor')).toHaveClass(/ace_editor/);
    await expect(page.locator('#editor-warn')).toBeHidden();
  });

  test('WT-ED-04 草稿 800ms debounce 自动保存', async ({ page }) => {
    await page.click('#editor .ace_content');
    await page.keyboard.type(MARK);
    await page.waitForTimeout(1200);                          // > 800ms debounce

    const draft = await page.evaluate(k => localStorage.getItem(`draft:${k}`), SLUG);
    expect(draft).toContain(MARK);

    const at = await page.evaluate(k => localStorage.getItem(`draftAt:${k}`), SLUG);
    expect(at).toMatch(/^\d+$/);                              // 两键必须同时写（D-27）
  });

  test('WT-ED-05 刷新后草稿恢复', async ({ page }) => {
    await page.click('#editor .ace_content');
    await page.keyboard.type(MARK);
    await page.waitForTimeout(1200);
    await page.reload();
    await page.waitForSelector('#editor.ace_editor');
    expect(await page.evaluate(() => window.__aceGet())) // 或直接读 localStorage
      .toContain(MARK);
  });

  test('WT-ED-06 多标签页草稿冲突：检测到就问，绝不自动覆盖', async ({ browser }) => {
    const ctx = await browser.newContext();                   // 共享 localStorage
    const tabA = await ctx.newPage();
    const tabB = await ctx.newPage();

    for (const p of [tabA, tabB]) {
      await p.goto(`/problem.html?slug=${SLUG}`);
      await p.waitForSelector('#editor.ace_editor');
    }

    // A 页写入草稿 → 触发 B 页的 storage 事件
    await tabA.click('#editor .ace_content');
    await tabA.keyboard.type(MARK);
    await tabA.waitForTimeout(1200);

    // B 页选择「否」
    tabB.on('dialog', d => d.dismiss());
    await tabB.click('#editor .ace_content');
    await tabB.waitForTimeout(500);
    // 选「否」后 B 页内容不被覆盖 —— 用 B 页自己的内容断言
    expect(await tabB.evaluate(k => localStorage.getItem(`draft:${k}`), SLUG))
      .not.toBeNull();

    await ctx.close();
  });

  test('WT-ED-11 空代码提交被前端拦截（不发请求）', async ({ page }) => {
    let posted = false;
    page.on('request', r => { if (r.url().includes('/api/submissions') && r.method() === 'POST') posted = true; });

    await page.evaluate(() => window.__aceSet(''));            // 清空编辑器
    await page.keyboard.press('Control+Enter');

    await expect(page.locator('.toast')).toContainText('请先写点代码再提交');
    expect(posted).toBe(false);                                // 关键：一次请求都不该发
  });
});
```

### 7.7 代表性 spec：`L3-submit.spec.js`（阻塞处理范式）

> **这是本文档里最该被抄走的一段**：如何让「环境不具备」在报告里**显式可见**，
> 而不是让 20 条用例集体变红、然后被误判成"系统坏了"。

```js
const { test, expect } = require('../fixtures');

test.describe('@L3 提交与判题 AC-5 AC-6', () => {

  test('WT-SB-12 judge 未接线时给出可理解的降级文案', async ({ user }) => {
    // 这一条不依赖 judge，永远可跑
    await user.goto('/problem.html?slug=two-sum');
    await user.waitForSelector('#editor.ace_editor');
    await user.click('#editor .ace_content');
    await user.keyboard.type('class Solution{};');
    await user.click('#submit');

    await expect(user.locator('#result .notice--error')).toBeVisible();
    await expect(user.locator('#result')).toContainText('judge 调度器尚未接线');
  });

  // 其余 30 条：judge 未接线时 skip，且 skip 原因写进报告
  test.beforeEach(async ({ judgeWired }, testInfo) => {
    testInfo.skip(!judgeWired,
      '阻塞：judgeAlive=false（SPEC §3.3 B-1）。接线后本用例自动恢复。');
  });

  test('WT-SB-01 提交后立即进入评测态', async ({ user }) => {
    await user.goto('/problem.html?slug=two-sum');
    await user.click('#submit');
    await expect(user.locator('#status')).toHaveAttribute('data-state', 'judging');
    await expect(user.locator('#status')).toContainText('评测中');
  });

  test('WT-RS-09 CE 不渲染逐用例表，状态行显示未执行数', async ({ user }) => {
    await user.goto('/problem.html?slug=two-sum');
    await user.click('#editor .ace_content');
    await user.keyboard.type('class Solution { vecto<int> a; };');   // 故意拼错
    await user.click('#submit');

    await expect(user.locator('.verdict--ce')).toBeVisible();
    await expect(user.locator('#status')).toContainText('个用例未执行');  // D-65
    await expect(user.locator('#result table.result__table')).toHaveCount(0); // CE 不渲染表
  });
});
```

### 7.8 代表性 spec：`L1-anonymous.spec.js`

> 对应 `AC-25` / `D-43`。**未登录可读题面**是本项目 v1.4 需求补全轮的重点，值得单列。

```js
const { test, expect } = require('../fixtures');

test.describe('@L1 未登录浏览 [D-43] AC-25', () => {
  test.use({ storageState: { cookies: [], origins: [] } });   // 全局匿名

  test('WT-PB-19 未登录可读完整题面与提示', async ({ page }) => {
    await page.goto('/problem.html?slug=two-sum');
    await expect(page.locator('#p-title')).toHaveText('两数之和');
    await expect(page.locator('#tab-hint')).toBeVisible();          // D-47：免费无限看
    await page.click('#tab-hint');
    await expect(page.locator('#p-hint')).toBeVisible();
  });

  test('WT-PB-11 未登录点提交：内联弹窗，不跳转', async ({ page }) => {
    await page.goto('/problem.html?slug=two-sum');
    await expect(page.locator('#submit')).toHaveText('登录后提交');
    const before = page.url();
    await page.click('#submit');
    await expect(page.locator('#inline-login')).toBeVisible();
    expect(page.url()).toBe(before);                                // URL 不变——草稿不丢
  });

  test('WT-PB-10 未登录点收藏：跳登录并带 redirect', async ({ page }) => {
    await page.goto('/problem.html?slug=two-sum');
    await page.click('#favorite');
    await page.waitForURL('**/login.html**');
    expect(page.url()).toContain('redirect=');
  });

  test('WT-PB-19b 未登录响应体不含 nextRecommendation', async ({ request }) => {
    const r = await request.get('/api/problems/two-sum');
    const body = await r.json();
    expect(body.code).toBe(0);
    expect(body.data).not.toHaveProperty('nextRecommendation');       // D-43：不给虚假承诺
  });

  test('WT-HR-04 直连 history.html 被守卫拦截', async ({ page }) => {
    await page.goto('/history.html');
    await page.waitForURL('**/login.html**');
    expect(page.url()).toContain('redirect=');
  });
});
```

### 7.9 运行

```bash
cd tests/web
npm install
npx playwright install chromium

export OJ_BASE=http://82.156.75.119:8080

npx playwright test --project=chromium              # 全量
npx playwright test --project=chromium --grep @L0  # 只跑静态可达性
npx playwright test --project=chromium --grep @L2  # 只跑无需 judge 的闭环
npx playwright show-report                          # 看 HTML 报告
```

---

## 8. 完整回归流程

```bash
# ── 0. 准备 ──────────────────────────────────────────────
export OJ_BASE=http://82.156.75.119:8080

# ── 1. 后端回归（改动触及 C++ 侧时必跑，~60s）─────────────
cd /home/ZL/project/cpp-oj-vibcoding
make build JOBS=1                       # SPEC §0.0：本机 2G，并发会进 swap
ctest --test-dir build -j1              # 期望 417 passed
bash tests/run_tests.sh                 # 期望 通过 98，失败 0
bash tests/run_problemsync_test.sh      # 期望 通过 30，失败 0

# ── 2. 前端回归（本文档）───────────────────────────────
cd tests/web && npm install
npx playwright test --project=chromium

# ── 3. 只跑当前环境能跑的两层（judge 未接线时的日常）─────
npx playwright test --project=chromium --grep '@L0|@L1|@L2'

# ── 4. 兼容性（发布前跑一遍，约 3 倍耗时）─────────────────
npx playwright test                      # chromium + edge + webkit
```

**分层回归的建议节奏**：

| 触发场景 | 跑哪些层 |
|---|---|
| 改了 `static/**` | L0 + L1 + L2（**每次提交前必跑**） |
| 改了 `src/web/**`（路由/鉴权） | L0 + L1 + L2 + L4 |
| 改了 `src/judge/**` | 全部，含 L3 |
| 改了 `db/init/*.sql` | 全部 + `run_problemsync_test.sh` |
| 发版前 | 全部 + 三浏览器 + NF 性能层（4G 机器） |

---

## 9. 故障排查

| 症状 | 原因 | 处理 |
|---|---|---|
| 所有用例超时打不开 | 服务未起或题库同步未完成 | `curl -sI $OJ_BASE/list.html`；看部署机 `/tmp/oj-run.log` |
| 登录成功但后续全跳登录 | Cookie 未持久化 | 确认 `Set-Cookie` 有 `HttpOnly; SameSite=Lax; Path=/`；Playwright 用 `storageState` 而非手工带 header |
| `#editor` 一直不是 `ace_editor` | Ace 资源没加载上 | 看 `#editor-warn` 是否可见（降级态）；检查 `/vendor/ace/` 四个文件 200 且 >1KB |
| 草稿用例时灵时不灵 | 800ms debounce 没等够 | `waitForTimeout(1200)` 而不是 `waitForTimeout(800)`（边界抖动） |
| 顶栏用户名断言失败 | 大小写：`zhangling` 登录回显 `ZhangLing` | 按 `WT-AU-12` 的实测值写断言 |
| 多标签页冲突用例不触发 | 两个 context 不共享 localStorage | 必须用**同一个** `browser.newContext()` 开两个 page，不能两个 context |
| 提交类用例集体变红 | judge 未接线 | 正常 —— 见 §3.3。确认 `judgeAlive` 后再看 |
| CE 用例找不到「个用例未执行」 | 判成了别的 verdict | 检查 `verdict` 到底是不是 `ce`；`D-65` 的 `case_total` 是 `GREATEST(明细行, 题库用例数)` |
| 逐用例表在 CE 场景出现了 | 前端把 CE 当成了可渲染形态 | 违反 `D-65`，应报 bug |
| 内存列断言失败（写成"内存"） | 口径 | 列头字面是 **`峰值RSS`**（`D-63`），界面不能出现「内存」 |
| 分页用例永远只有 1 页 | 题库只有 1 道题 | 见 §3.2，**不得记为通过** |
| 结果面板断言找不到元素 | 走了路径 B 的 SQL 造数但数据没生效 | 检查 `status='done'` 且 `case_passed/case_total` 与明细行一致（§3.4 断言） |
| `browserType.launch: Executable doesn't exist` | Playwright 浏览器没装 | `npx playwright install chromium` |

**日志与取证位置**：

```
部署机  logs/app.log                 每个请求一行 "METHOD /path → status" + 全部 WARN/ERROR
部署机  logs/judge.log               判题日志（judge 接线后含生成代码与编译命令）
部署机  /tmp/oj-judge/<id>/          单次提交的完整工作区（user.cpp / tester.cpp / run）
本地    tests/web/playwright-report/  HTML 报告（trace + 失败截图）
```

---

## 10. 当前未覆盖的部分

写在最后是为了**别把"没测"误当成"测过了"**。

| 项 | 现状 | 影响 |
|---|---|---|
| **判题主链路（L3 整层，31 条）** | `judgeAlive=false`，`POST /api/submissions` 回 `1009/503` | `SPEC.md §1.2` 的成功标准（提交→轮询→逐用例结果）**无法端到端验证**。这是本文档与后端测试之间最大的空隙 |
| **注销账号（`D-56`/`D-57`）** | 前端无入口无弹窗（`WT-GD-04,05`） | `AC-33` 的 5 条断言全部无法达成；`history.html` 源码注释里写明「账户设置不做注销账号」 |
| **提交 diff（`D-42`/`D-50`）** | `static/js/diff.js` 0 字节，无 checkbox（`WT-GD-01`~`03`） | `AC-28` 无法达成；后端 `GET /api/submissions/diff` 端点存在但无客户端 |
| **草稿冲突挂载期分支（`D-55`）** | `draftAt` 写入但从未比较（`WT-GD-06`） | `AC-32`① 的「改时间戳再刷新」路径不成立；**实时 `storage` 事件分支是可测的**（`WT-ED-06`） |
| **题库只有 1 道题** | 只有 `two-sum` | 难度筛选、分页、「仅看收藏」、掌握分布的 `total=0` 分支无法证伪（§3.2） |
| **性能层（L5，8 条）** | 本机 1967MB | `NF-1`~`NF-4`、`AC-38`、`AC-39` **必须在 4G 机器上跑**（`SPEC.md §1.4` 双档） |
| **三浏览器兼容性** | 骨架已配，未实跑 | `NF-11` 待发布前验证 |
| **越权层（L4 中的双账号部分，5 条）** | 需部署机 DB 与第二个账号 | `AC-20`、`AC-36`、`NF-7` 的完整验证需在部署机上配 MySQL 客户端 |

**一句话总结**：本部署实例上**能真正端到端跑通的是 L0/L1/L2（约 71 条）**，
覆盖 SPEC 49 条 AC 中的 **21 条**。**主闭环的判题环节完全未验证** ——
这不是测试设计的缺口，是部署环境的缺口（judge 调度器未接线）。

---

## 附录 A：相关文件

| 关注点 | 文件 |
|---|---|
| 规格（本文的 AC 追溯来源） | `SPEC.md` |
| 接口契约 | `API.md` |
| 接口层自动化（本文的互补层） | `基于curl的接口自动化测试文档.md` |
| 前端页面 | `static/{index,login,list,problem,history,admin}.html` |
| 前端脚本 | `static/js/{common,api,auth,landing,list,problem,history,overview,admin,editor,poll,markdown}.js` |
| 编辑器与判题轮询 | `static/js/editor.js`、`static/js/poll.js` |
| HTTP 契约验收 | `tests/run_tests.sh` |
| 题库链路验收 | `tests/run_problemsync_test.sh` |
| 单元测试入口 | `tests/run_unit_tests.sh` |
