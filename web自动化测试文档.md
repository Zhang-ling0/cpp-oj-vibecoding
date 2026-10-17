# CppOJ Web 自动化测试文档

> 与相邻文档的分工：
> `API.md` 说「接口应该长什么样」→ `基于curl的接口自动化测试文档.md` 说「怎么反复验证接口真的长那样」；
> **本文说「用户在浏览器里真的能不能用、看到的对不对」**。
> 本文是 `SPEC.md §10.1` 那 49 条 AC 里**只能过浏览器才算达成**的那部分的落地清单。

| 项 | 值 |
|---|---|
| 文档版本 | v1.1（**重跑安全轮**：全文补齐幂等约束，见 §1.5 / §2.5） |
| 生成日期 | 2026-10-15 |
| 被测服务 | `http://82.156.75.119:8080` |
| 契约来源 | `SPEC.md`（v1.9）、`API.md`、仓库 `static/` 实际 DOM 与脚本 |
| 测试层级 | 浏览器端到端（Web UI）+ 少量以浏览器为载体的越权/注入验证 |
| 用例总数 | **225 条**（可执行 154 / 阻塞 40 / 待实现 7 / 人工 24），其中 **86 条为 🔁 有状态用例** |
| 用例计数说明 | v1.1 起本行按 §5 各表**实际行数**统计（v1.0 曾写 142，与实际 221 条不符，属统计口径错误，已订正） |
| 脚本形态 | 用例设计表为主 + Playwright 骨架（§7），可手工执行也可接 CI |
| **重跑要求** | **本层的硬性验收标准：同一套数据连续跑 3 轮，判定结果必须逐条一致。** 见 §1.5 |

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

> **v1.1 新增的三块内容**（若只想读重跑相关部分，看这三处即可）：
> §1.5 [重跑安全总纲](#15-重跑安全幂等性总纲) ·
> §2.5 [账号与数据生命周期](#25-账号数据与浏览器状态的生命周期策略) ·
> §7.5 [幂等性基础设施](#75-幂等性基础设施-helpersjs--global-setupjs-)

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

> 「用例数」一列是 v1.0 的估算值，**未随逐条新增而校准**，仅用于判断各层的工作量比例。
> 精确条数请按 §1.4 的图例在 §5 各表里数：可执行 / 阻塞 / 待实现 / 人工。
>
> **v1.1 补一列更重要的维度** —— 各层的 🔁 有状态用例占比差异极大，
> 这直接决定了哪一层最容易被写坏：

| 层 | 🔁 占比 | 主要风险来源 |
|---|---|---|
| L0 / L1 | 0% | 纯只读，天然可重跑 |
| L2 | **约 1/3** | 收藏 toggle、草稿、分栏比例、临时账号 |
| L3 | **约 2/3** | M08 整层 100% 有状态；M07 集中在配额与轮询恢复类 |
| L4 | 约 1/3 | 双账号造数 |
| L5 | 半数 | 高频提交撞限流 |

> 全文 🔁 合计 **86 / 225 条**。其中最该警惕的不是数量，而是**分布**：
> 高风险的用例几乎都集中在 M06（草稿）、M07/M08（提交）、M11（改密）三处，
> 而这三处恰好也是"看起来像只读、实际在写库"最容易漏判的地方。

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
| 🔁 **有状态** | 该用例会**写状态或读绝对值**，必须按 §1.5 的规则书写才能重复通过；共 **38 条** |

### 1.5 重跑安全（幂等性）总纲

> **这一节的存在理由**：一份"跑一次全绿、跑第二次一半红"的用例表，
> 对回归的价值是**负的** —— 它训练团队养成"红了就手动修数据再跑一遍"的习惯，
> 于是真回归与手工补救混在一起，最终没人再相信报告。
>
> 本文的硬性标准：**同一套数据连续跑 3 轮，判定结果逐条一致**（§8 步骤 5 是它的机械化执行）。

#### 1.5.1 两类"第二次才失败"

| 类型 | 机理 | 典型 | 检测手段 |
|---|---|---|---|
| **A. 残留型** | 第一次用例如同留下了状态，第二次的**前置**不再成立 | `WT-AU-09` 注册 `wt_t1` → 第二次拿到 `1005 用户名已注册` | 同名注册、账号计数漂移 |
| **B. 反转型** | 用例断言的是**动作**（点了一次），而动作是**翻转** | `WT-LS-21` 点收藏 → 第一次 ★ / 第二次 ☆ | toggle 类端点 |

B 类比 A 类更危险：A 类换个用户名就好，B 类的**断言方向**本身就写反了。

#### 1.5.2 本项目已确认的三处"翻转"与"共享"陷阱

这三条已对照源码核实，是本层所有重跑问题的根源：

| # | 事实（源码位置） | 导致的用例问题 |
|---|---|---|
| **①** | **收藏是硬 toggle，且取消收藏会 `DELETE` 掉 `user_problems` 整行**（`src/web/user_api.cpp:161-189`：无行→INSERT favorite=1；有行且 favorite≠0→DELETE；有行且 favorite=0→UPDATE）。前端同样不传状态（`static/js/list.js:327`、`static/js/problem.js:152`）。 | `WT-LS-21`/`WT-PB-09` 点第二次就翻回去；更糟的是**取消收藏会连带删掉那行进度记录**，于是 `WT-LS-17` 的「4 收藏」与 `oj-reset` 造的 4 条收藏基线一起被破坏 —— 一条用例污染后面好几条 |
| **②** | **草稿键不含用户名**：`draft:<slug>` / `draftAt:<slug>`（`static/js/editor.js:24-25`），题目页挂载时**无条件用草稿覆盖模板**（`static/js/problem.js:243-244`）。 | `WT-ED-03`（断言编辑器 == 模板）第二次必挂；且草稿**跨账号共享**，临时账号的草稿会流进下一个用例 |
| **③** | **`WT-AD-04` 断言「共 7 人」，而每个临时账号都往 `users` 插一行**（`oj-reset` 造 7 个，`tools/reset_db.cpp:682-690`）；`GET /api/admin/overview` 的 `userCount` 是实时 `COUNT(*)`（`src/web/admin.cpp:117,245`）。 | 只要有任何一条用例注册过临时账号，`WT-AD-04` 就永久变红，且**再也不会自己恢复** |

#### 1.5.3 六条硬规则

> 写任何一条新用例前，先对照这六条；已有用例中不合规的已在 §5 逐条标注 🔁 并改写。

| 规则 | 要求 | 反例 → 正例 |
|---|---|---|
| **R1 唯一命名** | 任何**新建**的实体（账号 / 探针题 / 提交）名字里必须带本轮 `runId`，格式 `ojt<runId><seq>` | ✗ 注册 `ojt_temp01` → ✓ 注册 `ojt_m9f2a1b01`（`runId = OJ_RUN_ID ?? Date.now().toString(36)`） |
| **R2 归一化到已知态** | **不写"点一次然后断言结果"**，写"把状态调到 X 再断言"。toggle 类端点必须先读后写 | ✗ 点 `#favorite` → 断言 ★<br>✓ `await favoriteTo(page,'two-sum',true)` → 断言 ★ |
| **R3 绝对数→基线相对** | 断言计数前**先取基线**，断言 `after === before ± k`。基线来自同一轮的数据，不来自文档里的字面量 | ✗ `#sub-total` = `共 34 条`<br>✓ `expect(total).toBe(base.total + 1)` |
| **R4 自带数据 + 必收尾** | 破坏性用例**自己造前置数据、自己清理**，不依赖别的用例跑过。且收尾必须执行到 —— 用 `try/finally`，或让 `oj-reset` 兜底 | ✗ "先随便提交一条" → ✓ `await seedSubmission()` → `try{…}finally{await dropAccount()}` |
| **R5 前置自检 + 脏数据清理** | 每轮开始前**快照基线**、**清掉上一轮的残留**（探针题、`sessionStorage['judging:*']`、越界 `draftAt`） | 见 §2.5.4 的 `globalSetup` |
| **R6 配额与顺序隔离** | 会触发限流的用例必须**独占账号 + 独占时间窗**，且排在串行队列的末尾 | ✗ `WT-SB-07` 用 `zhangling` 连发 11 次<br>✓ 用一次性账号，且排最后 + 收尾等窗口滑出 |

#### 1.5.4 三类"读绝对值"的合法化

`§5` 中大量用例断言的是**造数工具的产物**（26 题 / 34 条提交 / 4 收藏 / 7 人）。这些数字本身没错，
错在它们是**全局共享常量**：任何一条写用例都会让它漂移。三种处理，按优先级：

| 方案 | 做法 | 适用 | 代价 |
|---|---|---|---|
| **① 基线相对（首选）** | 用例自己 GET 一遍拿基线，断言 delta 或恒等关系 | `WT-HR-01/02`、`WT-MG-05/06`、`WT-AD-04` | 无 |
| **② 全局重置** | 整轮开始时 `./build/oj-reset`，把基线按构造恢复 | `WT-LS-01/04`、`WT-IN-03`、`WT-MG-01/02/04` | 需要能重启服务；**单轮内后续写用例仍会污染**，故只作"轮前"手段 |
| **③ 挪到写用例之后断言** | 把纯读用例排在所有写用例**之前**（或用另一个从未被写过的账号） | `WT-LS-17`、`WT-PB-21` | 需要固定执行顺序 |

> **规则**：凡标 🔁 的用例，其步骤列里**必须**能读到"基线从哪来"这一句。
> 读不到，就说明这条用例还不能算写完。

#### 1.5.5 二次运行的验收方法

```bash
cd tests/web
npx playwright test --project=chromium                      # 第 1 轮：基线
npx playwright test --project=chromium                      # 第 2 轮：必须与第 1 轮逐条一致
npx playwright test --project=chromium                      # 第 3 轮：同上（3 轮是硬标准，见 §8）
```

判读时看两件事，缺一不可：

1. **判定一致**：每条用例的 pass/fail/skip 与上一轮完全相同（不是"这轮红的更多"）。
2. **基线不漂移**：把三轮的 `#user-count`、题库题数、`ZhangLing` 提交总数并排看，**必须三列相同**。

> 第 2 点比第 1 点更重要。一条用例"第二轮红了"是显性故障；
> "三轮都绿但 `共 7 人` 悄悄变成了 `共 23 人`"是**隐性故障** —— 它让后面所有依赖基线的断言失效，
> 而报告看上去是干净的。

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
>
> ⚠️ **v1.1 补充**：上面那条"临时账号"还不够 —— **临时账号本身必须每轮唯一**（R1），
> 且**用完必须注销掉**（R4），否则 `WT-AD-04` 的「共 7 人」会被永久破坏。完整策略见 §2.5。

### 2.4 造数与重置：`./build/oj-reset`

上面两个账号（`ZhangLing`/`666666`、`admin`/`admin123`）以及 §3.2/§3.3 缺的那批数据，
由仓库内的造数工具一条命令产出 —— 不必手工 SQL：

```bash
make build JOBS=2                 # §0.0：2 核实测机，并发上限 2（一次性编出 oj-reset）
cd /home/ZL/project/cpp-oj-vibcoding   # 必须在项目根目录，见下方「前置条件」

./build/oj-reset --dry-run        # 建议先跑：不连库、不写文件，只打印计划
pkill -f 'build/oj'               # 建议停掉 oj：避免清库期间它往里写
./build/oj-reset                  # 清库 + 重建全部测试数据（约 2 秒，峰值内存 ~13MB）
make run                          # 重新拉起 oj —— 不重启就读到旧题库缓存
```

#### 前置条件（4 条，缺一条就出问题）

| # | 条件 | 缺了会怎样 |
|---|---|---|
| 1 | **CWD = 项目根目录** | ⚠️ **换目录不会报错，只会静默退化** —— 详见下方实测 |
| 2 | **mysqld 在跑** | `oj-reset` 自己**不拉起** mysqld，只有 `make run` 会（`Makefile:run`） |
| 3 | **11 张表已存在** | 工具只做 `DELETE`、**不建表** —— 建表是 `./build/oj` 启动时的职责（`src/main.cpp:66`，SPEC §2.1-A）。⚠️ 所以 `db/reset.sql` 之后**不能**直接接 `oj-reset`，中间必须先起一次 `./build/oj` 把表建回来 |
| 4 | **跑完重启 `./build/oj`** | 它持有进程内题库缓存，不重启列表页/题目页仍是旧题库。工具结束时会自己打印这句提醒 |

> 「停掉 oj」是**建议不是强制** —— 工具代码里没有任何检查，文件头写一句而已。
> 目的是别在清库的同一时刻让 oj 往里写。停不停它都能跑完。

#### ⚠️ 条件 1 的实测：跑错目录是**静默退化**，不是报错

`oj-reset` 默认读 `.env`，但读不到时**只用内置默认值继续跑**，不退出
（`src/common/config.cpp:115` 的 `read_env_file`：缺 `.env` 只记 `kWarn`，
设计意图见注释——路线 B 的 compose environment 本来就绕过这个文件）。

实测（`cd /tmp && /home/.../build/oj-reset --dry-run`）：

```
[oj-reset] 配置 .env；题库目录 ./problems
[oj-reset] --dry-run：不连库、不写任何东西。计划：清 11 张表 → …      ← exit=0，看不出任何异常
```

更麻烦的是**连警告都看不到**：`oj-reset` 从不打印 `load_config` 的 diagnostics，
且 `init_logging()` 是在 `load_config()` **之后**才调的（`tools/reset_db.cpp:1093` 与 `:1099`），
所以那句 `未找到配置文件 … 请确认启动时的工作目录是项目根目录` **既不在 stdout、也不在 `logs/app.log`**
（实测 `grep -c 未找到配置文件 logs/app.log` = 0）。

而内置默认值恰好是 `mysql_host=127.0.0.1` / `mysql_user=oj` / `mysql_db=oj`
（`include/common/config.h:69,71,73`），**与本机 `.env` 相同** —— 于是它会连上正确的库、
却把 `problems_dir` 解析成 `<错误目录>/problems`，造出一份"库对了、题库文件写错地方"的局面。

> 这正是本文反复强调的那一类故障：**不是"红了"，是"没红但结果不对"**。
> 所以跑之前先 `pwd` 确认一下，比事后查日志有用。

#### 其余参数（`--help` 里有，本文原先漏了）

| 参数 | 用途 |
|---|---|
| `--env <路径>` | 换一份配置读。默认 `.env`；跑回归想连测试库而不动开发库时用 |
| `--problems-dir <目录>` | 覆盖题库目录。默认取 `.env` 的 `OJ_PROBLEMS_DIR`（`./problems`）；想验证种子题文件生成、又不碰正式题库目录时用 |

造出来的东西与本文档的对应关系：

| 造的 | 解锁的用例 |
|---|---|
| 26 道题（easy 13 / medium 8 / hard 5，11 个标签） | `WT-LS-04` 难度筛选、`WT-LS-09` 分页、`WT-LS-22` 仅看收藏、`WT-MG-01~07` 掌握分布三档齐全 |
| 7 个账号（`admin` 1 + 普通 6，其中 `alice01`/`erin05` 零提交） | `WT-TB-03`、`WT-AD-04`（共 7 人）、`WT-AD-05`（通过率显示 —） |
| `ZhangLing` 34 条提交：AC/WA/TLE/MLE/RE/CE/JE + 2 条 pending | `WT-RS-01~10`、`WT-HR-01/02/05~16` 的渲染分支（§3.3 路径 B） |
| 4 条可直接注入 Cookie 的会话（含 1 条已过期） | `WT-AC-06` 改密踢人、`WT-ED-14`/`WT-MG-11` 会话过期 |
| 4 题收藏 + 6 题进度行 | `WT-LS-21`、`WT-PB-09` |
| 进度三态样本：`two-sum`（已通过）/ `rotate-left`（只有 CE → 尝试过）/ 5 道从未提交的题 | `WT-LS-20` |

> ⚠️ **三点必须知道**：
> ① 执行后**要重启 `./build/oj`**（它持有进程内题库缓存）；
> ② 两条 `pending` 行在 **judge 接线后**会被 D-20 重新入队判掉，判题类回归前请重跑一次本工具；
> ③ **`--regen-problems` 不是可有可无的**：默认行为是**已存在的种子题文件一律不覆盖**
>    （`reset_db.cpp:1136-1142`，设计意图是「真人在录的题不该被造数工具改掉」）。
>    所以改了 `tools/reset_db.cpp:217-268` 的 `kSeeds`（题目/难度/标签）之后，
>    **不加这个参数，你改的是代码、跑出来还是旧题库** —— 而且不报错，看不出来。
>
> `tests/run_problemsync_test.sh` 原先写死"题库里只有 1 道题"的 4 条计数断言已改成
> 基线相对（题库多大都能跑），因此两套造数互不打架。

### 2.5 账号、数据与浏览器状态的生命周期策略

> 承接 §1.5。本节把六条硬规则落成本项目的**具体做法**，是 §7 脚本骨架的直接依据。

#### 2.5.1 三类账号

| 类别 | 账号 | 允许的用例 | 禁止 |
|---|---|---|---|
| **基线账号** | `ZhangLing`、`admin`、以及 `oj-reset` 造的 `alice01`~`erin05` | **只读**：渲染、筛选、分页、排序、断言绝对值 | 任何写操作（提交 / 删除 / 改密 / 注销 / **收藏 toggle**） |
| **一次性账号** | `ojt<runId><seq>`，每轮现造 | 所有写用例 | —— |
| **对手账号** | `ojt<runId>b`（越权用例里的"用户 B"） | `WT-SX-02`~`06` | —— |

> ⚠️ **`ZhangLing` 出现在写用例里的三处例外是有意为之**，因为要验的就是"同一个人的连续行为"：
> `WT-LS-21`/`WT-PB-09` 验收藏**翻转**、`WT-HR-12`~`16` 验删除重跑。
> 但这四条**必须用归一化写法（R2）并在自己的收尾里把状态复位**，否则它们会污染 `WT-LS-17`/`WT-LS-22` 的「4 收藏」基线。

#### 2.5.2 一次性账号的完整生命周期

```
造 → 登录 → 干活 → 注销（DELETE /api/auth/account）
                     └─ 这一步同时清掉 sessions / submissions /
                        submission_case_results /
                        submission_compile_errors / user_problems
                        （5 张表全部 ON DELETE CASCADE）
```

**为什么注销是首选收尾手段**：`DELETE /api/auth/account` **服务端已经实现**
（`src/web/routes.cpp:983,392-420` → `src/web/user_api.cpp:111-127`，`DELETE FROM users WHERE id=?`），
虽然前端没有入口（§3.4），但测试可以直接调。
它一次清干净 5 张表，正是 `WT-AC-12` 要验的那条级联链 —— **收尾逻辑和被测逻辑是同一条**，不需要另写清理代码。

> **例外**：`WT-AC-10`~`14` 本身就是验注销的，它们的"收尾"就是注销本身，不重复调用。
> `WT-AU-21`（验 bcrypt）需要**读库**验 `password_hash`，注销前验。

#### 2.5.3 浏览器侧状态：每次都用全新的 context

三条本地状态**不随账号走**，必须显式管理：

| 状态 | 键 | 污染后果 | 处置 |
|---|---|---|---|
| `localStorage` | `draft:<slug>`、`draftAt:<slug>` | `WT-ED-03` 第二轮起失效；草稿**跨账号泄漏**（§1.5.2 ②） | 每条用例 `browser.newContext()`（Playwright 默认已是独立 context）；**用例会主动写草稿的，还必须在 `beforeEach` 里 `removeItem`** |
| `localStorage` | `split:problem` | `WT-PB-15` 断言「≈70」时起点已变 | 用例先写死起点（如 `setItem('split:problem','50')`）再操作 |
| `sessionStorage` | `judging:<slug>` | 上一轮提交中断会留下一个**永远转圈**的幽灵轮询，`WT-ED-*` 打开同题时会看到"评测中…" | `globalSetup` 每轮清一次；提交类用例的 `finally` 里清 |

#### 2.5.4 每轮开始前：前置自检与脏数据清理

`globalSetup` 干四件事（详见 §7.7）：

```
① 快照基线：题库题数 / users 数 / ZhangLing 提交数 / ZhangLing 收藏数  → 存 baseline.json
② 清残留： sessionStorage['judging:*']（每个新 context 本来就是空的，此项针对复用 context 的手工执行）
③ 清探针： problems/ 下的 xss-probe.json / img-probe.json 与库中对应行（§2.5.5）
④ 校验：  ①的四个数与上一轮的 baseline.json 比对，不一致就在报告里打醒目 WARN
```

> ④ 是"隐性故障"的守门人。`WT-AD-04` 断言 7 人时**不**该靠文档里的字面量，
> 但**轮次之间**的数字必须能被自动发现已经漂了 —— 否则没人知道基线什么时候坏的。

#### 2.5.5 探针题的强制清理

`WT-PB-03`/`04`/`WT-SX-14` 需要往题库里加含 `<script>` / `<img>` 的坏题。文档 §5-M05 已写"测完删"，
但**"测完"在回归里是不可靠的** —— 用例失败、中途 Ctrl-C、机器断电，都会留下残骸。
残骸的直接后果：题库题数 26 → 27，`WT-LS-01`/`WT-LS-04`/`WT-MG-06`/`WT-IN-03` 的计数全部漂移。

处置：

| 阶段 | 动作 |
|---|---|
| 前置 | `globalSetup` 检查并删除 `problems/xss-probe.json`、`problems/img-probe.json` 及库中 `slug LIKE 'oj_probe%' OR slug LIKE '%-probe'` 的行（`oj-reset` 已有 `oj_probe%` 的清理逻辑，`tools/reset_db.cpp:997-1005`，web 侧沿用同一命名） |
| 用例内 | `test.afterAll` 里删文件 + 提示重启；**不依赖它删干净** |
| 兜底 | 下一轮 `globalSetup` 再删一次（**幂等，删不存在的也返回成功**） |

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

> **这张表是"裸库"基线。** 需要 §3.2/§3.3 那些用例可跑时，先 `./build/oj-reset`（见 §2.4）：
> 它把题库补到 26 道、造好 7 个账号与 40 条提交，耗时约 2 秒。
> 造数后的实测值：题库 26 道（easy 13 / medium 8 / hard 5）、用户 7、
> 提交 40（AC 21 / WA 5 / TLE 3 / CE 3 / MLE 2 / RE 2 / failed 2 / pending 2）。

### 3.2 题库只有 1 道题带来的用例受限

以下断言**在本实例上无法证伪**，必须在题库补足后执行，**不得记为"通过"**：

| 受限的用例 | 原因 | 补数办法 | `oj-reset` 后 |
|---|---|---|---|
| `WT-LS-04` 难度筛选（简单/困难） | 只有 medium 一档 | 录入 easy/hard 题后 | ✅ 26 题（13/8/5） |
| `WT-LS-09` / `WT-HR-08` 分页 | 只有 1 条数据，造不出第 2 页 | 录入 ≥ 25 题后 | ✅ 26 题 / 34 条提交 |
| `WT-LS-06`「仅看收藏」 | 需先收藏才 1 条，测不出过滤 | 同上 | ✅ 已收藏 4 题 |
| `WT-MG-03` 难度卡三项齐全 | easy/hard 的 `total=0`，整行不渲染 | 同上 | ⚠️ 反过来：三档都有题，该"不渲染"分支需裸库才验 |
| `WT-MG-05` 标签表排序 | 3 个标签可验排序，但样本太小 | 同上 | ✅ 11 个标签 |
| `WT-RS-*` 全部 | 无提交数据可渲染 | 见 §3.3 | ✅ 40 条提交（6 种 verdict + failed/pending） |

> **难度筛选到底需要几道题？** 这是本表里最容易被低估的一条：
>
> | 题库形态 | 简单 | 中等 | 困难 | 难度筛选这条用例能证到什么 |
> |---|---|---|---|---|
> | 只有 1 道 medium | 0 | 1 | 0 | **证不了**。三档里两档恒为 0，与「筛选参数根本没生效」「后端忽略了 difficulty」「结果集为空」三种故障的观测完全相同 |
> | 每档各 1 道 | 1 | 1 | 1 | 只能证「分了三档」，证不了**筛选生效** —— 任何返回 1 条的实现都能蒙对 |
> | 每档 ≥2 道，且三档之和 = 全量 | ≥2 | ≥2 | ≥2 | 才算真验：每档命中数互不相同、且并集等于全量，恒等返回与恒空返回都排除得掉 |
>
> `oj-reset` 造的是 easy 13 / medium 8 / hard 5（合计 26），正是上表最后一行；
> 而 26 > 每页 20 顺带把分页（`WT-LS-09` / `WT-HR-02`）也解掉了。

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
| M07 提交与判题 | `WT-SB-01` ~ `WT-SB-11`、`WT-SB-13`（`WT-SB-12` 不阻塞） | 12 |
| M08 结果面板 | `WT-RS-01` ~ `WT-RS-10` | 10 |
| M09 历史记录 | `WT-HR-01`、`WT-HR-02`、`WT-HR-05`、`WT-HR-06`、`WT-HR-09`、`WT-HR-12`~`16` | 10 |
| M10 掌握分布 | `WT-MG-01`、`WT-MG-02`、`WT-MG-04` | 3 |
| M05 最近提交 | `WT-PB-08` | 1 |

> ⚠️ **v1.1 更正**：原表把 M09 记作 5 条，漏了 `WT-HR-12`~`16`（删除与重跑，`AC-29`）——
> 它们同样需要 judge 产出 `failed`/`JE` 记录才能验"重跑按钮只对 failed 渲染"。
> `WT-SB-13`（配额按账号隔离）为 v1.1 新增，随 `WT-SB-06`/`07` 一起等限流接线。
>
> 另外提醒一句：**这 36 条阻塞用例里有 23 条是 🔁**（M08 整层 10 条 + M09 的 6 条 + M07 的 7 条）。
> 也就是说 L3 一旦接线，跑第一遍就会往库里写进几十条提交、
> 删掉若干条记录，第二遍开始 `WT-HR-01`/`WT-LS-17` 就未必有基线可依了。
> **接线当天请先 `./build/oj-reset` 再开跑，且严格按 §8 的三轮流程验收。**

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
>
> ⚠️ **v1.1 提醒（重跑相关）**：三个键**都不含用户名或 userId**，因此
> **同一浏览器 profile 内草稿跨账号共享**（源码 `static/js/editor.js:24-25`）。
> 写用例时的直接后果：`WT-AC-12`（注销清草稿）实际清的是**整个 profile** 的草稿，
> 与 `WT-ED-*` 同跑会互相干扰 —— 所以 `WT-AC-12` 必须跑在**专用 context** 里。

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
> - 需要登录的用例，默认用 `zhangling`；**删除/改密/注销类必须换一次性账号**（§2.5）。
> - **🔁 标记的用例必须按 §1.5 的 R1~R6 书写**：
>   步骤里要能看出「数据从哪来」（唯一命名 / 基线快照 / 自带造数）与「跑完怎么复位」，
>   断言里不能出现"点一次就应该是 X"这种把动作当结果的写法。
> - **基线数字（26 题 / 34 提交 / 4 收藏 / 7 人）的读法**：
>   出现在 🔁 用例里的一律按基线相对处理（R3）；出现在纯读用例里的一律要求"轮前跑过 `./build/oj-reset`"。

---

### M01 落地页

对应 `SPEC.md §7.1` 落地页一节（v1.10 新增）。核心契约：**纯展示、不含任何个人数据**。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-IN-01` | `/` 重定向到落地页 | 匿名 | 打开 `http://82.156.75.119:8080/` | HTTP `200`，`Content-Type: text/html`，页面含 `.hero__title`，无 `404` | — | 🟢 |
| `WT-IN-02` | 六页全部可达 | 匿名 | 依次打开 `index/login/list/problem?slug=two-sum/history/admin`.html | 全部 `200`；字节数与 `static/` 一致（18136/5761/4847/5983/6589/4497） | — | 🟢 |
| `WT-IN-03` | 实时数字在 API 可用时显示 | 匿名 + **轮前已 reset** | 打开落地页，等 `#live-stats` 可见 | **`#stat-problems` = `GET /api/problems?size=1` 的 `data.total`**，`#stat-algo`/`#stat-source` 同理取 `/api/tags` 的分类计数；裸库时为 `1`/`3`/`1`，reset 后为 `26`/`8`/`3`（§3.1）；**先 `hidden` 后显形** | — | 🟢 🔁 |
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
| `WT-AU-09` | 注册后自动登录 | 匿名 + 唯一名 | 切注册 tab，填 **`ojt<runId>01` / `pass1234`** → 提交 | 账号创建并直接进入已登录态，跳 `/list.html`；顶栏回显 `ojt<runId>01`。**收尾：注销该账号**（§2.5.2），否则 `WT-AD-04` 的用户数漂移 | `AC-1` | 🟢 🔁 |
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
| `WT-AU-21` | `bcrypt` 闭环（不以"没报错"为证） | 一次性账号 | 注册 `ojt<runId>02` → 登出 → 重新登录 | 登录成功；**且**库中 `password_hash` 为 `$2b$10$` 开头、共 60 字符。⚠️ **必须在注销前查库**：注销是级联硬删（§2.5.2），查完就没了。**收尾：注销** | `AC-49` | 🔵 🔁 |
| `WT-AU-22` | **重复注册被拒**（幂等性守卫） | 匿名 + 唯一名 | 用 `ojt<runId>03` 注册成功 → **原样再注册一次** | 第二次 `409/1005`（`src/web/auth_api.cpp:162-165`），页面显示冲突文案且**停在注册页**；`users` 表该用户名仍只有 1 行 | `AC-1` | 🟢 🔁 |

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
| `WT-LS-01` | 列表加载 | 匿名 + 轮前已 reset | 打开 `/list.html` | **`#result-hint` 的 N == `GET /api/problems?size=1` 的 `data.total`**（R3，reset 后 26）；**造数后** `#rows` 有 20 行（每页 20）；加载中为 `加载中…` | `AC-2` | 🟢 🔁 |
| `WT-LS-02` | 行内七列齐全 | 匿名 | 检查 `#rows > tr` | 七列：题名/slug、难度、用例数、限制、`我的进度`、收藏；标签 chip 带 `tag--algorithm`/`tag--source` | `AC-2` | 🟢 |
| `WT-LS-03` | 限制列口径文案 | 匿名 | 读 `.mono.muted[title]` | title 含 `CPU 时间上限 / 地址空间上限（AS 口径）`；文本形如 `3000ms · 128MB` | `AC-44` | 🟢 |
| `WT-LS-04` | 难度筛选 | 匿名 + 轮前已 reset | 依次点 `button.seg__btn[data-difficulty]` 的 空/简单/中等/困难 | **按基线相对（R3）**：先取 `?size=1` 的 `data.total` = 全量 N，再断言四档 `easy+medium+hard === N` **且三档互不相同**（排除"筛选参数没生效"与"恒等返回"）；reset 后为 26 = 13+8+5 | `AC-2` | 🟢 🔁 |
| `WT-LS-05` | 关键词筛选（含 debounce） | 匿名 | `#f-keyword` 填 `两数` → 等 400ms；再填 `two-sum` | 均命中 1 题；**请求在停止输入 300ms 后才发出**（debounce） | `AC-2` | 🟢 |
| `WT-LS-06` | 标签多选为 AND | 匿名 | `#f-tags` 同时点 `数组` 与 `哈希表` | 两个 chip 都被选中（`.chip.is-on`）；**造数后**命中 3 题（交集小于 `数组` 的 20 题）；另需一组**交集为 0** 的标签（如 `二分查找` ∧ `字符串`）验空态 | `AC-2` | 🟢 |
| `WT-LS-07` | 来源标签筛选 | 匿名 | 点 `chip[data-tag="高频面试题"]` | **造数后**命中 7 题（裸库时为 1 题） | `AC-2` | 🟢 |
| `WT-LS-08` | 排序切换 | 匿名 | 依次选 `#f-sort` 的 默认/精选优先/由易到难/由难到易 | 每次重取列表；`精选优先` 把 `two-sum`（`isFeatured=true`，`sort_no=10` 最小）排首 —— 造数后共 4 道精选 | `AC-2` | 🟢 |
| `WT-LS-09` | 分页 | 造数后满足（26 > 20） | 翻到第 2 页再翻回来 | `#pager` 出现 `第 1 / 2 页`（首页 20 题、第 2 页 6 题）；顺序稳定、不重不漏；`aria-current="page"` 正确 | `AC-2` | 🟢 |
| `WT-LS-10` | 分页参数边界（不依赖数据量） | 匿名 | 地址栏改 `?page=0` / `?size=1000` 直连 API | 前者 `400/1001 field=page`；后者 `200/0` 且 `data.size` 回显 `100`（夹取） | `AC-2` | 🟢 |
| `WT-LS-11` | 非法枚举值 | 匿名 | `?difficulty=nope` | `400/1001`，`data.field="difficulty"` | `AC-2` | 🟢 |
| `WT-LS-12` | 空态 + 一键清除 | 匿名 | `#f-keyword` 填不存在的词 | `#rows` 内 `div.empty`：`没有匹配的题目，试试放宽筛选条件` + 「清除筛选」按钮；点它后恢复 | `AC-2` | 🟢 |
| `WT-LS-13` | 「清除筛选」按钮 | 匿名 | 设好任一筛选后点 `#clear-filters` | 所有筛选复位，`#rows` 恢复全量 | `AC-2` | 🟢 |
| `WT-LS-14` | 匿名不得按进度筛选 | 匿名 | 检查 `#f-progress` 与 `#f-favorite` | 两者 `disabled` 且 `title="登录后可按进度筛选"`；`#personal-hint` 可见 | `AC-25` | 🟢 |
| `WT-LS-15` | 匿名 `progress` 参数被后端拒 | 匿名 | 直连 `?progress=solved` | `400/1001` | `AC-25` | 🟢 |
| `WT-LS-16` | 概览条：未登录不隐藏 | 匿名 | 打开 `/list.html` | `#overview` **不隐藏**，改为渲染引导文案 + 登录按钮；**题库表格照常渲染** | `D-38` | 🟢 |
| `WT-LS-17` | 概览条：登录态四档 + 最近 3 条 | 登录 + **轮前已 reset** | 打开 `/list.html` | `.overview__hello` = `你好，ZhangLing`；`.stat` **四个**、按「值 `<b>` 在前、标签 `<span>` 在后」渲染；**三个难度条全部渲染**。⚠️ 数值按基线相对（R3）：四项 stat 与 `GET /api/me/overview` 的对应字段**逐项相等**，难度条与 `byDifficulty` 逐项相等 —— **不要写死 `12/26`/`21 尝试中`/`4 收藏`/`34 提交`**，这些是 `oj-reset` 的产物，`WT-SB-*`/`WT-HR-*` 一提交就漂 | `AC-24` | 🟢 🔁 |
| `WT-LS-18` | 概览条不随分页/筛选重取 | 登录 | 翻页 + 改关键词 | `/api/problems` 请求数增加；`/api/me/overview` **始终只有 1 条** | `AC-24` | 🟢 |
| `WT-LS-19` | 零数据新用户 | **一次性账号** | 新账号 `ojt<runId>04` 打开 `/list.html` | 进度条渲染为空槽（0%）而非空白；最近提交区显示 `.overview__empty`：`还没有提交记录，去做第一题 →`。**收尾：注销** | `D-38` | 🟢 🔁 |
| `WT-LS-20` | 进度推导三态 | 登录 + 轮前已 reset | 打开 `two-sum`（有 AC）/ `rotate-left`（只有 CE）/ `count-spaces`（从未提交） | 分别显示 `.prog--solved` 已通过 / `.prog--attempted` 尝试过 / `.prog--none` 未开始；三态的题目数分别等于 `/api/me/overview.byDifficulty` 推导出的 attempted/solved 差（reset 后 12 / 9 / 5） | `AC-11` | 🟢 🔁 |
| `WT-LS-21` | **收藏切换 → 归一化到目标态** | 登录（`ZhangLing`） | 先读当前态，再**点到位**：`star.is-on` 为 false 则点、true 则点两次 | 到达 **★ + `.is-on` + `aria-pressed="true"`**；刷新后保持。**收尾：`favoriteTo(two-sum, true)` 把基线复位** ⚠️ 服务端是硬 toggle，取消收藏会 `DELETE` 整行 `user_problems`（`src/web/user_api.cpp:173-175`），不归位会连带破坏进度行与 `WT-LS-17` 的收藏数 | `AC-12` | 🟢 🔁 |
| `WT-LS-22` | 「仅看收藏」 | 登录 + 轮前已 reset | 取基线 `GET /api/problems?favorite=1` 的 `total`，再勾选 `#f-favorite` | 显示行数 **== 基线 total**（reset 后 4 道），且每一行的 slug 都在基线集合里。⚠️ 必须基线相对：`WT-LS-21` 若未归位，这里会从 4 变 3 | `AC-12` | 🟢 🔁 |
| `WT-LS-23` | 列表→详情跳转 | 匿名 | 点 `#rows` 内 `a[href^="/problem.html"]` | 落到 `/problem.html?slug=two-sum` | `AC-2` | 🟢 |
| `WT-LS-24` | 不存在页面 404 | 匿名 | 打开 `/nope.html` | HTTP `404`，JSON 包络 `code=1004` | — | 🟢 |
| `WT-LS-25` | **toggle 归位守卫**（幂等性自检） | 登录 | 对同一题连续 `favoriteTo(x, true)` 两次 | 第二次调用**不发请求**（已是目标态）；库里 `user_problems` 仍只有 1 行、`favorite=1`。这是 `WT-LS-21`/`WT-PB-09` 能安全反复运行的**前提** | `AC-12` | 🟢 🔁 |

---

### M05 题目详情与题面渲染

对应 `P3`、`AC-3`、`AC-25`、`AC-26`、`AC-27`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-PB-01` | 题面基本信息 | 匿名 | 打开 `?slug=two-sum` | `#p-title`=`两数之和`；`#p-difficulty.diff--medium`=`中等`；`#p-tags` 含 `数组`/`哈希表`/`高频面试题`；`#p-limits` 的 `dataset.memKb`=131072 | `AC-25` | 🟢 |
| `WT-PB-02` | Markdown 渲染 | 匿名 | 查看 `#p-markdown` | 标题(`h2/h3`)、无序列表(`ul>li`)、行内代码、行内 `<code>`、代码块 `<pre><code>` 均正确渲染 | `AC-3` | 🟢 |
| `WT-PB-03` | `<script>` 被 DOMPurify 清除 | 构造 | 让题面含 `<script>alert(1)</script>`（见下） | `#p-markdown` 内**无** `<script>` 元素；无弹窗 | `AC-3` | 🔵 🔁 |
| `WT-PB-04` | `img` 标签被过滤（`[D-48]` 题面禁图片） | 构造 | 题面含 `![](x.png)` | 渲染结果**无 `<img>` 元素** | `AC-27` | 🔵 🔁 |
| `WT-PB-05` | 提示 tab 仅在有 `hint_md` 时渲染 | 匿名 | 观察 `#tab-hint` | `two-sum` 有 `hint_md` → tab **可见**、`#pane-hint` 可点开 | `AC-26` | 🟢 |
| `WT-PB-06` | 提示免费无限看 | 匿名 | 反复点开/收起 `#tab-hint` 共 5 次 | 每次都正常展开；**无任何次数提示、无解锁流程** | `AC-26` | 🟢 |
| `WT-PB-07` | 无 `hint_md` 的题不渲染该 tab | **需无提示的题** | 打开该题 | `#tab-hint` 在 DOM 中**不存在**（不是 `hidden`） | `AC-26` | 🔵 |
| `WT-PB-08` | 「我的提交」tab | 登录 | 点 `#tab-recent` | 无数据时显示 `还没有提交记录`；有数据时最多 5 行 `button.recentrow[data-sub]` | `D-42` | 🔴 |
| `WT-PB-09` | 收藏星标：登录态 | 登录（`ZhangLing`） | 用 `favoriteTo(two-sum, <目标态>)` 把星标调到目标态，再点一次 | 描边态 `☆` ⇄ 实心 `★`，`aria-pressed` 同步翻转；刷新后保持。**收尾：归位回 ★**（与 `WT-LS-21` 共用同一个 `two-sum` 基线，见 §1.5.2 ①） | `D-43` | 🟢 🔁 |
| `WT-PB-10` | 收藏星标：匿名点按跳登录 | 匿名 | 点 `#favorite` | **跳转** `/login.html?redirect=%2Fproblem.html%3Fslug%3Dtwo-sum`（与"提交"的内联弹窗不同） | `D-43` | 🟢 |
| `WT-PB-11` | 匿名点提交**不跳转**而是内联弹窗 | 匿名 | 点 `#submit` | `#submit` 文案为 `登录后提交`；**URL 不变**；`#inline-login` 由 `hidden` 转可见 | `D-43` | 🟢 |
| `WT-PB-12` | 内联登录成功后续做 | 匿名 | 在 `#inline-login` 填账号密码 → `#inline-go` | 登录成功；页面**不刷新**；`#submit` 变回 `提交`；`#p-recent`/`#p-next` 被重新拉取填充 | `D-43` | 🟢 |
| `WT-PB-13` | 登录后**不自动补收藏** | 匿名（**前置：该题当前为未收藏态**） | 读基线 → 若已是收藏态则先 `favoriteTo(slug,false)` → 点 `#favorite` → 登录 → 登录后 | **不**自动写入收藏（避免隐式写操作）；`GET /api/problems/{slug}` 的 `isFavorite` 仍为 false。**收尾：`favoriteTo(slug, 基线值)`** | `D-43` | 🟢 🔁 |
| `WT-PB-14` | 题目不存在 | 任意 | 打开 `?slug=no-such-problem` | `#result` 渲染 `div.empty`：`题目不存在或已从题库移除` + 返回题库按钮；**`#editor` 与 `#submit` 被隐藏** | — | 🟢 |
| `WT-PB-15` | 分栏比例可调且持久化 | 登录 | **先写死起点** `localStorage['split:problem']='50'` → 拖 `.splitter` 到约 70% → 刷新 | `localStorage['split:problem']` ≈ `70`；刷新后比例恢复 | — | 🟢 🔁 |
| `WT-PB-16` | 分栏比例钳制在 25–75 | 登录 | **先写死起点 `'50'`** → 疯狂向右拖到底 | 比例停在 `75`，**不越界**。**收尾：`removeItem('split:problem')`**（否则 `WT-PB-15`/`17` 的起点被污染） | — | 🟢 🔁 |
| `WT-PB-17` | 分栏键盘可达 | 登录 | **先写死起点 `'50'`** → `.splitter` 聚焦后按 `←`/`→` | 每次 ±2%，写回 `localStorage`。**收尾：`removeItem`** | — | 🟢 🔁 |
| `WT-PB-18` | 视口宽度下限 | 匿名 | 视口缩到 `1280×800` | 左右分栏不塌陷；表格与结果面板不溢出 | §1.4 | 🟢 |
| `WT-PB-19` | 下一题建议卡：匿名不渲染 | 匿名 | 打开 `?slug=two-sum` | `#p-next` 保持 `hidden`；响应体中**不含** `nextRecommendation` 键 | `AC-25` | 🟢 |
| `WT-PB-20` | 下一题建议卡：登录后渲染三要素 | 登录 + 轮前已 reset | 打开 `?slug=two-sum` | `#p-next` 可见，含 `<b>下一题建议 · 中等</b>`、可点跳转的题名链接、**理由字符串**。⚠️ **不写死推荐哪道题** —— 推荐随进度变化（`WT-PB-24` 正是在验这一点），用 `toBeVisible()` + 结构断言，**不写死 slug 与难度** | `AC-34` | 🟢 🔁 |
| `WT-PB-21` | 理由字符串格式与主标签一致 | 登录 + 轮前已 reset | 取 `GET /api/problems/two-sum` 的 `nextRecommendation`，比对 DOM 理由 | DOM 理由与响应体 `reason` **逐字相同**；且匹配 `^「.+」标签你通过 \d+/\d+，本题尚未通过$`。⚠️ **不写死 `「数组」标签你通过 0/1`** —— 那是 reset 后的快照，任何一次提交都会让它变；正确写法是取 `reason` 里解析出的标签名与分子分母，去 `/api/me/overview.byTag` 里找同名项比对**数值相等** | `AC-34` `AC-46` | 🟢 🔁 |
| `WT-PB-22` | 理由不可由前端编造 | 登录 | 对比 DOM 理由与服务端 `reason` | **逐字相同**（理由由服务端算好，前端不拼装） | `D-59` | 🟢 |
| `WT-PB-23` | 全部通过后改为完成度文案 | **需全通过** | 把所有题 AC 掉 | `#p-next` 显示 `题库已全部通过` + `想扩充题库？编辑 problems/*.json 后重启服务`；**不显示**推荐 | `AC-34` | 🔵 🔁 |
| `WT-PB-24` | 推荐随进度变化 | **需 ≥2 题** | AC 掉当前推荐题后回到本页 | 推荐结果**发生变化**。**收尾：`oj-reset` 复位进度**（本条会永久改变 `ZhangLing` 的通过集，不复位则 `WT-PB-21`/`WT-LS-20` 全线漂移） | `AC-34` | 🔵 🔁 |

> **构造题面的做法（`WT-PB-03`/`04`）**：改题必须走「改 JSON → 重启」（`D-30`），
> 在**部署机**上加 `problems/xss-probe.json`（`statement_md` 里放 `<script>` 与 `![](x.png)`），
> 重启后测，**测完删文件并删库行**。参照 `tests/run_problemsync_test.sh` 的探针题隔离做法。
> **不要改 `two-sum.json`。**
>
> ⚠️ **v1.1：这两条现在是 🔁。** "测完删"在回归里靠不住 —— 用例失败、中途 Ctrl-C、机器断电都会留下残骸，
> 而残骸会让题库从 26 变 27，`WT-LS-01`/`WT-LS-04`/`WT-MG-06`/`WT-IN-03` 的计数**全线漂移且不再自动恢复**。
> 因此清理**不能只靠用例的 `afterAll`**，必须由下一轮的 `globalSetup` 兜底（§2.5.5）：
> 启动时无条件删 `problems/*-probe.json` 与库中 `slug LIKE '%-probe'` 的行，**删不存在的也算成功**。

---

### M06 编辑器与草稿

对应 `P3`、`D-27`、`D-28`、`D-44`、`D-55`、`AC-4`、`AC-32`。

> ⚠️ **本模块是全文重跑风险最高的一层**，原因是两条已核实的实现事实：
> ① 题目页挂载时**无条件**用 `localStorage['draft:<slug>']` 覆盖模板（`static/js/problem.js:243-244`）；
> ② 草稿键**不含用户名**（`static/js/editor.js:24-25`），**跨账号共享**。
>
> 于是：`WT-ED-03` 只要在本模块任何一条用例之后运行就必挂 —— 第一次跑是绿的，第二次跑红的。
> **本层所有用例统一约定：`beforeEach` 先 `removeItem('draft:'+slug)` 与 `removeItem('draftAt:'+slug)`，
> 且断言模板的用例必须在删完之后、输入之前执行。**

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-ED-01` | Ace 编辑器加载 | 匿名 | 打开 `?slug=two-sum` | `#editor` 存在且带类 `ace_editor`；`#editor-warn` 保持 `hidden`（未降级） | `AC-4` | 🟢 |
| `WT-ED-02` | C++ 语法高亮生效 | 匿名 | 观察编辑器 | token 被着色（`ace_keyword`/`ace_string` 等 class 存在） | `AC-4` | 🟢 |
| `WT-ED-03` | 预置模板 | 匿名 + **草稿已清** | **删掉 `draft:two-sum` 与 `draftAt:two-sum`** → 首次打开题目 | 编辑器内容 == `#editor[data-slug]` 对应的 `codeTemplate`，含 `class Solution` 骨架。⚠️ **必须先删草稿**：挂载时草稿优先级高于模板（`problem.js:243-244`），不删则本条只第一次跑能过 | `D-28` | 🟢 🔁 |
| `WT-ED-04` | 草稿自动保存（800ms debounce） | 任意 + **草稿已清** | 在编辑器里输入 `// my-draft-marker` → 等 1s | `localStorage['draft:two-sum']` 含该标记；**同时** `localStorage['draftAt:two-sum']` 为数字串。**收尾：删两个键** | `AC-4` | 🟢 🔁 |
| `WT-ED-05` | 刷新后草稿恢复 | 任意 + **草稿已清** | 输入标记 → 等 1s → 刷新 | 编辑器内容仍是草稿（**不是**模板）。**收尾：删两个键** | `AC-4` | 🟢 🔁 |
| `WT-ED-06` | **多标签页冲突（实时分支）** | 任意 + **草稿已清** | 同 context 开 A/B 两页访问同一题（两页挂载时草稿均为空 → B 页初始内容 == 模板）→ A 页改代码等 1s → 切到 B 页 | B 页弹 `confirm`：`此代码已在另一个标签页更新，是否载入？`；选「是」→ 内容被替换 + toast `已载入另一个标签页的代码`；选「否」→ **B 页内容保持不变**。⚠️ **草稿必须先清**：若 B 页挂载时已载入旧草稿，它与 A 页写入的新值可能相同 → `storage` 事件因 `newValue` 未变而不触发（`problem.js:270` 只比 `newValue`）。**收尾：删两个键** | `AC-32`① | 🟢 🔁 |
| `WT-ED-07` | 多标签页冲突（挂载期分支） | 任意 + **草稿已清** | 写草稿 → 记下 `draftAt` 原值 → 把它改成未来时间 → 刷新 | 按 SPEC 应弹同一 `confirm`；**当前实现不会弹**（`draftAt` 写入了但从未被比较：`loadDraft` 返回的 `savedAt` 全仓库零消费者，见 `editor.js:143`）。⚠️ **收尾必须把 `draftAt` 还原或直接删除** —— 留一个未来时间戳在库里，会让将来真正实现了的挂载期分支**每轮都弹框**，`WT-ED-06` 之后全部受影响 | `AC-32`① | 🟡 🔁 |
| `WT-ED-08` | 切题再切回草稿仍在 | 任意 + **草稿已清** | 在 `two-sum` 写草稿 → 回 `/list.html` → 再进 `two-sum` | 草稿仍在（键按 slug 隔离）。**收尾：删 `draft:two-sum`** | `AC-4` | 🟢 🔁 |
| `WT-ED-09` | 草稿键按 slug 隔离 | 任意 + **草稿已清** | 两道题各写不同草稿 | `draft:<slugA>` 与 `draft:<slugB>` 互不覆盖。**收尾：删这两题的 4 个键** | `AC-4` | 🟢 🔁 |
| `WT-ED-10` | 「重置为模板」需二次确认 | 任意 + **草稿已清** | 输入点内容 → 点 `#reset` | 弹 `confirm`：`用题目的初始模板覆盖当前代码？当前草稿会被替换。`；确认后内容变模板；**取消则不变** | — | 🟢 |
| `WT-ED-11` | 空代码提交被前端拦截 | 任意 | 清空编辑器 → `Ctrl+Enter` | toast `请先写点代码再提交`；**不发请求**（Network 无 `/api/submissions`） | `D-28` | 🟢 |
| `WT-ED-12` | 超长代码被前端拦截 | 任意 | 粘贴 65KB+ 内容 → 提交 | toast `代码 XX.XKB 超过 64.0KB 上限`；**不发请求**。⚠️ **收尾：删草稿**（超长内容会落进 `localStorage`） | `D-28` `D-22` | 🟢 🔁 |
| `WT-ED-13` | `Ctrl/Cmd+Enter` 快捷键 | 登录 | 聚焦编辑器 → `Ctrl+Enter` | 等同点 `#submit` | `D-28` | 🔴 |
| `WT-ED-14` | 会话过期后草稿自动恢复 | 一次性账号 | 有草稿 → 手工删 Cookie（或等 7 天过期）→ 操作触发 1002 → 重登 | 跳 `/login.html?...&expired=1`；登录后回到该题，草稿自动载入 + toast `已恢复你上次未完成的代码`。⚠️ **必须用一次性账号**：草稿跨账号共享，用 `ZhangLing` 会在库里留下一个只有它能看到的草稿。**收尾：注销 + 删草稿** | `AC-32`② | 🟢 🔁 |
| `WT-ED-15` | 草稿写入失败不阻断提交 | 任意 | DevTools 禁用 localStorage → 改代码 → 提交 | Console 有 `[editor] 草稿写入失败` 警告；**提交链路不受影响** | `D-51` | 🟢 |

---

### M07 提交与判题 🔴

对应 `P4`、`D-10`、`D-54`、`D-60`、`D-62`、`AC-5`、`AC-14`、`AC-30`、`AC-31`。
**整层被 §3.3 阻塞（`judgeAlive=false`）。** 当前实例上点提交，预期是 `1009/503` 与错误文案。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-SB-01` | 提交后立即进入评测态 | 登录+judge | 点 `#submit` | `#status[data-state]` 变 `judging`，文案 `评测中…`；`#result` 仍 `hidden` | `AC-5` | 🔴 🔁 |
| `WT-SB-02` | `Ctrl+Enter` 与按钮等价 | 登录+judge | 分别用快捷键与按钮提交 | 两者行为完全一致 | `D-28` | 🔴 🔁 |
| `WT-SB-03` | 轮询节奏 1.5s | 登录+judge | 提交后 Network 面板计时 | `/api/submissions/{id}` 请求间隔约 `1500ms`，直到终态停止 | `D-10` | 🔴 |
| `WT-SB-04` | 队列位置提示 | 登录+judge + **占满 4 个全局闸门** | 先用别的账号提交占住 worker，再提交 | `#status` 显示 `评测中… 前面还有 N 个评测`；`N` 每秒递减。⚠️ 必须**先占住**才能造出 `N>0`；不占住时 `N` 恒为 0，本条等于没测 | `AC-30` `D-54` | 🔴 🔁 |
| `WT-SB-05` | 队列位置不泄露全局负载 | 登录+judge | 检查提交响应体 | 只含 `queueAhead`；**不含**全局队列长度、不含任何他人信息 | `D-60` | 🔴 |
| `WT-SB-06` | 单用户同时评测上限 | 登录+judge + **一次性账号** | 连续提交第 3 条 | toast `你已有 2 个评测在进行中，请等它们结束`。⚠️ **必须一次性账号**：该用例必然在库里留下 2~3 条 `pending`/`judging`，用 `ZhangLing` 会污染 `WT-HR-01/02` 的提交总数。**收尾：等终态 + 注销** | `AC-14` `D-22` | 🔴 🔁 |
| `WT-SB-07` | 每分钟提交上限 | **一次性账号** + 独占时间窗 | 1 分钟内提交 11 次 | 第 11 次 toast `提交过于频繁，请稍后再试（每分钟上限 10 次）`。⚠️ **三重隔离（R6）**：① 一次性账号 —— 否则这 11 次把 `ZhangLing` 的配额吃光，同一轮里后面的提交类用例全挂；② **排在串行队列最末尾**；③ 收尾必须等满 60s 窗口滑出，或直接注销账号（配额是**进程内按 userId** 计的，注销即失效） | `AC-14` `D-22` | 🔴 🔁 |
| `WT-SB-08` | **刷新恢复轮询** | 登录+judge | 提交后立刻 F5 | `sessionStorage['judging:two-sum']` 存在 → 状态区**自动重建**并继续显示 `评测中…` 直至出结果。⚠️ **收尾必删 `sessionStorage['judging:*']`**（或在终态后确认已删）：留一个幽灵键会让后续任何打开 `two-sum` 的用例（包括 `WT-ED-*`）看到一个永不转圈结束的状态区 | `AC-31` `D-53` | 🔴 🔁 |
| `WT-SB-09` | 终态后不再重建 | 登录+judge | 等结果出齐后再 F5 | `sessionStorage` 键已清；刷新**不**重建轮询、不转圈 | `AC-31` | 🔴 |
| `WT-SB-10` | **另一标签页不跟着转圈** | 登录+judge | A 页提交 → 开 B 页访问同一题 | B 页**不**重建轮询（`sessionStorage` 不跨标签共享） | `AC-31` | 🔴 |
| `WT-SB-11` | 慢评测兜底提示 | 登录+judge | 提交后等 > 5 分钟 | `#status` 变 `评测耗时较长，可稍后在「我的」页查看`；轮询继续 | `D-10` | 🔴 |
| `WT-SB-12` | judge 未接线的降级文案 | 登录 | **当前实例**：点 `#submit` | `#result` 渲染 `div.notice.notice--error`（`判题服务不可用…`）+ `p.result__foot`「当前 judge 调度器尚未接线（T2.7），提交未进入队列、也未写入任何记录」；`/api/admin/overview` 的 `judgeAlive=false` 可佐证。✅ **本层唯一天然幂等的用例** —— 它明确不写入任何记录，所以不会污染后续任何计数断言 | §3.3 | 🟢 |
| `WT-SB-13` | 提交配额是**按账号**而非全局 | **双一次性账号** | A 连发 10 次打满配额 → 紧接着 B 提交 1 次 | B **不受影响**（拿到 `judgeAlive` 正常受理），证明配额键是 `user_id` 而非全局。这是 `WT-SB-07` 的隔离策略**成立**的前提 —— 若配额是全局的，用一次性账号根本救不了别的用例 | `AC-14` `D-22` | 🔴 🔁 |

> `WT-SB-12` 是**当前唯一能跑**的提交类用例，它验的正是"降级路径也要给用户看得懂的解释"，
> 而不是把 `1009` 当成 bug 报上去。
>
> ⚠️ **v1.1 的限流现实提醒**：`rate_max_submits_per_window=10` 与 `judge_user_gate=2` 目前**只是配置、尚未接线**
> —— `include/web/ratelimit.h` 与 `src/web/ratelimit.cpp` 都是 **0 字节**，
> `handle_submission_create` 里没有任何配额检查，因此 `1006`/`1007` 两个码**当前不可达**
> （`API.md:1040` 已自报此缺口）。这带来两个后果：
> ① `WT-SB-06`/`07`/`13` 现在**验不了**，接线后才恢复；
> ② **这正是它们最容易在接线当天集体变红的那一刻** —— 因为"点一次就发 11 次"的旧写法，
> 从"永远通不过"变成"突然通了，但把整轮用例的配额吃光"。所以隔离方案要**现在**就写好。

---

### M08 结果面板三形态 🔴

对应 `P5`、`D-63`、`D-64`、`D-65`、`D-66`、`AC-6`、`AC-7`、`AC-40`~`AC-44`。
整层被 §3.3 阻塞；可用路径见 §3.3 路径 B。

> ⚠️ **v1.1：整层 10 条全部 🔁 —— 它们每条都要提交一次。**
> 十条用例打下来，`ZhangLing` 的提交数就 +10，`WT-HR-01`/`WT-HR-02` 的分页断言、
> `WT-LS-17` 的「提交」stat、`WT-PB-21` 的推荐理由**全部漂移**。
> 因此本层统一约定：**每条用例用自己的一次性账号 `ojt<runId><n>`**，
> 断言只针对**那一条提交**（`data.id` 固定），不碰任何跨用例的总数断言；收尾注销。
> 唯一例外是走 §3.3 路径 B 的 SQL 造数 —— 那条路径本来就是"造数据验渲染"，
> 造完**必须把造的 `submission_id` 那一行删掉**（`DELETE FROM submissions WHERE id=?` 级联清明细）。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-RS-01` | AC 全绿 | judge+AC 解 | 提交正确解法 | `span.verdict.verdict--ac`；`通过 2/2`；每行 `tr.case--yes`；`总耗时`/`峰值RSS` 有值 | `AC-6` | 🔴 🔁 |
| `WT-RS-02` | WA 逐用例定位 | judge+错解 | 提交第 2 个用例错的解 | `verdict--wa`；`通过 1/2`；第 1 行 `--yes`、第 2 行 `--no` | `AC-6` `AC-7` | 🔴 🔁 |
| `WT-RS-03` | 七列表头齐全 | 同上 | 读 `table.result__table thead th` | `# / 输入 / 你的输出 / 期望输出 / 耗时 / 峰值RSS / 结论` | `AC-7` | 🔴 🔁 |
| `WT-RS-04` | 内存列头写「峰值RSS」 | 同上 | 读第 6 列列头 | 字面为 **`峰值RSS`**，**不是**「内存」 | `AC-44` `D-63` | 🔴 🔁 |
| `WT-RS-05` | 口径脚注常驻不折叠 | 同上 | 读 `p.result__foot` | 文本形如 `ⓘ 限制口径：地址空间 128MB ｜ 列为实测峰值 RSS，仅供参考`；**无折叠控件** | `AC-44` | 🔴 🔁 |
| `WT-RS-06` | MLE 徽标与 RSS 不同层级 | judge+内存炸弹 | 提交撑爆内存的解 | 徽标写 `内存超限 · 地址空间 128MB`；RSS 数值**不与**徽标并列成同一视觉层级 | `AC-44` `D-63` | 🔴 🔁 |
| `WT-RS-07` | TLE 跨列显示 | judge+死循环 | 提交 `while(true){}` | 该行 `verdict--tle`；输入/输出/期望**跨列合并**显示 `超出时间限制 (3000ms)`；`耗时`/`峰值RSS` 为空 | `AC-6` | 🔴 🔁 |
| `WT-RS-08` | RE 显示 stderr 摘要 | judge+崩溃解 | 提交 `*p=nullptr` 解法 | `verdict--re`；该行展示 stderr 摘要；**有** `@@OJ@@` 前序行作证 | `AC-6` `D-66` | 🔴 🔁 |
| `WT-RS-09` | CE 形态：显示「N 个用例未执行」且**不渲染用例表** | judge+语法错误 | 提交 `class Solution{ vecto<int> a; }` | `verdict--ce`；状态行 `编译错误 · 2 个用例未执行`；**无 `table.result__table`** | `AC-41` `D-65` | 🔴 🔁 |
| `WT-RS-10` | JE 形态 + 重试按钮 | **需 `__expr` 坏题** | 提交到生成期校验漏网的坏题 | `verdict.verdict--je` `平台错误`；文案 `这通常是系统问题，不是你的代码问题`；**存在** `#retry`；**不显示**「运行错误」 | `AC-40` `D-66` | 🔴 🔁 |

---

### M09 我的 · 提交记录

对应 `P6`、`D-35`、`D-49`、`D-58`、`D-61`、`AC-10`、`AC-29`、`AC-37`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-HR-01` | 默认时间倒序 | 轮前已 reset | 打开默认 tab | `#sub-rows` 首行是**最新**那条；**基线相对（R3）**：`#sub-total` 的 N == `GET /api/submissions?size=1` 的 `data.total`（reset 后 34，第 2 页 14 条）。⚠️ **实测口径**：端点按 `ORDER BY s.id DESC` 排序（`submission_api.cpp:228`），**不是** `created_at DESC` —— 所以造数必须保证 id 序与时间序一致，否则首行会是一条 id 最大、时间却最早的记录。⚠️ **不写死 34**：任何提交类用例跑过都会让它变 | `AC-37` `D-61` | 🟢 🔁 |
| `WT-HR-02` | 每页 20 条 | 轮前已 reset（34 条） | 打开默认 tab | `data.size` 回显 `20`；`#sub-pager` 显示 `第 1 / 2 页`。**基线相对（R3）**：第 2 页行数 == `total - 20`（reset 后 14），不写死 | `AC-37` | 🟢 🔁 |
| `WT-HR-03` | 空态（从未提交） | **一次性账号** | 新账号 `ojt<runId>05` 打开 `history.html` | `#sub-rows` 内 `div.empty`：`还没有提交记录` + 「去刷题」链接；`#sub-total` 为**空串**（不是 `共 0 条`）。**收尾：注销** | `D-38` | 🟢 🔁 |
| `WT-HR-04` | 匿名直连被守卫拦截 | 匿名 | 直接打开 `/history.html` | 跳 `/login.html?redirect=%2Fhistory.html`；顶栏**无**「我的」入口 | `AC-25` | 🟢 |
| `WT-HR-05` | 展开详情：代码全文 | judge | 点 `tr.subrow[data-id]` | 展开 `tr.subdetail`，内含完整 `pre` 代码块 | `AC-10` | 🔴 |
| `WT-HR-06` | 展开详情：逐用例表 | judge | 展开一条 AC 提交 | `table.tagtable` 含 `# / 输入 / 你的输出 / 期望输出 / 结论` | `AC-10` | 🔴 |
| `WT-HR-07` | 详情只同时展开一条 | judge | 展开 A → 点 B | A 自动收起，B 展开 | — | 🔴 |
| `WT-HR-08` | 行键盘可达 | judge | `tr.subrow` 聚焦后按 `Enter` | 同样展开详情；按 `Space` 亦可 | — | 🔴 |
| `WT-HR-09` | 按题目筛选 | judge | `#f-problem` 选 `two-sum` | 只剩该题记录；与 `#f-verdict` **可组合** | `AC-10` `AC-37` | 🔴 |
| `WT-HR-10` | 按结论筛选 | judge | `#f-verdict` 选 `WA` | 只剩 WA 记录 | `AC-10` | 🔴 |
| `WT-HR-11` | 筛选后空态文案不同 | judge | 筛一个无记录的组合 | 文案为 `没有匹配的提交记录`（**不是**「还没有提交记录」） | — | 🔴 |
| `WT-HR-12` | **删除提交 + 二次确认** | judge + **自带造数** | ① 取基线 `#sub-total` 的 N → ② 用一次性账号 `ojt<runId>06` 提交一条并等到终态 → ③ 展开该条 → 点 `[data-del]` | `confirm`：`这条提交记录将被永久删除，含 N 个用例结果，不可恢复。确认删除？`（`N>0` 时才有「，含 N 个用例结果」这一段）。⚠️ **不能靠"先跑一条提交用例留下数据"** —— 那既赌执行顺序，又会在中途失败时留下无法回收的孤儿数据（R4） | `AC-29` `D-49` | 🔴 🔁 |
| `WT-HR-13` | 删除后行消失且总数减一 | judge + 自带造数 | 承上，确认删除 | 行消失；**`#sub-total` == 基线 N − 1**（R3 相对断言，不写死绝对值）；回到 `?slug=two-sum` 时最近提交**同步移除**。**收尾：注销账号**（级联清残留行，§2.5.2） | `AC-29` | 🔴 🔁 |
| `WT-HR-14` | 删除可取消 | judge + 自带造数 | `confirm` 选「取消」 | 行仍在；`#sub-total` **== 基线 N**（真没删，而不是"删了但计数没刷新"）。**收尾：注销账号** | — | 🔴 🔁 |
| `WT-HR-15` | 重跑按钮仅对 failed/JE 渲染 | judge | 展开一条 `done` 的提交 | **无** `[data-retry]` 按钮 | `AC-29` `D-58` | 🔴 |
| `WT-HR-16` | 重跑新增一条而非覆盖 | judge + **自带 failed 造数** | ① 造一条 `failed`/`JE`（§3.3 路径 B 的 SQL）→ ② 记下原记录 id/verdict → ③ 点「重跑」 | 历史条数 **== 基线 +1** 且多出来那条是 `pending`；**原记录 `id`/`status`/`verdict` 完全不变**。⚠️ **收尾：把新增那条也删掉** —— 本条每跑一次 +1 条，不清理会线性累积，最终把 `WT-HR-01/02` 的分页算术全部推翻 | `AC-29` `D-58` | 🔴 🔁 |
| `WT-HR-17` | 切 tab 与翻页不重取 overview | 登录 | 在三个 tab 间来回切 + tab1 翻页 | `/api/me/overview` 全程**只有 1 条请求** | `AC-24` | 🟢 |
| `WT-HR-18` | hash 定位 tab | 登录 | 直接打开 `/history.html#progress` | `#pane-progress` 激活、其余 `hidden`；tab 按钮 `aria-selected` 正确 | `D-39` | 🟢 |
| `WT-HR-19` | 未知 hash 回落到默认 | 登录 | 打开 `/history.html#nope` | 回落 `#records` | `D-39` | 🟢 |

> ⚠️ **v1.1：执行顺序约束（R6）**。`WT-HR-12/13/14/16` 是本模块唯一会**改变数据**的四条。
> 若它们跑在 `WT-HR-01/02` 之前，后者的基线就变了。三种安排任选其一，写进执行脚本：
>
> | 方案 | 做法 | 代价 |
> |---|---|---|
> | ① 全部用一次性账号（**推荐**） | 四条改用 `ojt<runId><n>`，`ZhangLing` 永不被写 | 依赖 judge 可用 |
> | ② 排序 | 放到 `WT-HR-01~11` 之后执行 | 删除若命中首页首行，`WT-HR-01` 的"首行是最新"前提仍在，但总数断言必须已基线相对化 |
> | ③ 轮后 reset | `./build/oj-reset` + 重启服务 | 打断整轮，最重 |
>
> **默认取 ①**：`ZhangLing` 的只读基线被 `WT-LS-17`、`WT-PB-21`、`WT-MG-*` 等十几条用例共享，
> 不值得为四条用例把它搭进去。

---

### M10 我的 · 掌握分布

对应 `P6`、`D-38`、`D-39`、`D-68`、`AC-22`、`AC-24`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-MG-01` | 难度卡三档 | 登录 + 轮前已 reset | 切到 `#progress` tab | `.diffcard` **三张齐全**（简单/中等/困难）。⚠️ **基线相对（R3）**：每张卡的标题与 `GET /api/me/overview.byDifficulty` 的 key 一一对应，且每张卡的 `total` > 0（reset 后 13/8/5）；**不写死 `5/13`** —— 那是 reset 快照 | `AC-24` | 🟢 🔁 |
| `WT-MG-02` | 进度条宽度 = solved/total | 登录 + 轮前已 reset | 读 `byDifficulty`，逐档量 `.bar__fill--*` 的宽度百分比 | `Math.round(width%) == Math.round(solved/total*100)`，三档都验。**比值断言，与具体数字无关** | `AC-24` | 🟢 🔁 |
| `WT-MG-03` | `total=0` 的难度不渲染该条 | **仅裸库可验** | 看难度卡 | 造数后 easy/hard 都有题 → 该分支不可验。验法二选一：① 裸库（不跑 `oj-reset`）；② 用零提交账号 `alice01` 确认「`solved=0` **仍渲染**」，说明过滤条件是 `total` 而非 `solved`。⚠️ 走 ② 时**不要往 `alice01` 提交任何东西**，它是 `WT-AD-05`「通过率显示 —」的唯一样本 | `D-38` | 🔵 🔁 |
| `WT-MG-04` | 标签表四列 | 登录 + 轮前已 reset | 切 `#progress` | `table.tagtable` 表头 `标签 / 已过 / 总数 / 通过率`。⚠️ **基线相对**：首行 == `byTag` 里 `total` 最大者（reset 后 `数组`），**不写死 `9/20`**；只断言"排序与 `byTag` 数组顺序一致" | `AC-22` | 🟢 🔁 |
| `WT-MG-05` | **标签多对多不重复计数** `[D-38]` | 登录 + 轮前已 reset | 核对 `two-sum`（挂 2 个 algorithm + 1 个 source 标签） | 该题在**两个** algorithm 标签的 `total` 中**各计 1 次**（不是 2 次）；`byTag[].total` 之和 **= 3 > 1**（大于题目总数是**正确**行为）。⚠️ 用恒等式表达（`Σ total == Σ 每题标签数`），不依赖题库规模 | `AC-22` | 🟢 🔁 |
| `WT-MG-06` | `byDifficulty` 总数自洽 | 登录 | 把三档 `total` 相加 | **== `GET /api/problems?size=1` 的 `data.total`**（R3）。⚠️ 这条是**探针题残留的哨兵**：一旦 `WT-PB-03/04` 的坏题没清干净，题库 26→27，本条立刻红。裸库时两边都等于 1 | `AC-22` | 🟢 🔁 |
| `WT-MG-07` | 标签按 total 倒序 | 轮前已 reset（11 标签） | 看标签表 | 顺序按 `total` 降序、同值按名称；与 `byTag` 数组顺序逐项一致 | — | 🟢 🔁 |
| `WT-MG-08` | source 标签折叠 | 登录 + 轮前已 reset | 看标签表下方 | `高频面试题` 折叠在 `<details class="more"><summary>展开来源标签（N）</summary>` 内，**N == `byTag` 里 category=source 的条数**（reset 后 1）。不写死 `（1）` | `D-39` | 🟢 🔁 |
| `WT-MG-09` | 与首页概览条**同源同值** | judge | 并排比对 `list.html#overview` 与 `history.html#pane-progress` | `已通过 x/N` 与三个难度进度条**逐项相等**（同一端点、同一 `overview.js` 渲染） | `AC-24` | 🔴 |
| `WT-MG-10` | 概览条最近 3 条可点跳转 | judge | 点 `#overview` 内 `.recent` 项 | 跳 `/problem.html?slug=...`；每条含 `.recentrow__time`（`MM-DD HH:mm`）、题名、`.verdict` 徽标 | `D-38` | 🔴 |
| `WT-MG-11` | 会话过期提示 | **需过期** | 注入 `oj-reset` 造的过期 sid（`alice01-expired`）→ 打开 `#progress` | `div.notice.notice--warn`：`登录状态已过期，请重新登录后查看掌握分布。` | `D-44` | 🔵 |

---

### M11 我的 · 账户设置

对应 `P9`、`D-40`、`D-56`、`D-57`、`AC-21`。

> ⚠️ **v1.1：本模块 11 条里有 8 条是"一次性"的，比原先"用临时账号"的要求更强一档。**
> 原写法"用临时账号"仍有漏洞：同一个临时账号被 `WT-AC-05` 改过密之后，
> `WT-AC-07` 再拿旧密码登录就验错了 —— **改密是不可逆的，必须每条用例一个全新账号**（R1）。
> 另外注销用例**消耗账号本身**（`WT-AC-10`~`13` 跑完账号就没了），第二次跑若复用同名会撞 `1005`。
>
> **本模块统一模板**：
> `账号 = ojt<runId><seq>`（每条不同）→ 干活 → `finally { DELETE /api/auth/account }`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-AC-01` | 账户信息只读 | 登录 | 切到 `#account` | `#acc-username` 显示用户名；**不提供**修改用户名的输入框（`D-40` 要点 6）；`#acc-role` 显示角色 | `D-40` | 🟢 |
| `WT-AC-02` | 普通用户不显示"普通用户"标签 | `zhangling` | 看 `#acc-role` | 显示文本 `普通用户`；**顶栏**则不显示（两处刻意不同） | `D-41` | 🟢 |
| `WT-AC-03` | 改密三项前端校验 | 一次性账号 | 依次触发：旧密码空 / 新密码 5 位 / 两次不一致 | 分别报 `请输入当前密码` / `新密码需 6–64 位` / `两次输入的新密码不一致`；**三次都不发请求**（故密码未变，可与本层其他用例共享一个账号） | `D-40` | 🟢 |
| `WT-AC-04` | 旧密码错误 | 一次性账号 | 用错误旧密码提交 | `#old-password-error` = `旧密码不正确`；`#old-password` 被清空；**`#new-password` 保持已填值不清空**（便于只改旧密码后重试）。密码未变 → 该账号可继续用 | `D-40` §7.4 | 🟢 🔁 |
| `WT-AC-05` | **改密后当前会话仍有效** | 一次性账号 | 改密成功（`pass1234` → `pass5678`） | toast + 表单 `.reset()`；**页面不跳转、未登出**；`auth/me` 仍 `200`；顶栏静默重拉。⚠️ **该账号密码已永久变为 `pass5678`，不得再被其他用例按 `pass1234` 登录** | `AC-21` | 🟢 🔁 |
| `WT-AC-06` | **改密踢掉其他会话** | 一次性账号 + 两个 context | 同账号在 A/B 两处登录 → 在 A 处改密 → 回到 B | B 的 `/api/auth/me` 返回 `401/1002`；A 仍 `200`。**收尾：注销**（此时该账号密码已是新密码，注销请求要用新密码） | `AC-21` `D-40` | 🟢 🔁 |
| `WT-AC-07` | 旧密码失效 / 新密码可登录 | 一次性账号（**自带改密**） | ① 先改密 `pass1234`→`pass5678` ② 登出 ③ 用旧密码登录 / 用新密码登录 | 旧密码 `400/1001`；新密码 `200/0`。⚠️ **不能假设"别人已经改过密"**，本条自带前置改密才自洽 | `AC-21` | 🟢 🔁 |
| `WT-AC-08` | 新密码长度不符（服务端侧） | 一次性账号 | 绕过前端直接调接口传 5 位 | `400/1001`，`data.field='newPassword'`。⚠️ 服务端还多一条前端不校验的规则：**全空白密码也拒**（`auth_api.cpp:115-120`），可一并验 | `AC-21` | 🟡 |
| `WT-AC-09` | 登出当前账号 | 登录 | 点 `#logout` | 跳 `/login.html`；服务端会话行删除（`session.cpp:240`）。天然幂等 | `D-40` | 🟢 |
| `WT-AC-10` | **注销账号：输密码 + 逐项列出** | 一次性账号 | 切 `#account` 底部 → 点注销 | ① 要求输密码；② 二次确认弹窗**逐项列出** `提交 N 条 / 收藏 M 题 / 会话 K 个` + 红字**不可恢复**（**不是**一句"确定吗？"）。**收尾：注销本身**（账号即消耗品，第二次跑必须换名） | `AC-33` `D-57` | 🟡 🔁 |
| `WT-AC-11` | 注销密码错误不删数据 | 一次性账号 | 输错密码确认 | `1001`；**任何数据都不删**（账号仍可正常登录）→ 因此本条**可以继续用**该账号做后续校验 | `AC-33` | 🟡 🔁 |
| `WT-AC-12` | 注销后清空本地草稿 | 一次性账号 | 先在**两个不同 slug** 上写草稿 → 注销成功 | `localStorage` 中**所有** `draft:*` 与 `draftAt:*` 被删净。⚠️ 草稿键**不含用户名**（`editor.js:24`），本条实际删的是**整个浏览器 profile 的草稿** —— 所以它**必须在一个专用的干净 context 里跑**，否则会连带清掉 `WT-ED-*` 用例刚写的草稿 | `AC-33` `D-57` | 🟡 🔁 |
| `WT-AC-13` | 注销后跳登录页并提示 | 一次性账号 | 注销成功 | 跳 `/login.html?deleted=1`；`#notice` 显示 `账号已注销，相关数据已一并删除。` | `D-57` | 🟡 🔁 |
| `WT-AC-14` | 注销的级联完整性 | 一次性账号 + DB | **先造数据**（提交 1 条 / 收藏 1 题 / 开 2 个会话）→ 注销 → 库中核对 | 这 5 张表的 `WHERE user_id = <被注销者>` 行数**全为 0**：`sessions`/`submissions`/`submission_case_results`/`submission_compile_errors`/`user_problems`；`problems`/`problem_cases`/`tags` **总行数不变**（**先记基线再比**，不写死绝对值） | `AC-33` `R-12` | 🔵 🔁 |

---

### M12 运维概览

对应 `D-37`、`AC-19`、`AC-23`。

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-AD-01` | admin 可访问 | `admin` | 打开 `/admin.html` | `#denied` 保持 `hidden`；系统状态卡 + 用户清单表渲染 | `AC-19` | 🟢 |
| `WT-AD-02` | system 十二项齐全 | `admin` | 读 `#sys-stats` + `#sys-runtime` | `#sys-stats` 四张卡（用户/题目/提交/未完成提交）；`#sys-runtime` 含 `队列长度`、`并发评测`、`运行中/未接线` pill | `AC-19` | 🟢 |
| `WT-AD-03` | **judge 未接线如实回显** | `admin` | 看 `#sys-runtime` 第三个 pill | class 含 `pill--warn`、文案 `未接线`；`#sys-note` 出现解释段落；**不得**显示 `运行中` | `AC-19` | 🟢 |
| `WT-AD-04` | 用户清单列齐全 | `admin` | 看 `table.admin-users` | `thead` **11 列**；每行角色用 `span.badge.badge--admin` 或 `span.muted`；`#user-count` 的数字 **== `data.system.userCount`**（R3 相对断言）。⚠️ **不要写死「共 7 人」** —— 7 是 `oj-reset` 的产物，而**每条一次性账号用例都往 `users` 插一行**（`userCount` 是实时 `COUNT(*)`，`admin.cpp:117`），写死必然在第一次跑完之后就永久变红 | `AC-19` | 🟢 🔁 |
| `WT-AD-05` | 零提交用户的展示 | `admin` | 看 `alice01` 行 | 提交数 0 → 通过率显示 `—`（**不是 `0%`**）；`lastVerdict` 为 `—` 且**不回退到上一次结论**。⚠️ **`alice01` 是全层唯一"零提交"样本，谁都不能往它提交**（`WT-MG-03` 也用它） | `AC-19` | 🟢 |
| `WT-AD-06` | 普通用户被拒 | `zhangling` | 打开 `/admin.html` | `#denied` 可见「无权访问」；**1.5 秒后自动跳 `/list.html`**；`/api/admin/overview` 返回 `403/1003` | `AC-19` | 🟢 |
| `WT-AD-07` | 匿名被拒且码序正确 | 匿名 | 打开 `/admin.html` | `/api/admin/overview` 返回 `401/1002`（**不是 `1003`** —— 先判登录再判角色） | `AC-19` | 🟢 |
| `WT-AD-08` | **严格只读** | `admin` | 全页扫描 | 页面上**无任何** `<form>`、无写操作按钮；Network 面板除 `GET /api/admin/overview` 外**无其他请求** | `AC-19` `D-37` | 🟢 |
| `WT-AD-09` | 刷新按钮 | `admin` | 点 `#refresh` | 按钮变 `加载中…` 且 `disabled`，完成后恢复并重取数据 | — | 🟢 |
| `WT-AD-10` | 读取失败的用户可见提示 | `admin` | 拦掉 API 后点 `#refresh` | toast `管理端点读取失败，请看 logs/app.log`（驻留 6s） | — | 🟢 |
| `WT-AD-11` | **整轮跑完后基线未漂移**（收尾自检） | `admin` | 整轮所有用例跑完后执行：取 `system.userCount` / `problemCount` / `submissionCount`，与 `globalSetup` 写的 `baseline.json` 逐项比 | **四项全部相等**。若不等，报告里指名是哪个数、漂了多少。⚠️ 本条**不删数据、只报警**，它的职责是把"隐性故障"（三轮都绿但基线已经烂了）变成报告里一条显性红 | — | 🟢 🔁 |

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
| `WT-GD-06` | 草稿冲突的挂载期分支缺失 | 任意 + **草稿已清** | 改 `localStorage['draftAt:two-sum']` 为未来时间后刷新 | **不弹**任何 `confirm`（`draftAt` 写入了但从未被读取：`loadDraft` 返回的 `savedAt` 全仓库零消费者）。⚠️ **收尾：删掉两个草稿键** —— 留一个未来时间戳，等挂载期分支真被实现后，它会让**之后每一轮**的 `WT-ED-06` 都莫名弹框 | `D-55` | 🟢 🔁 |
| `WT-GD-07` | `clearDraft` 无调用方 | — | 检索 `static/` | 唯一保留给 `[D-57]` 的 `clearDraft()` 目前**无任何调用点** | `D-57` | 🟢 |

---

### M14 鉴权边界与输入安全

对应 `D-37`、`AC-13`、`AC-20`、`AC-36`、`NF-7`。**以浏览器为载体的越权验证。**

| 用例ID | 标题 | 前置 | 步骤 | 预期结果 | AC | 状态 |
|---|---|---|---|---|---|---|
| `WT-SX-01` | 匿名访问提交接口 | 匿名 | 打开 `history.html` | 跳登录带 `redirect`；后端 `/api/submissions` 返回 `401/1002` | `AC-13` | 🟢 |
| `WT-SX-02` | 用户 A 访问用户 B 的提交详情 | **两个一次性账号** | 本层统一前置：`A = ojt<runId>a`、`B = ojt<runId>b`，**B 自带一条提交**（`seedSubmission(B)`），记下其 id → A 请求 `/api/submissions/{B的id}` | `403/1003`。⚠️ B **不得复用 `ZhangLing`**：这五条每跑一次就给 B 加一条提交，`WT-LS-17`/`WT-HR-01` 的计数会持续漂。**收尾：注销 A 与 B** | `AC-13` | 🔵 🔁 |
| `WT-SX-03` | **admin 也不放开归属** | `admin` + 一次性 B 的提交 | `admin` 请求 B 的提交详情 | **仍是 `403/1003`** —— `[D-37]` 的 conscious omission | `AC-20` | 🔵 🔁 |
| `WT-SX-04` | 删除他人提交被拒 | 一次性 A + B | A 调 `DELETE /api/submissions/{B的id}` | `403/1003`；B 的记录完好（**复查：总数与内容都未变**，不能只看码） | `AC-36` | 🔵 🔁 |
| `WT-SX-05` | diff 他人提交被拒 | 一次性 A + B | A 调 `?from={A_id}&to={B_id}` | `403/1003` | `AC-36` | 🔵 🔁 |
| `WT-SX-06` | 重跑他人提交被拒 | 一次性 A + B | A 调 `POST /api/submissions/{B_id}/retry` | `403/1003`；**B 的记录状态未被改动**（没被偷偷入队） | `AC-36` | 🔵 🔁 |
| `WT-SX-07` | 重跑非 failed 记录 | judge | 对 `done` 的提交调 retry | `409/1011`；**且前端不渲染**重跑按钮（`canRetry=false`） | `AC-29` | 🔴 |
| `WT-SX-08` | 会话过期全局跳转 | 登录 | 清掉 Cookie 后点任意需鉴权操作 | 全局跳 `/login.html?redirect=<当前路径>` | `§7.3` | 🟢 |
| `WT-SX-09` | Cookie 不可被 JS 读 | 登录 | Console 执行 `document.cookie` | **读不到 `sid`**（`HttpOnly`） | `AC-1` `D-69` | 🟢 |
| `WT-SX-10` | 同源 Cookie 策略 | 登录 | 检查登录响应头 | `SameSite=Lax`；**首版无 `Secure`**（HTTP 部署下的刻意取舍，启用 HTTPS 时必须补） | `D-69` | 🟢 |
| `WT-SX-11` | SQL 注入：关键词 | 任意 | `#f-keyword` 填 `' OR 1=1--` | 正常返回 0 条或正常匹配；**不得** 5xx、**不得** 泄露 SQL 错误文本 | `NF-7` | 🟢 |
| `WT-SX-12` | SQL 注入：用户名登录 | 匿名 | 用户名填 `' OR 1=1--`，密码随意 | `400/1001`；**不得**登录成功 | `NF-7` | 🟢 |
| `WT-SX-13` | SQL 注入：代码体 | **一次性账号** | 提交体里塞 `'; DROP TABLE submissions;--` | 正常受理或被校验拒；**不得** 5xx；事后 `GET /api/problems?size=1` 仍 `200`（表还在）。⚠️ 若"正常受理"则库里多一条脏提交 → 用一次性账号 + 注销收尾 | `NF-7` | 🟢 🔁 |
| `WT-SX-14` | XSS：题面渲染 | **需构造坏题** | 题面含 `<img src=x onerror=alert(1)>` | 被 DOMPurify 清除，**无弹窗** | `AC-3` `AC-27` | 🔵 |
| `WT-SX-15` | XSS：代码显示 | 任意 | 提交历史里代码含 `<script>` | 以文本节点渲染，**不执行**（不用 `innerHTML` 拼代码） | `§6.3.2.1` | 🟢 |
| `WT-SX-16` | XSS：用户名回显 | 任意 | 尝试注册用户名 `<img src=x onerror=alert(1)>` | **优先断言被拒**：`validate_username` 只放行 `[A-Za-z0-9_-]{3,32}`（`auth_api.cpp:83-92`），应得 `400/1001`。⚠️ **注意**：用户名根本放不进特殊字符，所以这条**真正能验的是"注册端按白名单拦"**，"回显按文本渲染"只是附带的第二层。**次用例**（可选，走 §3.3 路径 B 直接改库造一个带 `<` 的用户名）才验回显不弹窗；此时用一次性账号并收尾 | `§6.3.2.1` | 🟢 🔁 |
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
| `WT-NF-07` | 提交到出结果 P95 | **需 4G + judge** | 10 用例以内的题，连跑 20 次 | P95 `< 8s`。⚠️ **必须错开限流**：`rate_max_submits_per_window=10`/60s，20 连发必然触发 `1006`，而"排队等重试"的时间会被算进 P95，让本条**永远假红**。正确做法：分 2~3 个一次性账号轮转，或每次间隔 ≥6s | `NF-1` | 🔵 🔁 |
| `WT-NF-08` | diff 计算耗时 | 4G + diff 实现 | 两条 4000 行提交做 diff | P95 `< 100ms`；`> 2000` 行降级为整体替换 | `AC-28` `R-11` | 🟡 |
| `WT-NF-09` | 并发下不误判 TLE | **需 4G + judge** | 造 2.5s CPU 用例，**全局 4 并发**（= 全局闸门 `[D-74]`）连跑 10 轮 | **100% 得到 AC/WA，0 次 TLE**。⚠️ 4 并发意味着至少 4 个账号同时提交（单用户闸门只有 2），且总提交数 40 > 每分钟配额 10 → **必须临时把 `OJ_RATE_MAX_SUBMITS` 调高，或分 4 分钟跑**。这是本层唯一需要改 `.env` 的性能用例，原因就是限流会污染延迟测量 | `AC-38` | 🔵 🔁 |
| `WT-NF-10` | 阻塞能被抓住 | 4G + judge | ①`sleep(999)` ②`while(true){}` ③正常用例×50 | ①1.5s 内判 TLE（**不是**等 35s 兜底）②约 4s 判 TLE ③50/50 全 AC。⚠️ ③的 50 次提交是配额的 **5 倍**，同 `WT-NF-09`：要么调高 `OJ_RATE_MAX_SUBMITS` 并在报告里注明，**要么把 ③拆成 5 轮 × 10 次、轮间隔 60s**。且 50 条提交会永久留在库里 → 用一次性账号 + 注销收尾 | `AC-39` | 🔵 🔁 |
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
| AC-1 | 注册登录 | `WT-AU-01`~`03,07,09,17,18,21,22` | L2 | 部分 |
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
| AC-12 | 收藏 | `WT-LS-21,22,25`、`WT-PB-09` | L2 | 部分 |
| AC-13 | 鉴权边界 | `WT-SX-01,02` | L4 | 部分 |
| AC-14 | 限流配额 | `WT-SB-06,07` | L3 | 🔴 |
| AC-15 | 重启恢复 | — （curl 文档已覆盖） | — | — |
| AC-16 | 用例落库与范式 | — （`run_problemsync_test.sh` 已覆盖） | — | — |
| AC-17 | 过期会话回收 | — （curl 文档已覆盖） | — | — |
| AC-18 | 角色声明式同步 | `WT-AU-20`、`WT-SX-18` | L4 | 🔵 |
| AC-19 | 只读管理能力 | `WT-TB-09`、`WT-AD-01`~`11` | L2 | ✅ |
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
├── global-setup.js             # ★ R5：轮前快照基线 + 清残留（幂等）
├── global-teardown.js          # ★ R4：轮后基线比对（WT-AD-11）
├── helpers.js                  # ★ R2：favoriteTo / seedSubmission / readBaseline
├── fixtures.js                 # 登录态、一次性账号、辅助断言
├── specs/
│   ├── L0-static.spec.js       # M01
│   ├── L1-anonymous.spec.js    # M02 部分 + M03 部分 + M04 部分
│   ├── L2-list.spec.js         # M04
│   ├── L2-problem.spec.js      # M05
│   ├── L2-editor.spec.js       # M06
│   ├── L2-topbar.spec.js       # M03
│   ├── L2-account.spec.js      # M11（改密部分）
│   ├── L2-admin.spec.js        # M12（含 WT-AD-11 基线自检）
│   ├── L3-submit.spec.js       # M07 + M08（judge 未接线时 skip）
│   └── L4-authz.spec.js        # M14
└── README.md
```

> ★ = v1.1 新增的**幂等性基础设施**。这四个文件不是"可选的工具"，
> 而是 §1.5 六条硬规则的落地载体 —— 没有它们，§5 里的 🔁 用例只能靠人肉记忆收尾。

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
  globalSetup: require.resolve('./global-setup'),      // ★ R5
  globalTeardown: require.resolve('./global-teardown'), // ★ R4
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

/** 本轮开始的 UTC 时刻 —— 孤儿账号清理谓词的 cutoff（R5，见 global-setup.js） */
const RUN_START_UTC = new Date();

/**
 * ★ R1：本轮唯一标识。所有自造实体的名字都以它开头，跨轮绝不重名。
 *
 * 两处都不能省：
 *
 * ① **前缀用 `ojt` 而不是 `wt`**。前缀同时是 R5 清理谓词 `username LIKE 'ojt%'`
 *    的选择器 —— 用 `wt` 会误伤真人手工建的 `wtxxx` 账号。`ojt`（oj-test）
 *    与 `oj-reset` 既有的 `oj\_probe%` 命名习惯同源（`tools/reset_db.cpp:997`）。
 *
 * ② **必须用完整 base36，不要 `.slice(-4)`**。base36 末 4 位每
 *    36^4 ≈ 1.68e6 ms（≈28 分钟）才进位一次，截 4 位会让**相隔半小时内的两轮
 *    拿到同一个 runId**，第二轮的 `ojt<runId>01` 正好撞上第一轮残留的账号 → `1005`。
 *    完整 base36 约 8 字符，30 年内不重复。
 *
 * `ojt` + 8 字符 + 2 位序号 = 12 字符，远小于 `username VARCHAR(32)`。
 *
 * ★ **可用 OJ_RUN_ID 覆盖**：CI 里固定 `OJ_RUN_ID=ci042` 之后，
 *   重跑同一个 id 就能**精确 grep 出那一轮的全部残留**（`SELECT ... WHERE username LIKE 'ojtci042%'`），
 *   而不必靠"早于本轮开始"的时间窗口去猜。反过来，本地随手跑时留空、用时间戳即可。
 */
const RUN_ID = `ojt${process.env.OJ_RUN_ID || Date.now().toString(36)}`;

/** 账号序号，单调递增，保证同一条用例内多个账号也不重名 */
let _seq = 0;
const nextSeq = () => String(++_seq).padStart(2, '0');

/** ★ 泄漏账本：没能自动注销的账号名，global-teardown 会把它打进报告 */
const LEAKED = new Set();

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
  //    ★ v1.1：用户名必须每条用例唯一（否则第二次跑撞 1005），
  //      且必须**自动注销收尾**（否则 WT-AD-04 的用户数永久漂移）。
  //      `password` 字段会被改密用例改掉，所以注销时用 this.current()。
  freshUser: async ({ page, request }, use) => {
    const username = `${RUN_ID}${nextSeq()}`;
    const password = 'pass1234';
    const state = { username, password };

    const res = await request.post('/api/auth/register',
      { data: { username, password } });
    if (res.status() !== 201) throw new Error(`注册失败 ${username}: ${res.status()}`);
    await login(page, username, password);

    // ★ R4：无论用例是 pass 还是 fail，退出时都尝试注销并注销清理账本
    try {
      await use({ page, username, password, current: () => state.password });
    } finally {
      state.password = await tryCancelAccount(request, username, state.password);
    }
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

/** 尝试注销账号；密码可能已被改密用例改过，故允许传入"当前密码" */
async function tryCancelAccount(request, username, password) {
  const login = await request.post('/api/auth/login', { data: { username, password } });
  if (login.status() !== 200) {
    // 登录失败 = 密码已被改而我们不知道。留下账本条目，由 global-teardown 报告
    LEAKED.add(username);
    return password;
  }
  const del = await request.delete('/api/auth/account', { data: { password } });
  if (del.status() !== 200) LEAKED.add(username);
  return password;
}

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

module.exports = {
  test, expect: base.expect, assert: expect,
  USER, PASS, ADMIN, RUN_ID, LEAKED, RUN_START_UTC,
};
```

### 7.5 幂等性基础设施：`helpers.js` + `global-setup.js` ★

> **这是 v1.1 最重要的一节**。§5 里的 🔁 标记不是注释，是**对这四个文件的依赖清单**：
> 归一化靠 `favoriteTo`、基线靠 `readBaseline`、收尾靠 `tryCancelAccount`、轮前清理靠 `globalSetup`。
> 少任何一个，对应的 🔁 用例就会退回"只能跑一次"的状态。

#### `helpers.js` —— R2 归一化与 R3 基线

```js
const BASE = process.env.OJ_BASE || 'http://82.156.75.119:8080';

/**
 * R2：把某题的收藏态**调到**目标值，而不是"点一次"。
 *
 * 为什么要这样：服务端是硬 toggle，取消收藏会 DELETE 整行 user_problems
 * （src/web/user_api.cpp:173-175）。所以「点一次就断言 ★」这种写法第二次跑必挂，
 * 而且还会顺手删掉进度行，污染 WT-LS-17 的收藏数。
 */
async function favoriteTo(page, slug, want) {
  const api = await currentFavorite(page, slug);        // 先读当前态
  if (api === want) return false;                       // 已是目标态 → 不发请求
  const star = page.locator(`button.star[data-star="${slug}"]`);   // 列表页星标
  if (await star.count()) {
    await star.click();
  } else {                                              // 题目页星标
    await page.goto(`/problem.html?slug=${slug}`);
    await page.locator('#favorite').click();
  }
  return true;
}

async function currentFavorite(page, slug) {
  const body = await page.evaluate(async s => {
    const r = await fetch(`/api/problems/${s}`);
    return r.json();
  }, slug);
  return body?.data?.isFavorite === true;
}

/** R3：取一份"轮前基线"，供相对断言用；不要在用例里写死 26 / 34 / 4 / 7 */
async function readBaseline(request) {
  const admin = await adminCookie(request);
  const ov = await (await request.get('/api/admin/overview',
    { headers: { cookie: admin } })).json();
  const problems = await (await request.get('/api/problems?size=1')).json();
  const slugs = await (await request.get('/api/problems?size=100')).json();
  return {
    problemTotal: problems?.data?.total,
    userCount: ov?.data?.system?.userCount,
    submissionCount: ov?.data?.system?.submissionCount,
    probeSlugs: (slugs?.data?.items || [])
      .map(p => p.slug).filter(s => /-probe$/.test(s)),   // 探针题残留哨兵
  };
}

/** R5：清掉某题的两个草稿键。本层所有写草稿的用例都必须先调它。 */
async function clearDraft(page, slug) {
  await page.evaluate(s => {
    localStorage.removeItem(`draft:${s}`);
    localStorage.removeItem(`draftAt:${s}`);
    sessionStorage.removeItem(`judging:${s}`);
  }, slug);
}

module.exports = { BASE, favoriteTo, currentFavorite, readBaseline, clearDraft };
```

#### `global-setup.js` —— 轮前自检与清理

```js
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { readBaseline } = require('./helpers');
const { RUN_START_UTC } = require('./fixtures');

const BASELINE = path.join(__dirname, 'baseline.json');
const MYSQL = ['-h127.0.0.1', '-uoj', '-poj', 'oj'];

module.exports = async function globalSetup(config) {
  const { request } = config;
  const before = await readBaseline(request);
  const prev = fs.existsSync(BASELINE) ? JSON.parse(fs.readFileSync(BASELINE, 'utf8')) : null;

  // ① 探针题残留：WT-PB-03/04 上轮没清干净就会留在这儿（§2.5.5）。
  //    删除动作在部署机上执行（本地测试机连不上 MySQL），这里只负责**发现并报警**。
  if (before.probeSlugs.length) {
    console.warn(`[R5] ⚠️ 发现探针题残留：${before.probeSlugs.join(', ')}\n` +
      '    题库基线已被污染，请在部署机执行 ./build/oj-reset 后再跑回归。');
  }

  // ② 跨轮漂移告警：三项基线必须与上一轮一致，否则本轮结论不可信
  if (prev) {
    for (const k of ['problemTotal', 'userCount', 'submissionCount']) {
      if (prev[k] !== before[k]) {
        console.warn(`[R5] ⚠️ 基线漂移：${k} ${prev[k]} → ${before[k]}（${before[k] - prev[k]}）。\n` +
          '    上一轮有用例没有收尾干净，本轮结果仅供定位问题，不作回归结论。');
      }
    }
  }

  // ③ 孤儿账号清扫：上一轮崩在中途时 try/finally 没跑到，账号就留下了。
  sweepOrphans();

  fs.writeFileSync(BASELINE, JSON.stringify(before, null, 2));
};

/**
 * 清理谓词二选一，优先级从高到低：
 *   ① 设了 OJ_RUN_ID → username LIKE 'ojt<runId>%'   （精确，可重跑同一轮复现）
 *   ② 本地随手跑     → username LIKE 'ojt%' AND created_at < <本轮开始的 UTC 时刻>
 *
 * 为什么用 created_at 而不是 id：users.created_at 是 DATETIME NOT NULL
 * （db/init/01-schema.sql:47），按约定存 UTC（oj-reset 用 UTC_TIMESTAMP()），
 * 所以 cutoff 也必须是 UTC 字符串，不能用 Date.toString()。
 */
function sweepOrphans() {
  const cutoff = RUN_START_UTC.toISOString().slice(0, 19).replace('T', ' ');

  // OJ_RUN_ID 直接进 LIKE 模式，必须先夹紧：`_` 是 LIKE 单字符通配、
  // `%` 是任意长通配，放进来会让谓词比预期宽得多（`-e` 也可能把引号带出去）。
  const raw = process.env.OJ_RUN_ID || '';
  if (raw && !/^[A-Za-z0-9]+$/.test(raw)) {
    console.warn(`[R5] 忽略非法 OJ_RUN_ID=${raw}（只允许字母数字），退回时间窗口谓词。`);
  }
  const pinned = /^[A-Za-z0-9]+$/.test(raw) ? raw : '';

  const where = pinned
    ? `u.username LIKE 'ojt${pinned}%'`
    : `u.username LIKE 'ojt%' AND u.created_at < '${cutoff}'`;

  let out;
  try {
    out = execFileSync('mysql', [...MYSQL, '-N', '-B', '-e',
      `SELECT u.username FROM users u WHERE ${where} ORDER BY u.created_at`],
      { encoding: 'utf8' }).trim();
  } catch (e) {
    console.warn('[R5] 孤儿清扫跳过：连不上 MySQL（测试机按 §2.1 不可直连库）。' +
      '\n    请在部署机上执行，或改用 ./build/oj-reset。');
    return;
  }
  if (!out) return;

  const names = out.split('\n');
  console.warn(`[R5] ⚠️ 发现 ${names.length} 个上一轮遗留的测试账号：\n  ${names.join('\n  ')}`);
  console.warn('[R5]    它们不在 freshUser 的泄漏账本里（崩在中途，finally 没执行到）。\n' +
    '       确认无误后在部署机执行：\n' +
    `         mysql -uoj -poj oj -e "DELETE FROM users WHERE ${where}"`);

  if (process.env.OJ_SWEEP === '1') {          // 显式开关：默认只报不删
    execFileSync('mysql', [...MYSQL, '-e', `DELETE FROM users WHERE ${where}`]);
    console.warn(`[R5] 已清扫 ${names.length} 个孤儿账号（5 张表按 FK CASCADE 一并清理）。`);
  }
}
```

> **为什么默认只报警、不自动删**：谓词是 `LIKE 'ojt%'`，而不是「我这一轮建的那些 id」。
> 它在理论上可能匹配到别人手工建的 `ojt` 前缀账号 —— 测试脚本**自动 `DELETE` 掉一个
> 不是自己创建的账号是不可接受的**，所以删除必须由人确认（`OJ_SWEEP=1`）。
>
> 这与 R4「收尾必须执行」不矛盾，两者管的是不同东西：
> R4 管**知道自己造了什么**的账号（走 `DELETE /api/auth/account`，那步安全，走 API 即可）；
> 这里管**崩了之后不知道自己还剩什么**的残留，只能靠人在环。

#### `global-teardown.js` —— 轮后收口（`WT-AD-11` 的执行体）

```js
const fs = require('fs');
const path = require('path');
const { readBaseline } = require('./helpers');
const { LEAKED } = require('./fixtures');

module.exports = async function globalTeardown(config) {
  const { request } = config;
  const before = JSON.parse(
    fs.readFileSync(path.join(__dirname, 'baseline.json'), 'utf8'));
  const after = await readBaseline(request);

  const drift = [];
  for (const k of ['problemTotal', 'userCount', 'submissionCount']) {
    if (before[k] !== after[k]) drift.push(`${k}: ${before[k]} → ${after[k]}`);
  }
  if (LEAKED.size) drift.push(`未注销账号: ${[...LEAKED].join(', ')}`);

  if (drift.length) {
    // ★ 抛错 = 整轮标记失败。这是有意的：基线漂移是隐性故障，
    //   不让报告"绿着过去"是这一节存在的全部理由。
    throw new Error(`[R4] 轮后基线未复位：\n  - ${drift.join('\n  - ')}`);
  }
  console.log('[R4] ✅ 基线已复位（题库/用户/提交三项与轮前一致）');
};
```

> **为什么漂移要 `throw` 而不是 `warn`**：漂移的典型后果是**下一轮**才暴露 ——
> 这一轮的报告是绿的，下一轮的 `WT-AD-11` 也可能是绿的（因为基线文件已被本轮覆盖），
> 于是问题被永久掩盖。抛错把"这一轮的数据不可信"变成不可忽略的事实。

### 7.6 代表性 spec：`L2-topbar.spec.js`

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

### 7.7 代表性 spec：`L2-editor.spec.js`

> 对应 M06 / `AC-4` / `AC-32`。**两处必须注意**：
> ① Ace 加载后会清空 `#editor` 的内容并移除降级用的 `<textarea>`，因此**必须用键盘输入，不能 `fill()`**；
> ② **草稿键不含用户名且挂载时优先于模板**（§1.5.2 ②），所以 `beforeEach` 里
>    **必须先清草稿、再进页面** —— 顺序反了的话草稿已经在挂载时被读进去了。

```js
const { test, expect } = require('../fixtures');
const { clearDraft } = require('../helpers');

test.describe('@L2 编辑器与草稿 [D-27][D-55] AC-4 AC-32', () => {
  const SLUG = 'two-sum';
  const MARK = '// web-auto-draft-marker';

  // ★ R4：本层每条用例都会往 localStorage 写草稿，afterEach 统一清掉，
  //   否则 WT-ED-03（断言编辑器 == 模板）在本层任何一条之后运行都必挂。
  test.afterEach(async ({ page }) => { await clearDraft(page, SLUG); });

  test.beforeEach(async ({ page }) => {
    await page.goto(`/problem.html?slug=${SLUG}`);
    await page.waitForSelector('#editor.ace_editor');          // 等 Ace 起来
    await clearDraft(page, SLUG);
    await page.reload();                                       // 清完再重载，确保挂载读到空草稿
    await page.waitForSelector('#editor.ace_editor');
  });

  test('WT-ED-01 Ace 加载成功且未降级', async ({ page }) => {
    await expect(page.locator('#editor')).toHaveClass(/ace_editor/);
    await expect(page.locator('#editor-warn')).toBeHidden();
  });

  test('WT-ED-03 预置模板：草稿为空时编辑器 == codeTemplate', async ({ page }) => {
    // 这一条是本层"重跑安全"的试金石：草稿没清干净它就红，且第二次轮必红
    await expect(page.locator('#editor')).not.toContainText(MARK);
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

> ⚠️ 注意骨架里 `WT-ED-05` 的写法：它 `reload()` 之后断言的必须是**本用例刚写的**草稿，
> 而不是"任何草稿"。若照抄成 `expect(编辑器内容).toBeTruthy()`，它就会退化成
> "localStorage 里恰好有东西"，在草稿被上一轮残留的情况下假绿。

#### `WT-LS-21` / `WT-PB-09`：toggle 的归一化写法

> 这两条是全文最容易被写成"只能跑一次"的用例。**对照看**：

```js
const { test, expect } = require('../fixtures');
const { favoriteTo } = require('../helpers');

test.describe.serial('@L2 收藏 [AC-12] AC-24', () => {
  const SLUG = 'two-sum';          // 与 oj-reset 造的收藏基线同一道题

  // ✗ 错误写法：第二次跑这条会红，因为服务端已经把它取消收藏了
  test('WT-LS-21 ✗ 点一次就断言 ★', async ({ user }) => {
    await user.goto('/list.html');
    await user.locator(`button.star[data-star="${SLUG}"]`).click();
    await expect(user.locator(`button.star[data-star="${SLUG}"]`)).toHaveClass(/is-on/);
  });

  // ✓ 正确写法：调到位再断言 + 收尾归位，两头都不依赖上一轮的状态
  test('WT-LS-21 ✓ 归一化到 ★', async ({ user }) => {
    await user.goto('/list.html');
    await favoriteTo(user, SLUG, true);        // 已收藏则一次请求都不发
    await expect(user.locator(`button.star[data-star="${SLUG}"]`)).toHaveClass(/is-on/);
    await expect(user.locator(`button.star[data-star="${SLUG}"]`))
      .toHaveAttribute('aria-pressed', 'true');
  });

  test('WT-LS-25 幂等：连续两次调位只发一次请求', async ({ user }) => {
    await user.goto('/list.html');
    let posts = 0;
    user.on('request', r => {
      if (r.url().includes('/favorite') && r.method() === 'POST') posts++;
    });
    await favoriteTo(user, SLUG, true);
    await favoriteTo(user, SLUG, true);        // 第二次必须是 no-op
    expect(posts).toBeLessThanOrEqual(1);
    await expect(user.locator(`button.star[data-star="${SLUG}"]`)).toHaveClass(/is-on/);
  });

  // 收尾：无论上面几条怎么跑，结束时把基线放回「已收藏」
  test.afterAll(async ({ request }) => {
    await favoriteToViaApi(request, SLUG, true);   // 登录 ZhangLing 后直接调 API
  });
});
```

### 7.8 代表性 spec：`L3-submit.spec.js`（阻塞处理范式）

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

  // ★ R6：WT-SB-07 会打满某个账号 60s 内的配额（每分钟 10 次）。
  //   它必须排在整层**最后**，否则同账号后续提交全部拿到 1006。
  test.describe.configure({ mode: 'serial' });

  test('WT-SB-01 提交后立即进入评测态', async ({ user }) => {
    await user.goto('/problem.html?slug=two-sum');
    await user.click('#submit');
    await expect(user.locator('#status')).toHaveAttribute('data-state', 'judging');
    await expect(user.locator('#status')).toContainText('评测中');
  });

  test('WT-RS-09 CE 不渲染逐用例表，状态行显示未执行数', async ({ freshUser }) => {
    // ★ 整层统一用一次性账号：每条用例都会新增一条提交，
    //   用 ZhangLing 会让 WT-HR-01/02 的提交总数与分页断言全线漂移。
    await freshUser.page.goto('/problem.html?slug=two-sum');
    await freshUser.page.waitForSelector('#editor.ace_editor');
    await freshUser.page.click('#editor .ace_content');
    await freshUser.page.keyboard.type('class Solution { vecto<int> a; };');   // 故意拼错
    await freshUser.page.click('#submit');

    await expect(freshUser.page.locator('.verdict--ce')).toBeVisible();
    await expect(freshUser.page.locator('#status')).toContainText('个用例未执行');  // D-65
    await expect(freshUser.page.locator('#result table.result__table')).toHaveCount(0); // CE 不渲染表
    // 收尾由 freshUser fixture 的 finally 负责：注销 → 级联清掉这条提交
  });

  test('WT-SB-07 每分钟上限：独占账号 + 独占时间窗', async ({ freshUser }) => {
    test.info().annotations.push({
      type: 'quarantine',
      description: '本条独占配额窗口；串行队列中排在 L3 最后（R6）',
    });
    const codes = [];
    for (let i = 0; i < 11; i++) {
      const r = await freshUser.page.request.post('/api/submissions', {
        data: { slug: 'two-sum', language: 'cpp', code: 'class Solution{};' },
      });
      codes.push(r.status());
    }
    // 前 10 次受理，第 11 次被拒
    expect(codes.slice(0, 10).every(c => c === 200 || c === 201)).toBe(true);
    expect(codes[10]).toBe(429);                       // 1006 的 HTTP 映射
    // 收尾：注销即释放该 userId 的配额计数器，不必干等 60s
  });
});
```

> ⚠️ `WT-SB-07` 的写法里那行 `test.describe.configure({ mode: 'serial' })` 不是可有可无的。
> 它和"用一次性账号"是**两个独立的保险**：账号隔离保证配额不外溢到别的用例，
> 串行 + 排末尾保证**它自己**不落在中间被别人依赖。
> 另外注意它现在**必然失败** —— 限流还没接线（`ratelimit.cpp` 0 字节，见 §M07 末尾提醒），
> 第 11 次会返回 `503/1009` 而不是 `429`。接线当天这条会突然从"红"变"绿"，
> 那正是它写对了的证据。

### 7.9 代表性 spec：`L1-anonymous.spec.js`

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

### 7.10 运行

```bash
cd tests/web
npm install
npx playwright install chromium

export OJ_BASE=http://82.156.75.119:8080

npx playwright test --project=chromium              # 全量
npx playwright test --project=chromium --grep @L0  # 只跑静态可达性
npx playwright test --project=chromium --grep @L2  # 只跑无需 judge 的闭环
npx playwright show-report                          # 看 HTML 报告

# ★ v1.1：连续三轮必须逐条一致 —— 这是本层的验收命令（§1.5.5）
npx playwright test --project=chromium \
  && npx playwright test --project=chromium \
  && npx playwright test --project=chromium

# CI / 复现：固定 runId，三轮用**同一个** id，残留可按前缀精确清扫
export OJ_RUN_ID=ci042
npx playwright test --project=chromium

# 确认 globalSetup 报出的孤儿账号确实该删之后，再显式开清扫
export OJ_SWEEP=1
npx playwright test --project=chromium
```

| 环境变量 | 作用 | 默认 | 谁需要设 |
|---|---|---|---|
| `OJ_RUN_ID` | 固定本轮 runId（替代时间戳） | `Date.now().toString(36)` | CI、复现现场；本地随手跑留空 |
| `OJ_SWEEP=1` | 允许 `globalSetup` **真的删**孤儿账号 | 只报警不删 | 人工确认过 `globalSetup` 打印的账号列表之后 |
| `OJ_BASE` / `OJ_USER` / `OJ_PASS` / `OJ_ADMIN` / `OJ_ADMIN_PASS` | 环境与账号 | 见 §2.1 / §2.2 | 全部 |
| `OJ_USER` 的取值约束 | **必须是 `oj-reset` 造的基线账号**（`ZhangLing`） | — | 换别的账号时注意 `WT-LS-17` / `WT-MG-*` 的基线会变 |

> 判读第二轮与第三轮时**先看 `baseline.json` 有没有变**（`globalSetup` 每次覆盖它，
> 所以要看的是报告里 `[R5] ⚠️ 基线漂移` 那几行有没有出现），
> 再看 `globalTeardown` 有没有抛 `轮后基线未复位`。
> 这两处都干净，才说明「三轮全绿」是真的全绿，而不是基线已经烂掉之后的绿。

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

# ── 2. 轮前重置（R5：把基线按构造恢复）──────────────────
#    只在「要跑依赖绝对数字的用例」时需要；日常增量回归可跳过，
#    代价是 WT-LS-01/04、WT-IN-03、WT-MG-01/02/04 这批用 baseline 断言，风险自担。
#    前置条件见 §2.4：CWD 必须是项目根目录、mysqld 在跑、11 张表已存在。
pkill -f 'build/oj' ; ./build/oj-reset      # ≈2s（停 oj → 清库造数）
make run                               # 重启 oj：它持有进程内题库缓存，不重启读到旧题库
                                       # （本机实测：oj 未装 systemd unit，用 make run）

# ── 3. 前端回归（本文档）───────────────────────────────
cd tests/web && npm install
npx playwright test --project=chromium

# ── 4. 只跑当前环境能跑的两层（judge 未接线时的日常）─────
npx playwright test --project=chromium --grep '@L0|@L1|@L2'

# ── 5. ★ 三轮一致性验收（本层的硬性标准，§1.5.5）─────────
npx playwright test --project=chromium \
  && npx playwright test --project=chromium \
  && npx playwright test --project=chromium
# 判读：① 每条用例的 pass/fail/skip 三轮一致
#       ② globalTeardown 三轮都**没有**抛「轮后基线未复位」
#       ③ 报告里没有出现 [R5] ⚠️ 基线漂移

# ── 6. 兼容性（发布前跑一遍，约 3 倍耗时）─────────────────
npx playwright test                      # chromium + edge + webkit
```

### 8.1 「第二次才失败」的自查清单

> 提交新用例前对着这 12 条过一遍，比跑三轮再回头查快得多。

| # | 自查项 | 怎么自查 |
|---|---|---|
| 1 | 新建的账号/题/提交名字里带 `runId` 了吗 | grep 用例里的字面量用户名，看有没有硬编码 |
| 2 | 注册类用例收尾注销了吗 | 有 `DELETE /api/auth/account` 或 `finally` |
| 3 | toggle 类断言写的是"调到 X"还是"点一次" | 有没有用到 `favoriteTo` |
| 4 | toggle 收尾归位了吗 | 用例末尾有没有 `favoriteTo(slug, 基线)` |
| 5 | 写草稿的用例清了草稿吗 | 有没有 `clearDraft` |
| 6 | 断言里有没有裸的绝对数字 | grep `34 条` / `共 26` / `共 7 人` / `5/13` |
| 7 | 用例依赖的前置数据是它自己造的吗 | 有没有写"先跑 X 用例" |
| 8 | 会触发限流的用例隔离了吗 | 是不是一次性账号 + 排末尾 |
| 9 | 会改密码的用例和别的用例共享账号吗 | 改密后原密码失效，共享必错 |
| 10 | 探针题/越权造数清了吗 | 有没有 `afterAll` + `globalSetup` 兜底 |
| 11 | 新建的名字前缀是 `ojt` 而不是 `wt` 吗 | `wt` 会在 R5 清理时误伤真人手工建的账号 |
| 12 | 崩在中途的残留谁负责发现 | `globalSetup` 的孤儿清扫（默认只报警，`OJ_SWEEP=1` 才删） |

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
| **第一次全绿，第二次一半红** | 典型的残留型失败 | 见下方 §9.1 专项表；根治靠 §1.5 六条规则，不是靠手动清数据 |
| **三次都绿，但 `共 7 人` 变成了 `共 23 人`** | 隐性故障：一次性账号没注销 | 查 `globalTeardown` 的泄漏账本；把该用例改成 `freshUser` fixture |
| 注册类用例第二次报 `1005` | 用户名写死，重名 | 用 `ojt<runId><seq>`（R1）；`runId = OJ_RUN_ID ?? Date.now().toString(36)` |
| 相隔半小时重跑，runId 撞了 | base36 末 4 位每 ≈28 分钟才进位一次 | 用**完整** base36，别 `.slice(-4)`（§7.4） |
| `[R5] ⚠️ 发现 N 个上一轮遗留的测试账号` | 上一轮崩在中途，`finally` 没执行到 | 看打印的列表确认后 `OJ_SWEEP=1` 重跑，或在部署机按给出的 SQL 手删 |
| 孤儿列表里混进了不该删的账号 | 谓词是 `LIKE 'ojt%'`，不是「本轮建的 id 列表」 | 默认只报警不删正是为此，别无脑加 `OJ_SWEEP=1` |
| `WT-ED-03` 从第二轮起红 | 草稿残留覆盖了模板 | 确认用例的 `beforeEach` 里有 `clearDraft`；注意**清草稿要在进页面之前或 reload 之后** |
| `WT-LS-22` / `WT-LS-17` 的收藏数莫名少 1 | `WT-LS-21` 点第二次把收藏取消了，且**服务端删掉了整行 `user_problems`**，连带进度行 | 用 `favoriteTo` 归位；不要用"点一次"（§1.5.2 ①） |
| 提交类用例在限流接线当天集体变红 | `WT-SB-07` 吃满了 `ZhangLing` 的 60s 配额 | 改一次性账号 + 排串行末尾（R6） |
| `WT-HR-01` 的提交总数每轮 +N | 判题类用例没有收尾删除 | 每条用一次性账号，或显式删除造出的记录 |
| 题库题数从 26 变 27 | `WT-PB-03/04` 的探针题没删 | `./build/oj-reset`；并确认 `globalSetup` 的残留告警已接上（§2.5.5） |
| 某题页一直显示"评测中…"不结束 | `sessionStorage['judging:<slug>']` 残留 | `clearDraft` 里已含这一项；手工清：`sessionStorage.clear()` |
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

### 9.1 「第一次通过、第二次失败」专项对照表

> 这张表是 §1.5 的**反面索引**：按症状直接定位到根因与修法。
> 左列全部是实际会遇到的报错/红灯，右列是**机制**而非"清一下数据就好"。

| 症状 | 根因机制 | 修法 | 对应用例 |
|---|---|---|---|
| 注册返回 `1005 用户名已注册` | `uk_username` 唯一键（`01-schema.sql:48`） | 名字带 `runId` | `WT-AU-09/21/22` |
| 收藏断言在 `☆` 与 `★` 之间反复横跳 | 硬 toggle，且请求不带目标态（`user_api.cpp:161-189`） | `favoriteTo(slug, want)` | `WT-LS-21/25`、`WT-PB-09` |
| 收藏数少 1，且**进度行也没了** | 取消收藏走的是 `DELETE FROM user_problems`，整行消失 | 同上，收尾归位 | `WT-LS-22` |
| 编辑器内容是上次的草稿而不是模板 | 挂载时 `draft` 优先级高于 `codeTemplate`（`problem.js:243-244`） | `clearDraft` + reload | `WT-ED-03` |
| 多标签页冲突用例不弹框 | B 页挂载时已载入旧草稿 → `newValue` 未变 → `storage` 事件不触发 | 先清草稿再开两页 | `WT-ED-06` |
| 将来某天每轮都莫名弹冲突框 | 上轮 `WT-GD-06` 留了未来 `draftAt` | 收尾删 `draftAt` | `WT-ED-07`、`WT-GD-06` |
| 改密后同轮其他用例登录失败 | 改密是**不可逆**的，别的用例还拿旧密码 | 改密用例独占账号 | `WT-AC-05/07` |
| 分页断言算错 | 每轮新增的提交把 `total` 顶过了 20 的倍数 | 基线相对 + 一次性账号 | `WT-HR-01/02` |
| 越权用例给对手账号堆了一堆提交 | B 复用了 `ZhangLing` | A/B 都是一次性账号 | `WT-SX-02`~`06` |
| 性能用例 P95 虚高 | 排队等 `1006` 重试的时间算进了延迟 | 账号轮转 / 调高 `OJ_RATE_MAX_SUBMITS` | `WT-NF-07/09/10` |

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
| **限流（`WT-SB-06/07/13`）** | `ratelimit.h`/`ratelimit.cpp` **0 字节**，`rate_max_submits_per_window=10` 与 `judge_user_gate=2` 无人消费，`1006`/`1007` 不可达 | 配额类用例**现在验不了**；接线当天它们会同时从"红"变"绿"。**隔离写法必须提前写好**（R6），否则接线那一刻会连带打挂同轮的其他提交类用例 |
| **幂等性的自动化验收** | §7.5 的 `helpers.js` / `global-setup.js` / `global-teardown.js` 仍是**骨架**，仓库中尚无 `tests/web/` 目录 | §1.5 的六条规则目前靠人读文档遵守。**在骨架落地并跑通三轮之前，本文所有 🔁 用例都只能算"设计完成"，不能算"可回归"** |

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
