# SPEC — CppOJ：面向求职面试的在线判题系统

| 项 | 值 |
|---|---|
| 版本 | v1.9（httplib 版本对齐：改用本机已装的 header-only 0.60.1，修订 `[D-72]`） |
| 日期 | 2026-10-11 |
| 状态 | 规格已确认，进入实施 |
| 交付周期 | 3 周 |

---

## ⚠️ 0.0 开发机硬约束（开工前必读，优先于本文档其余全部内容）

> **这份约束的读者是"每次开新窗口的任何人/任何 agent"**，所以写在最前面而不是附录里。

### 实测资源

| 项 | 实测值 | 来源 |
|---|---|---|
| 开发机 | Ubuntu 24.04，**2 核 / 1967MB** | §6.7、`[D-74]` 的下调依据 |
| 可用内存 | mysqld 常驻后常只剩 **300MB 上下** | 实测 `free -m` |
| 本项目规模 | `ojcore` 22 个翻译单元 + `oj` + 7 个 gtest 目标 | 根 `CMakeLists.txt` 的显式源列表 |
| 带 sanitizer 时 | 单个 TU 峰值内存显著更大（ASan 影子内存） | 实测 |

### 规则：**编译并发上限 = 2；带 sanitizer 时 = 1**

| 场景 | 命令 | 说明 |
|---|---|---|
| 日常编译 | `make build` | Makefile 内部已固定 `cmake --build build -j2` |
| 手写编译 | `cmake --build build -j2` | **不要**省略 `-j2`，也不要写 `-j`（=核数）或更大 |
| 单测 | `make test` / `ctest --test-dir build` | ctest **不加 `-j`**：274 条串行 30 秒，并发只会与 DB/构建抢资源 |
| ASan / UBSan | `cmake --build build-asan -j1` | 另需 `ASAN_OPTIONS=detect_leaks=0`（本机 LSan/TSan 受限，见根 `CMakeLists.txt` 注释） |
| 只重新配置 | `cmake -S . -B build` | 无编译步骤，可随意并发 |
| 临时降并发 | `make build JOBS=1` | 不必改 Makefile |

### 为什么不是"核数=2 所以 `-j2` 刚好"

**卡住的不是 CPU，是内存。** 2 核实测下 `-j4` 尚可，但**带 sanitizer 的构建 `-j4` 会挂住**（表现为长时间无输出、最终超时），`-j1` 正常。原因是 ASan 的影子内存按进程翻倍，mysqld 常驻后 300MB 余量无法同时容纳 4 个被插桩的编译器进程 —— 表现是 swap 抖动或直接被 OOM killer 干掉，而现象是"构建卡死"，很容易被误判成"编译器有 bug"。

> **这条与 `[D-74]`（worker 2 / 闸门 4）同源：都是按实测机器定，不是按通用经验定。** v1.0 假定 4G 机器时给的 `-j`、`worker 4`、`闸门 8` 全部偏大，落地到 2G 后被逐项下调；编译并发就是这一轮补上的最后一项。
>
> **症状 → 处置对照**（遇到时先查这张表，再查代码）：

| 症状 | 最可能的原因 | 处置 |
|---|---|---|
| 构建长时间无输出 / 工具超时 | 并发过高（尤其 sanitizer） | 降到 `-j1`，重来 |
| ctest 报"找不到 ./problems" | 工作目录不对 | ctest 已固定 `WORKING_DIRECTORY=仓库根`；手动跑测试要从仓库根执行 |
| 单测大面积超时 | 与 `mysqld` / 构建并发抢资源 | 先让构建结束；测试不加 `-j` |

---

## 0. 决策溯源

本文档由 75 项访谈决策收敛而成。每条标注 `[D-xx]` 便于回溯与验收对照。

| # | 决策 | # | 决策 |
|---|---|---|---|
| D-01 | 定位：求职面试备战工具 | D-19 | 队列：进程内线程池 + 内存队列 |
| D-02 | 成功标准：功能闭环可用 | D-20 | 重启兜底：judging 置失败、pending 重新入队 |
| D-03 | 部署：单机 Docker Compose | D-21 | 并发：httplib 线程池 + 互斥量串行化 DB |
| D-04 | 隔离：fork + setrlimit | D-22 | 防护：应用层限流 + 配额 |
| D-05 | 范式：LeetCode 式 tester.h | D-23 | 鉴权：用户名密码 + Session Cookie |
| D-06 | 绑定：固定 harness + 链接用户 .o | D-24 | 密码哈希：bcrypt |
| D-07 | 用例来源：题库 JSON → 生成 tester.cpp | D-25 | 前端：原生三件套 + CDN CodeMirror |
| D-08 | 用例类型：按题声明 + 结构化字面量 | D-26 | 布局：题目页左右分栏 |
| D-09 | 反馈粒度：逐用例全量详情 | D-27 | 草稿：localStorage 自动保存 |
| D-10 | 判题模式：异步任务 + 前端轮询 | D-28 | 提交：预置模板 + 提交前校验 |
| D-11 | 资源限制：贴近 LeetCode 严格值（**⚠️ 内存为 AS 口径，比 LeetCode 的 RSS 口径更严，见 §5.5.1 `[D-63]`**） | D-29 | 错误码：数字业务码 + 同步 HTTP 状态码 |
| D-12 | 度量：CPU 时间 + ru_maxrss | D-30 | 题库文件为源，启动时同步入库 |
| D-13 | 工具链：g++ 12 / C++17 / -O2 -Wall | D-31 | 提交历史：代码全文 + 逐用例结果全存 |
| D-14 | 题面：Markdown + 前端渲染 | D-32 | 前端：多页应用 |
| D-15 | 分类：难度 + 算法标签 + 来源标签 | D-33 | 容器：mysql + oj 双容器 |
| D-16 | 进度：由提交历史自动推导 | D-34 | 日志：应用日志 + judge 日志双文件 |
| D-17 | 时间：存 UTC + 前端本地化 | D-35 | 历史页：全局页 + 题内最近 5 次 |
| D-18 | 存储：MySQL | D-36 | 页面：login / list / problem / history 四页（`D-37` 追加 `admin.html`，共五页） |
| — | — | D-37 | 角色：`OJ_ADMIN_USERNAMES` 声明式名单，启动时同步；1 个只读 admin 端点 + 极简管理页 |
| — | — | D-38 | 首页：`list.html` 兼落地页，顶部加个人概览条；**不新增首页** |
| — | — | D-39 | 个人主页**合并进** `history.html`（三 tab：提交记录 / 掌握分布 / 账户设置）；顶栏入口改名「我的」 |
| — | — | D-40 | 账户管理**只做改密码**：验旧密码 + 踢掉其他会话（保留当前会话） |
| — | — | D-41 | 顶栏**常驻显示角色徽标**；`admin.html` 仍严格只读，**D-37 不被推翻** |
| — | — | **D-42 ~ D-61（v1.4 需求补全轮）** | |
| D-42 | 同题两次提交**并排 diff**（行级高亮） | D-52 | 顶栏**同一组件条件渲染**两套形态（未登录/已登录） |
| D-43 | 未登录**可读题面**，不可提交/收藏/看统计 | D-53 | 提交 id 存 **sessionStorage**，页面加载时自动恢复轮询 |
| D-44 | 会话过期重登后**自动恢复草稿 + 提示** | D-54 | 提交时**告知队列位置** |
| D-45 | 基于薄弱标签的**规则推荐「下一题」** | D-55 | 多标签页草稿冲突 → **检测到冲突就问用户**，不自动覆盖 |
| D-46 | 多账号**不特殊处理**（无会话列表、不预填用户名） | D-56 | **账号注销**，需输密码确认 |
| D-47 | 题目**方向提示 `hint_md`**，免费无限看 | D-57 | 注销 = **硬删除**（CASCADE 级联），不可恢复 |
| D-48 | 题面**禁图片**，树/图题用字符画 | D-58 | 重跑失败的提交 = **新增一条**记录，不覆盖原记录 |
| D-49 | 可**删除**自己的提交 + 可**重跑**失败的提交 | D-59 | 「下一题」推荐卡片放**题目页底部**，**带可解释理由** |
| D-50 | diff 视图在**题目页内**，勾选两条展开 | D-60 | 队列位置语义 = **「你前面还有 N 个」**，不泄露全局长度 |
| D-51 | 草稿**不特殊处理**（50 题 ≪ 5MB 上限） | D-61 | 提交历史默认**时间倒序 + 20 条/页** |
| — | — | **D-62 ~ D-70（v1.5 对抗性审计修复轮）** | |
| D-62 | 阻塞判定改为 **CPU 停滞检测**（增量为 0 才判阻塞） | D-66 | **0 条 `@@OJ@@` → `JE`**（judge 错误，不判 RE） |
| D-63 | 内存**保留严格值**，明确为 `RLIMIT_AS` 口径并标注 | D-67 | `failed` 纳入"0 明细行"不变量 + 断言扩展 |
| D-64 | 平台生成文件的编译错误**归属题库**，不提供行高亮 | D-68 | 推荐算法的**主标签选取规则**（取最薄弱标签） |
| D-65 | **CE 时 `case_total` = 题库用例数**，非 0 | D-69 | CSRF 论述补齐（依赖 `SameSite=Lax` 隐式防护） |
| — | — | D-70 | 注销时清理内存限流计数器 |
| — | — | **D-71 ~ D-75（v1.6 环境落地轮）** | |
| D-71 | 部署走**路线 A：宿主机原生构建**；Docker 降为可选目标形态 | D-73 | 前端三库 **vendored** 到 `static/vendor/`，不再运行时引 CDN |
| D-72 | cpp-httplib 用**本机已装的 header-only 0.60.1**（`/usr/local/include/httplib.h`）`[v1.9 修订]` | D-74 | 按实测资源下调：**worker 2 / 全局闸门 4** |
| — | — | D-75 | 密码哈希走 **OpenSSL Blowfish**（24.04 官方源无 `libbcrypt-dev`） |

> **D-38 ~ D-41 的共同取向：只加数据，不加页面。** 个人主页所需的统计、分类掌握度、账户信息三类需求，数据全部已存在于 `submissions` / `users` / `problem_tags`，**因此不需要新表、不需要新列、不需要新页面** —— 只缺聚合端点与展示位。故 `§3.2` 的 DDL 在 v1.3 **完全不变**，页面总数仍为五页。

> **D-42 ~ D-61 是需求补全轮**，起因是逐条复核 v1.3 对本文档开头五项页面需求的**需求层**（而非设计层）覆盖度，发现 20 个空白 —— 大多是"线框里画了但需求层没定义"（如提示 tab）、"功能做了但异常路径没定义"（如会话过期时的草稿）、"新功能带出的新语义没定义"（如重跑的结果怎么落）。这一轮的取向是：
>
> | 取向 | 体现 |
> |---|---|
> | **补需求，不扩架构** | 20 项决策**没有一项引入新表**；DDL 仅新增 `problems.hint_md` 一列（D-47）。新增能力全部复用既有 11 张表 |
> | **异常路径优先于新功能** | D-44（草稿恢复）、D-53（刷新恢复轮询）、D-55（草稿冲突）三条都不是"新功能"而是"已定功能在异常下的行为"，此前完全空白 |
> | **每项都带明确的"不做什么"** | D-46（不做会话列表）、D-48（禁图片）、D-51（不清理草稿）、D-57（不做软删除）—— 与 v1.3 一贯的 conscious omission 风格 |

> **D-62 ~ D-70 是对抗性审计修复轮**。做法是对 v1.4 做一次"假设它会出错"的推演，找出 **4 个会必然产生错误判定的硬漏洞**（§8.1.1 详列）与 6 个文档不一致/未定义。修复的一处通则值得记下：
>
> > **多数漏洞的根源不是"没做"，而是"两处各自合理的设计放在一起就矛盾"。** 例如墙钟保险阀（`time_limit_ms × 2 + 1s`）与全局并发闸门 8 各自都说得通 —— 前者想卡住 `sleep(999)`，后者想用满 CPU —— 但 2 核 8 并发下墙钟被放大 8 倍，**前者会系统性地杀死后者正在拼命工作的合法进程**。这类问题读文档时看不出来，必须把两个数字代进去算一遍才暴露。

> **D-71 ~ D-75 是环境落地轮**。起因是"把 SPEC 拿到真实机器上跑"时做了一次依赖清点，暴露出**三处文档假设与现实不符**：
>
> | 文档假设 | 实测 | 处置 |
> |---|---|---|
> | D-33 双容器是唯一部署形态 | Docker 已装但**用户不在 `docker` 组** → socket `permission denied` | `D-71` 路线 A 为主，B 保留 |
> | §6.2 依赖表有 bcrypt | **24.04 官方源无 `libbcrypt-dev`** | `D-75` 走 OpenSSL Blowfish |
> | §6.2「CodeMirror 5」一条依赖 | 实测 `codemirror.min.js` **不含任何语言模式**（grep `clike` = 0） | `D-73` 拆成 5 个文件条目 |
>
> 附带两处按实测环境做的选型调整：`D-72`（cpp-httplib 锁 apt 版）与 `D-74`（worker/闸门按实测资源下调）。完整清单与安装命令见 **§6.7**。
>
> **这一轮的取向**：环境问题**不回头改架构**。DDL、判题流水线、API 契约、错误码**全部零变更** —— 路线 A 与路线 B 的差别只在"谁负责拉起 mysqld 与 g++"，不触及任何业务逻辑。
>
> **v1.7 是对 v1.6 依赖清单的补正**（不新增决策）。起因是把 §6.7.2 的清单放到"空白 Ubuntu 24.04"上逐条走了一遍，发现 v1.6 隐含假设了一台**已配置的机器**：
>
> | 缺口 | 后果 | v1.7 处置 |
> |---|---|---|
> | 未列 `ca-certificates` | 阶段 ④ 的 `curl -sSL` 失败，报错像"证书过期" | 补为阶段 ① 必需 |
> | 未列 `curl` / `git` | 空白系统上根本没有下载器与版本库 | 补为阶段 ① 必需 |
> | 未列 `mysql-client` | `run_tests.sh` 跑不了 §3.4 的断言 SQL | 补为阶段 ④ 必需 |
> | 未显式列 `libc6-dev` | judge 的 `getrusage`/`wait4`/`prctl` 头文件靠它（虽由 `g++-12` 传递带入） | 显式列出并说明传递链 |
> | 列了 `cmake`/`pkg-config` | 与 T0.1 已定的 Makefile 并存，徒增困惑 | 移入「⛔ 不装」并写明理由 |
> | 建议过 `build-essential` | 会连带装 g++-13，与 D-13 冲突 | 移入「⛔ 不装」 |
>
> **净变化：+5 必需项、−3 不需要项，决策数不变（仍 75 项），DDL 与架构零变更。** 另新增 **A.6 的 7 项装完验证**，其中第 ⑦ 项（fork+setrlimit+prctl+wait4）是 §5 判题机制的最小验证点。
>
> > **⚠️ 上面第 5 行的处置已被 v1.8 推翻**（`cmake` 回到「必装」）：见下节。
>
> **v1.8 是构建入口轮**（不新增决策，仍 75 项）。起因是 M0 实际开工时发现 T0.1 的 `Makefile` 一直空着，而"手写 Makefile"与"用 CMake"在本项目里并不是两种风格之争，是**同一份工作量的两种写法**：
>
> | 维度 | 手写 Makefile | CMake（v1.8 取此） |
> |---|---|---|
> | 依赖探测 | 自己写 4 个库的路径判断（httplib / nlohmann / mysqlclient / libcrypto）+ gtest | `find_package` + apt 自带的 CONFIG 包（nlohmann 3.11.3、GTest 1.14 均带） |
> | 每加一个测试 | 改编译行 + 链接行 | 测试文件自动成为 target |
> | 测试运行 | 自己写过滤与失败输出逻辑 | ctest 免费给 `-R` / `--gtest_shuffle` / `--output-on-failure` |
> | 每加一个 `.cpp` | 改两处（core 源列表 + 测试链接） | 改一处 |
>
> **被推翻的那条理由**：v1.7 说"cmake 与 Makefile 并存只会让人不知用哪个"。v1.8 让 `CMakeLists.txt` 成为**唯一**构建规则、`Makefile` 降为 15 行转发封装（`make build/run/test/clean` 契约不变），"并存"不复存在，该理由随之作废。
>
> **取向：只换构建工具，不碰任何设计。** DDL、判题流水线、API 契约、错误码、线程模型**全部零变更**；`cmake` 只是从「⛔ 不装」移回「必装」。
>
> **v1.9 是 `[D-72]` 的修订**（不新增决策，仍 75 项）。起因是 v1.8 写 CMake 时实测到：
>
> ```
> $ dpkg -l libcpp-httplib-dev      → 未安装
> $ ls /usr/include/httplib.h       → 不存在（apt 路径是空的）
> $ grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h
> #define CPPHTTPLIB_VERSION "0.60.1"
> ```
>
> | v1.6~v1.8 的假设 | 实测 | v1.9 处置 |
> |---|---|---|
> | httplib 取 apt 的 **0.14.3**，锁版本是为了消除"宿主编过、容器编不过" | 本机实际在用的是**自行安装的 `/usr/local/include/httplib.h` 0.60.1**，apt 包从未安装 | 真源改为 0.60.1；`libcpp-httplib-dev` 从「必装」移入「明确不装」 |
> | 路线 B 镜像装 apt 0.14.3 即可与宿主一致 | 宿主已不用 apt 版 | 路线 B 必须把**同一份 0.60.1 头文件**放进镜像，否则 D-72 要消除的漂移会原样复发 |
>
> **为什么直接改版本而不回退到 0.14.3**：header-only 单文件库装进 `/usr/local/include` 只有一行 `install`，回退反而要额外装一个 apt 包；而且 T0.5 尚未接线 httplib，**现在改口径的代价是零，等接线后再改就要动源码与镜像两处**。这正是 §0 v1.6 那句"环境问题必须在写业务代码前暴露"的又一次兑现。
>
> **不变的部分**：`#include <httplib.h>` 的写法不变（§6.2 尾注那条"源码零差异"继续成立），`CPPHTTPLIB_OPENSSL_SUPPORT` 继续关闭（单机 HTTP，§6.2），`third_party/httplib/httplib.h`（0.14.3）**降级为环境缺失时的兜底**，不再是版本真源。

---

## 1. 需求

### 1.1 背景与目标

个人在算法面试备战过程中，需要一个能离线、稳定地反复刷题的判题环境。市面上的通用 OJ 要么太重（部署与学习成本高），要么功能缺失（无逐用例反馈、无历史复盘）。

本项目交付一个**单机可运行**、**功能闭环**的 C++ 在线判题系统，核心价值是：

1. 逐用例全量反馈，让用户精确定位错误边界
2. 提交历史与草稿保留，支撑面试前的复盘与二刷
3. 架构本身在技术面试中可讲述（fork 隔离原理、代码生成器设计、线程模型）

### 1.2 成功标准 `[D-02]`

3 周后交差标准为**功能闭环可用**，即下列链路端到端无致命 bug：

```
注册 → 登录 → 筛选题目 → 查看题面 → 编辑代码 → 提交
     → 轮询看到逐用例结果 → 回看提交历史 → 再次尝试
```

**非目标（首版明确不做）**：多语言执行、竞赛/排行榜、实时推送、题解社区、支付、管理后台、移动端原生适配、真沙箱安全隔离、**独立个人主页页面**（统计/分类/账户合并进 `history.html` 的 tab，见 `[D-39]`）、找回密码、修改用户名、登出其他设备、**多账号会话快速切换** `[D-46]`、**题面图片**（树/图题用字符画，`[D-48]`）、**账号软删除/匿名化**（注销即硬删，`[D-57]`）、草稿容量淘汰策略 `[D-51]`、代码在线题解/讨论区。

> **关于"管理后台"的澄清** `[D-37]`：v1.2 引入**最小角色体系** —— `users.role` 仅 `admin`/`user` 两值，管理员由 `.env` 的 `OJ_ADMIN_USERNAMES` **声明式指定**（启动时同步，见 §6.3.1），并配**一个只读**管理端点 `GET /api/admin/overview` 与**一个极简**页面 `admin.html`。这**仍然不是管理后台**：没有管理界面上的增删改、没有题库在线编辑、没有禁用/删除用户、没有 impersonation。角色可反复调整，但**调整途径是改配置 + 重启**，不是点按钮 —— 这与 §2.2 既定的「运行时改题需重启」运维模式一致，不引入第二套配置哲学。

### 1.3 核心用户流程

| 流程 | 描述 |
|---|---|
| P1 注册登录 | 用户名 + 密码注册，登录后获得 HttpOnly Cookie，会话有效期 7 天 |
| P2 找题 | 首页列表：按关键词、难度、算法标签、来源标签、我的进度（未开始/尝试过/已通过）筛选与排序 |
| P3 刷题 | 打开题目页，左侧 Markdown 题面（含示例、约束、提示），右侧编辑器（预置模板）+ 提交按钮 |
| P4 提交判题 | Ctrl/Cmd+Enter 提交 → 立即返回 submissionId → 前端每 1.5s 轮询 → 进度条/状态实时刷新 |
| P5 看结果 | 状态徽标（通过/失败/超时/内存超限/运行错误/编译错误）+ 可折叠的逐用例表格（编号/输入/你的输出/期望/耗时/内存） |
| P6 复盘 | 「我的」页（`history.html`）三个 tab：**提交记录**（全局检索 + 筛选 + 展开详情）、**掌握分布**（按难度/标签的已通过统计）、**账户设置**（改密码 + 登出）。题目页底部显示该题最近 5 次提交 |
| P7 收藏 | 题目页右上角星标切换收藏，列表页可按"仅看收藏"筛选 |
| P8 看进度 | 首页顶部概览条：已通过 X/N、按难度三个迷你进度条、最近 3 条提交；与「我的」tab2 **同源同数值** `[D-38]` |
| P9 改密码 | 「我的」tab3：输入旧密码 + 新密码 → 验证通过后改密并**踢掉其他设备的会话**（当前会话保留，改密后不丢失登录态）`[D-40]` |
| P10 未登录浏览 `[D-43]` | 未登录可读**全部题面**（相当于"题库阅读"）；提交按钮显示为「登录后提交」；收藏星标显示为描边态，点击弹登录框并记下当前题目路径 |
| P11 对比复盘 `[D-42] [D-50]` | 题目页底部「你的最近提交」勾选任意两条 → 上方展开并排 diff，行级高亮 `+`/`-`，可一键把任一侧代码**回填到编辑器** |
| P12 接着刷下一题 `[D-45] [D-59]` | 题目页底部「下一题建议」卡片：给出题名 + **可解释理由**（如"链表 · 你在此标签下 2/5 通过"）+ 一键跳转。已全部通过时改为展示完成度而非推荐 |
| P13 删除与重跑 `[D-49] [D-58]` | 「我的」tab1 每条可**删除**（仅本人）；`failed`/`JE` 状态的条目额外提供**重跑**，重跑**新增一条**记录而非覆盖原记录 |
| P14 注销账号 `[D-56] [D-57]` | 「我的」tab3 底部：输密码 → 二次确认弹窗（列明"提交历史将一并删除且不可恢复"）→ 硬删 `users` 行（级联清会话/提交/收藏）+ 清本机该用户草稿 → 回登录页 |

> **P1 的角色部分不构成"切换"流程** `[D-41]`：普通用户与管理员之间**没有页面上的一键切换**。管理员身份的变更途径只有一条 —— 改 `.env` 的 `OJ_ADMIN_USERNAMES` 后重启（§6.3.1）。用户在页面上的动作只有两个：**切换账号**（登出后换一个人登录）与**看见自己是谁**（顶栏常驻角色徽标）。`admin.html` 因此保持**严格只读**，D-37 的核心论证不被推翻。
                     
### 1.4 非功能需求

| 维度 | 要求 |
|---|---|
| 性能 | 页面加载 < 1s；提交到拿到结果 P95 < 8s（10 用例以内）；列表查询 P95 < 200ms |
| 并发 | 支持 10 个用户同时提交；**全局同时评测数硬上限 4** `[D-74]`；单用户同时评测数上限 2 |
| 可靠性 | 服务重启后不出现永久"评测中"的任务；judge 进程崩溃不拖垮主进程 |
| 安全 | 用户代码不可读文件系统（除自身工作目录）、不可无限 fork、不可超限占用内存/CPU/磁盘；所有 SQL 走预处理语句；注册可一键关闭（`OJ_ALLOW_REGISTER=0`）；管理员由 `.env` 的 `OJ_ADMIN_USERNAMES` 声明（`users.role`），`/api/admin/*` 需 `role='admin'` |
| 可观测 | 应用日志与 judge 日志分文件、按天切割、保留 7 天 |
| 成本 | **双档** `[D-74]`：**功能验证 ≥ 2G**（2 核实测机）/ **性能验收 4G**（NF-1 ~ NF-4 与 AC-38 必须在 4G 机器上跑，见 §6.7.1）。无额外中间件（无 Redis / MQ） |
| 兼容性 | 桌面 Chrome/Edge/Safari 最近两个大版本；1280px 以上宽度 |

> **v1.3 补充的两条非功能约束** `[D-40]`：改密码端点必须做到「**改密后当前会话仍有效**」（否则用户把自己踢下线，体验等同于登出失败）；`GET /api/me/overview` 的聚合成本必须让首页概览条保持 **P95 < 200ms**，因此该端点**不得**为了图省事而全表捞 `submissions` 回应用层聚合（聚合必须在 SQL 内完成，理由见 §6.5）。

> **v1.4 补充的三条非功能约束**：
>
> 1. **diff 视图 P95 < 100ms** `[D-42]`。两份代码各 ≤ 64KB（§5.8），LCS 行级 diff 最坏 O(n·m) ≈ 4000×4000 行级比较，须在**前端内存内**完成（不上服务端），且 64KB 上限本身即是 diff 的性能护栏 —— 超过 64KB 的代码根本进不来。
> 2. **注销必须"不可逆"这件事对用户可见** `[D-57]`。硬删意味着误操作无法挽回，因此二次确认弹窗必须**逐项列出**将被删除的数据（提交 N 条 / 收藏 M 题 / 会话 K 个），而不是一句"确定吗？"。这是硬删除方案能成立的**唯一前提**。
> 3. **队列位置不得泄露全局负载** `[D-60]`。只返回"你前面还有 N 个"（你自己在队列中的序号），不返回全局队列长度、不含任何其他用户信息。理由见 §6.3.6。

> **v1.6 的资源下调声明** `[D-74]`：目标开发机实测 **2 核 / 1967MB**（§6.7），而 v1.0 假定的 4G 机器上 `worker 4 × 闸门 8` 会让 judge 与 HTTP 线程争抢内存。处置：
>
> | 参数 | 原值 | 现值 | 依据 |
> |---|---|---|---|
> | Judge Worker 数 | 4 | **2** | 每 worker 可派生 2 个子进程（闸门 4 ÷ 2），2 核下 4 worker 会自相争抢 |
> | 全局并发闸门 | 8 | **4** | 与 worker 数匹配（闸门 2×worker），且给 HTTP 线程池留出余量 |
> | 内存档位 | 单档 4G | **双档** | 2G 只保证**功能**正确；**性能类验收（NF-1~NF-4、AC-38）必须在 4G 机器上做**，本机跑出来的延迟数字不可信 |
>
> **连带重算的只有一处**：§5.4.2 的兜底墙钟 `×10 + 5000`（默认 35s）。闸门 8→4 使墙钟放大倍数从 8 降到 4（3s × 4 = 12s），**余量从 1.46 倍升到 2.9 倍 —— 变安全了，不需要下调系数**。详见 §5.4.2 的关系式。

---

## 2. 架构

### 2.1 系统总览

**两条部署路线并存** `[D-71]`。差别只在"**谁负责拉起 mysqld 与 g++**" —— 判题流水线、API 契约、数据模型**三者完全相同**，因此 C++ 代码里不需要任何 `#ifdef DEPLOY_MODE` 分支，只有启动脚本不同（§9 T0.1）。

| | 路线 A（宿主机原生） | 路线 B（Docker Compose） |
|---|---|---|
| 地位 | **当前开发/交付形态** | **目标形态**（可复现部署） |
| httplib + judge | 宿主机进程 | `oj` 容器 |
| MySQL | 宿主机 `mysqld` | `mysql` 容器 |
| g++ | 宿主机 `g++-12` | 容器内 `g++-12` |
| 建表方式 | 应用启动时幂等 `CREATE TABLE IF NOT EXISTS` | initdb 脚本 + 同样的兜底 |
| 日志 | `./logs/` | volume → `/var/log/oj` |
| 适用 | 开发迭代、单人长期使用 | 交付给他人、避免污染宿主环境 |

#### 2.1-A 路线 A：宿主机原生（当前）

```
┌────────── Ubuntu 24.04 宿主机（实测 2 核 / 1967MB） ──────────┐
│                                                                │
│  ┌───────── ./oj 进程 ───────────────────────────────────┐    │
│  │                                                         │    │
│  │  ┌──────────────┐        ┌─────────────────────────┐   │    │
│  │  │ HTTP 线程池   │        │  Judge 调度器 (1 线程)   │   │    │
│  │  │ cpp-httplib  │        │   内存 FIFO 队列         │   │    │
│  │  │  (N=8)       │───────▶│   启动恢复 pending       │   │    │
│  │  │  - 静态文件   │  入队   │   全局闸门(4) / 用户(2) │   │    │
│  │  │  - REST API  │        └──────────┬──────────────┘   │    │
│  │  │  - Session   │                   │ 分发             │   │    │
│  │  └──────┬───────┘                   ▼                 │   │    │
│  │         │            ┌──────────────────────────┐     │   │    │
│  │    ┌────▼─────┐      │  Judge Worker × 2 (K=2)  │     │   │    │
│  │    │   DAO    │      │  1. 取题 + 读用例(缓存)  │     │   │    │
│  │    │ 互斥量    │      │  2. 生成 3 个 harness 文件│     │   │    │
│  │    └────┬─────┘      │  3. g++-12 -c user.cpp    │     │   │    │
│  │         │            │     g++-12 user.o tester  │     │   │    │
│  │         │            │  4. 解析编译器输出 → CE? │     │   │    │
│  │         │            │  5. 逐用例 fork+setrlimit │     │   │    │
│  │         │            │  6. 单事务写回 DB         │     │   │    │
│  │         │            └──────────┬───────────────┘     │   │    │
│  │         │                       │                     │   │    │
│  │  ┌──────▼────────┐   ┌──────────▼─────────────────┐  │   │    │
│  │  │ 题库加载器     │   │ 工作区 /tmp/oj-judge/<id>/ │  │   │    │
│  │  │ problems/*.json│   │  user.cpp / user.o        │  │   │    │
│  │  │ 同步入库+缓存  │   │  *.h / tester.cpp / run   │  │   │    │
│  │  └──────┬────────┘   └────────────────────────────┘  │   │    │
│  └─────────┼────────────────────────────────────────────┘    │
│            │ TCP 127.0.0.1:3306                                    │
│  ┌─────────▼────────────────────────────────────────────┐        │
│  │  mysqld (宿主机) 8.0.46                                 │        │
│  │  /var/lib/mysql/<datadir>      ← 不用 initdb 脚本      │        │
│  │  建表由应用启动时幂等执行（§8 R-8 的兜底在此成为主方案） │        │
│  └────────────────────────────────────────────────────────┘        │
│  ./logs/app.log   ./logs/judge.log    （按天切割，留 7 天）         │
│  ./static/vendor/  ← 前端三库 vendored（D-73，无运行时外网依赖）  │
└────────────────────────────────────────────────────────────────────┘
```

> **路线 A 独有的三个细节**：
>
> 1. **建表不用 `/docker-entrypoint-initdb.d`**（那是容器机制）。改为应用启动时**幂等执行** `CREATE TABLE IF NOT EXISTS` —— §8 R-8 原本只是"兜底"，在此成为**主方案**。
> 2. **不用 volume**。`logs/` 与 `static/` 直接是仓库内目录，`.gitignore` 忽略其内容而非目录本身。
> 3. **MySQL 内存必须调低**。默认 `innodb_buffer_pool_size = 128MB` 在 1967MB 的机器上占比过大。启动脚本里设为 **`64MB`**（§9 T0.1），否则 2 worker × 2 子进程 × 128MB `RLIMIT_AS` 逼近上限时，会把 mysqld 变成 OOM killer 的首选目标。

#### 2.1-B 路线 B：Docker Compose（目标形态）

```
┌──────────────────────────────────────────────────────────────────────┐
│  Docker Compose (单机)                                                 │
│                                                                        │
│  ┌──────────────────────── oj 容器 ────────────────────────────────┐  │
│  │  ┌──────────────┐        ┌───────────────────────────────────┐  │  │
│  │  │ HTTP 线程池   │        │  Judge 调度器 (1 个调度线程)       │  │  │
│  │  │ cpp-httplib  │        │   - 内存 FIFO 队列                  │  │  │
│  │  │  (N=8)       │───────▶│   - 启动时恢复 pending 任务         │  │  │
│  │  │  - 静态文件   │ 入队   │   - 全局并发闸门(4) / 用户闸门(2)  │  │  │
│  │  │  - REST API  │        └────────────┬──────────────────────┘  │  │
│  │  │  - Session   │                     ▼                          │  │
│  │  └──────┬───────┘         ┌──────────────────────────────────┐  │  │
│  │         │                 │  Judge Worker × 2 (K=2)          │  │  │
│  │    ┌────▼─────┐           │   1. 取题 + 读用例（内存缓存）     │  │  │
│  │    │   DAO    │           │   2. 生成 solution_decl.h        │  │  │
│  │    │ 互斥量    │           │      + tester_util.h + tester.cpp│  │  │
│  │    └────┬─────┘           │   3. g++-12 -c user.cpp -o user.o │  │  │
│  │         │                 │      g++-12 user.o tester.cpp     │  │  │
│  │         │                 │   4. 解析编译器输出 → CE?        │  │  │
│  │         │                 │   5. 逐用例 fork+setrlimit 执行    │  │  │
│  │         │                 │   6. 写回 DB(逐用例行) + 汇总列     │  │  │
│  │  ┌──────▼────────┐        └────────────┬───────────────────┐  │  │
│  │  │ 题库加载器     │        ┌────────────▼─────────────────┐  │  │
│  │  │ problems/*.json│        │ 工作区 /tmp/oj-judge/<id>/    │  │  │
│  │  │ 启动时 upsert  │        │  user.cpp / user.o            │  │  │
│  │  │ + 灌内存缓存   │        │  *.h / tester.cpp / run       │  │  │
│  │  └──────┬────────┘        └──────────────────────────────┘  │  │
│  └─────────┼────────────────────────────────────────────────────   │
│            │  mysql://oj:oj@mysql:3306/oj                             │
│  ┌─────────▼────────────────────────────────────────────────────┐    │
│  │  mysql 容器  (8.0)   volume: oj-mysql-data                  │    │
│  │  /docker-entrypoint-initdb.d/01-schema.sql   首次自动建表   │    │
│  └────────────────────────────────────────────────────────────┘    │
│  volume: oj-logs → /var/log/oj/{app,judge}.log                      │
└──────────────────────────────────────────────────────────────────────┘
```

> **两张图的差异全部在"外壳"**：应用内部（HTTP 池 / DAO / 调度器 / Worker / 题库加载器 / 工作区）在 A/B **逐项一致**。因此 §2.3 的线程模型、§5 的判题流水线、§6 的 API 契约**对两条路线同等适用，文档不分叉**。
>
> **路线 B 的额外前提**（当前机器不满足）：需把当前用户加入 `docker` 组（`sudo usermod -aG docker $USER`）**并注销重新登录**，否则 `docker` 会持续报 `permission denied on /var/run/docker.sock`。

### 2.2 关键架构决策与理由

| 决策 | 选择 | 理由 | 代价 / 取舍 |
|---|---|---|---|
| 判题隔离 | fork + setrlimit | 零额外依赖，秒级启动，适合单机可信用户 | **无法防逃逸**。恶意用户可读取同机文件。已在 §8 声明为已知风险 |
| 队列载体 | 进程内线程池 + 内存队列 | 单机无需引入 Redis/MQ，部署最简 | 重启丢失任务 → 用 D-20 兜底：judging 置失败、pending 重放入队 |
| 用户代码编译 | 两步：`-c user.cpp` → 链接 `user.o + tester.cpp` | 用户代码与题库 harness **物理隔离**，题库代码永不改动 | 签名不匹配在链接期才暴露（错误信息不如编译期友好）。缓解：题面明示签名，生成 `solution_decl.h` 供用户参考 |
| 用例存储 | JSON 文件为源，启动同步入库（`problem_cases` / `problem_params`）+ 内存缓存 | 题面/用例/模板改一次文件即可；入库后逐用例可被 SQL 聚合（定位"第几个用例最常错"），不必全表捞 JSON 回应用层解析 | 运行时改题需重启。首版接受 |
| 生成器复杂度控制 | 结构化字面量为主 + `__expr` 逃生舱 | 覆盖 80% 题目零成本，复杂构造（树/图/链表）用逃生舱 | 逃生舱写法无编译期校验，靠录入时自测 |
| 内存度量 | harness 内 `getrusage(RUSAGE_SELF)` | 单次调用即可拿到，无需平台侧 wait4 复杂解析 | 报的是**进程峰值**含 harness 自身开销。已在 §5.5 显式声明为已知精度限制 |

### 2.3 进程与线程模型

```
主线程        : 加载题库 → 启动 judge 调度器 → httplib listen() 阻塞
HTTP 线程 × 8 : 处理请求；DB 访问经全局 std::mutex 串行化
调度线程 × 1  : 从 DB 恢复 pending → 入队 → 循环 pop 任务 → 投递到空闲 worker
Judge Worker × 2: 阻塞在条件变量上，被唤醒后独占执行一个提交的全流程  [D-74]
```

**锁顺序**：`DB mutex` 是唯一全局锁，仅在 DAO 调用期间持有，**绝不在持有 DB 锁时等待 judge**。判题流程不碰 DB 锁（除最终写回），避免长事务阻塞 HTTP 请求。

**worker 数与并发闸门的关系** `[D-74]`：

```
闸门 = 2 × worker 数
4 = 2 × 2  ✓

闸门 = 2 × worker 的意义：保证「每个 worker 同时最多派生 2 个子进程」，
且任一 worker 不会一次占用全部闸门额度而让其他 worker 饿死。
若闸门 == worker 数，则并发度退化为 1（一次只判一题），吞吐减半。
```

**为什么不加大 worker 数**：三条约束同时生效，取最紧的那条：

| 约束 | 上限 | 依据 |
|---|---|---|
| CPU 核数 | 2 核 / 4 worker = 0.5 核/题 | 低于 1 核时 wall-clock 放大倍数上升，逼近 §5.4.2 的兜底墙钟 |
| 内存 | 1967MB 总 / (2 worker × 2 子进程 × 128MB `RLIMIT_AS`) | 加上 mysqld（§2.1-A 已把 buffer pool 降到 64MB）与 8 个 HTTP 线程后余量有限 |
| 闸门与 worker 的比例 | 闸门 = 2 × worker | 上面那条关系式 |

> v1.0 曾取 worker 4 × 闸门 8。**下调到 2 × 4 后，§5.4.2 的墙钟放大倍数从 8 倍降到 4 倍**（3s CPU → 12s 墙钟），兜底墙钟的余量**从 1.46 倍升到 2.9 倍 —— 变安全了**。也就是说这次下调没有连带削弱任何安全性，只是把吞吐换成了与机器匹配的量级。

---

## 3. 数据模型

### 3.1 ER 关系

```
users 1───n sessions
users 1───n submissions n───1 problems
users n───n problems   (通过 user_problems 表达收藏)
problems n───n tags    (通过 problem_tags)

-- 题库同步链：JSON 里的数组字段一律拆表，避免"单元格存多值"（见 §3.4）
problems 1───n problem_cases         -- 题目侧用例：args / expected / brief
problems 1───n problem_params        -- 题目侧形参：name / cpp_type（生成器用）

-- 判题结果链：与 problem_cases 解耦，只靠 case_idx 快照对齐
submissions 1───n submission_case_results    -- 逐用例结论 + 输入输出快照
submissions 1───n submission_compile_errors  -- 逐条编译诊断
```

关键取舍：`submission_case_results` **不**外键引用 `problem_cases`。题库是活的（D-30 改 JSON 即改题），而提交历史要求可回放（D-31）；若用外键，改题删用例会让历史详情凭空少行。因此结果表内 `case_idx` 与输入/输出/期望值文本一律**快照**，代价是有意承担的数据重复，换取历史完整性。

### 3.2 DDL

```sql
CREATE TABLE users (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  username      VARCHAR(32)  NOT NULL UNIQUE,
  password_hash VARCHAR(255) NOT NULL,           -- bcrypt, cost=10
  role          ENUM('user','admin') NOT NULL DEFAULT 'user',  -- D-37 真源为 OJ_ADMIN_USERNAMES，启动时同步，见 §6.3.1
  created_at    DATETIME     NOT NULL,
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sessions (
  token      CHAR(64)     NOT NULL PRIMARY KEY,  -- 32 字节随机数的 hex
  user_id    BIGINT UNSIGNED NOT NULL,
  expires_at DATETIME     NOT NULL,
  created_at DATETIME     NOT NULL,
  KEY idx_user (user_id),
  KEY idx_expire (expires_at),
  CONSTRAINT fk_session_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE tags (
  id       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name     VARCHAR(32) NOT NULL,
  category ENUM('algorithm','source') NOT NULL,  -- 算法主题 / 来源与面试频率
  UNIQUE KEY uk_name_cat (name, category)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE problems (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  slug          VARCHAR(64)  NOT NULL UNIQUE,    -- URL 友好标识，如 two-sum
  title         VARCHAR(128) NOT NULL,
  difficulty    ENUM('easy','medium','hard') NOT NULL,
  statement_md  MEDIUMTEXT   NOT NULL,           -- Markdown 原文，前端渲染
  hint_md       MEDIUMTEXT   NULL,               -- 方向提示 Markdown，免费无限看（D-47）。NULL/空串则前端不渲染提示 tab
  signature     TEXT         NOT NULL,           -- Solution 类声明（见 §4）
  method_name   VARCHAR(64)  NOT NULL,           -- harness 调用的方法名
  return_type   VARCHAR(128) NOT NULL,           -- Solution 方法返回类型
  harness_prelude TEXT       NULL,               -- __expr 逃生舱所需的前置声明/头文件（见 §4.2）
  code_template MEDIUMTEXT   NOT NULL,           -- 编辑器初始内容
  time_limit_ms INT UNSIGNED NOT NULL DEFAULT 3000,
  mem_limit_kb  INT UNSIGNED NOT NULL DEFAULT 131072,
  case_count    INT UNSIGNED NOT NULL DEFAULT 0, -- 受控反范式：= COUNT(problem_cases)，列表页免 JOIN，见 §3.4
  is_featured   TINYINT(1)   NOT NULL DEFAULT 0, -- 精选置顶
  sort_no       INT          NOT NULL DEFAULT 0,
  created_at    DATETIME     NOT NULL,
  updated_at    DATETIME     NOT NULL,
  KEY idx_diff (difficulty), KEY idx_featured (is_featured, sort_no)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 题目侧用例。题库 JSON 的 cases 数组按题内出现顺序展开为多行，
-- 不以 JSON 文本落库（1NF，见 §3.4）。单条用例内部的 args 仍可为数组 ——
-- 那是该用例的固有复合参数，不是重复组。
CREATE TABLE problem_cases (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  problem_id    INT UNSIGNED NOT NULL,
  case_idx      INT UNSIGNED NOT NULL,           -- 题内 0-based 序号，与 @@OJ@@ 的 idx 对齐
  brief         VARCHAR(255) NULL,               -- 如"基础用例"，展示在结果表
  args_json     MEDIUMTEXT   NOT NULL,           -- [[2,7,11,15], 9] 或 [{"__expr":"buildTree(...)"}]
  expected_json MEDIUMTEXT   NOT NULL,
  UNIQUE KEY uk_problem_case (problem_id, case_idx),
  CONSTRAINT fk_pc_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 题目侧形参。题库 JSON 的 params 数组同样展开。
-- 元素类型推导规则（§4.2）依赖此表的 cpp_type 列，故必须结构化才能被查询/校验。
CREATE TABLE problem_params (
  id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  problem_id INT UNSIGNED NOT NULL,
  param_idx  INT UNSIGNED NOT NULL,              -- 形参序号，决定调用顺序
  name       VARCHAR(64)  NOT NULL,              -- 如 nums
  cpp_type   VARCHAR(128) NOT NULL,              -- 如 std::vector<int>&
  UNIQUE KEY uk_problem_param (problem_id, param_idx),
  CONSTRAINT fk_pp_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE problem_tags (
  problem_id INT UNSIGNED NOT NULL,
  tag_id     INT UNSIGNED NOT NULL,
  PRIMARY KEY (problem_id, tag_id),
  KEY idx_tag (tag_id),
  CONSTRAINT fk_pt_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,
  CONSTRAINT fk_pt_tag     FOREIGN KEY (tag_id)     REFERENCES tags(id)     ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 收藏关系。progress 字段由提交历史异步推导（见 §6.4）
CREATE TABLE user_problems (
  user_id    BIGINT UNSIGNED NOT NULL,
  problem_id INT UNSIGNED    NOT NULL,
  favorite   TINYINT(1) NOT NULL DEFAULT 0,
  updated_at DATETIME    NOT NULL,
  PRIMARY KEY (user_id, problem_id),
  KEY idx_fav (user_id, favorite),
  CONSTRAINT fk_up_user    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
  CONSTRAINT fk_up_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE submissions (
  id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id         BIGINT UNSIGNED NOT NULL,
  problem_id      INT UNSIGNED    NOT NULL,
  language        VARCHAR(16) NOT NULL DEFAULT 'cpp',
  code            MEDIUMTEXT   NOT NULL,          -- 代码全文，可回看
  status          ENUM('pending','judging','done','failed') NOT NULL DEFAULT 'pending',
  verdict         ENUM('AC','WA','TLE','MLE','RE','CE','JE') NULL, -- 仅 status=done 时非空
  message         VARCHAR(255) NULL,               -- 顶层说明，如"第3个用例超时"
  compiler_output MEDIUMTEXT   NULL,               -- 原始编译器输出全文（原子文本，不拆分）
  case_passed     INT UNSIGNED NULL,               -- 受控反范式：= COUNT(passed=1) 于 submission_case_results。CE 时为 0（D-65）
  case_total      INT UNSIGNED NULL,               -- 受控反范式：= GREATEST(COUNT(明细行), problems.case_count)，见 §5.6.3（D-65 修正，原为单纯 COUNT）
  total_ms        INT UNSIGNED NULL,               -- 受控反范式：= COALESCE(SUM(time_ms),0)，TLE/MLE 用例度量缺失故不计
  max_mem_kb      INT UNSIGNED NULL,               -- 受控反范式：= COALESCE(MAX(mem_kb),0)
  created_at      DATETIME     NOT NULL,           -- UTC
  finished_at     DATETIME     NULL,
  KEY idx_user_time (user_id, id DESC),
  KEY idx_problem_time (problem_id, id DESC),
  KEY idx_status (status),
  KEY idx_user_verdict (user_id, verdict),
  CONSTRAINT fk_sub_user    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
  CONSTRAINT fk_sub_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 提交侧逐用例结果。原先这些信息以 JSON 数组存在 submissions.case_results 一列内，
-- 属"单元格存多值"（1NF 违反），已拆表。前端拿到的 caseResults 由本表按 case_idx
-- 升序组装（见 §6.3）。文本列为快照，理由见 §3.1。
CREATE TABLE submission_case_results (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  submission_id BIGINT UNSIGNED NOT NULL,
  case_idx      INT UNSIGNED    NOT NULL,        -- 快照自 problem_cases.case_idx
  verdict       ENUM('AC','WA','TLE','MLE','RE') NOT NULL,  -- 单用例结论
  passed        TINYINT(1)      NOT NULL,
  args_text     MEDIUMTEXT      NULL,            -- 展示用序列化文本（§5.3 规则）
  output_text   MEDIUMTEXT      NULL,            -- 用户 stdout
  expected_text MEDIUMTEXT      NULL,
  time_ms       INT UNSIGNED    NULL,            -- TLE/MLE 时为 NULL：harness 被 SIGKILL，未产出度量
  mem_kb        INT UNSIGNED    NULL,            -- 同上
  error_msg     VARCHAR(255)    NULL,            -- 如"超出时间限制 (3000ms)"、RE 的 stderr 摘要
  UNIQUE KEY uk_sub_case (submission_id, case_idx),
  KEY idx_case_verdict (case_idx, verdict),      -- 统计"第几个用例最常错"
  CONSTRAINT fk_scr_sub FOREIGN KEY (submission_id) REFERENCES submissions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 提交侧编译诊断。原先为 submissions.compile_errors 的 JSON 数组，同属 1NF 违反。
-- file_name 已做路径归一化（§5.6，只保留 user.cpp，不暴露服务器目录）。
-- 未被正则命中的行不进本表，原样留在 submissions.compiler_output。
CREATE TABLE submission_compile_errors (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  submission_id BIGINT UNSIGNED NOT NULL,
  seq           INT UNSIGNED    NOT NULL,        -- 出现顺序，保证前端展示与编译器输出一致
  file_name     VARCHAR(64)     NOT NULL DEFAULT 'user.cpp',
  line          INT UNSIGNED    NOT NULL,        -- 1-based，行高亮用
  col           INT UNSIGNED    NOT NULL DEFAULT 0,
  severity      ENUM('error','warning','note') NOT NULL DEFAULT 'error',
  message       VARCHAR(512)    NOT NULL,
  UNIQUE KEY uk_sce_seq (submission_id, seq),
  KEY idx_sce_line (submission_id, line),
  CONSTRAINT fk_sce_sub FOREIGN KEY (submission_id) REFERENCES submissions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### 3.3 索引与查询说明

| 场景 | 走索引 |
|---|---|
| 列表页按难度/精选排序 | `idx_diff` / `idx_featured` |
| 用户历史页倒序翻页 | `idx_user_time` |
| 历史页按 verdict 筛选 | `idx_user_verdict` |
| 题目页最近 5 次提交 | `idx_problem_time` |
| 启动恢复 pending | `idx_status` |
| 收藏筛选 | `user_problems` 主键 + `idx_fav` |
| 生成 tester.cpp 时取全部用例 | `problem_cases.uk_problem_case`（按 case_idx 有序） |
| 提交详情返回逐用例表格 | `submission_case_results.uk_sub_case` |
| 提交详情返回编译诊断 | `submission_compile_errors.uk_sce_seq` |
| 复盘统计"某题第 N 个用例最常错" | `submission_case_results.idx_case_verdict` |

`submission_case_results` 有两条访问路径，故两条索引都留：按提交取（详情页，高频）与按用例取（复盘统计，低频但需要）。50 题 × 12 用例量级下，全表 `idx_case_verdict` 也可接受，二者不冲突。

**复盘统计示例**（"某题第几个用例最常错"——原先只能捞 JSON 回应用层 parse，现在一条 SQL）：

```sql
SELECT r.case_idx,
       COUNT(*)                       AS attempts,
       SUM(r.verdict <> 'AC')          AS failed,
       ROUND(AVG(r.time_ms))          AS avg_ms
FROM submission_case_results r
JOIN submissions s ON s.id = r.submission_id
WHERE s.problem_id = ? AND s.user_id = ?
GROUP BY r.case_idx
ORDER BY r.case_idx;
```

> 两张表都有 `verdict` 列，**必须带表别名前缀**，否则报 `Column 'verdict' in field list is ambiguous`。`AVG` 同样自动忽略 TLE/MLE 的 NULL 行，与 §3.4 的聚合约定一致。

### 3.4 范式取舍与受控反范式

**遵循到 1NF，不再向上。** 本项目是单机自用判题系统，读多写少、无并发报表需求，2NF/3NF 带来的表数量膨胀换不到可观测收益。因此设计准则是：**消灭"单元格存多值"（1NF 硬要求），但不追求纯粹的关系范式（保留必要的派生汇总列）。**

**已消除的多值列**（原先写法 → 现写法）：

| 原写法 | 1NF 问题 | 现写法 |
|---|---|---|
| `submissions.case_results` 存逐用例 JSON 数组 | 一格 N 个用例 × 10 个字段，不可索引/约束/聚合 | `submission_case_results` 每用例一行 |
| `submissions.compile_errors` 存诊断 JSON 数组 | 同上 | `submission_compile_errors` 每条一行 |
| 用例仅存于 `problems/<slug>.json`，DB 无对应表 | DB 中"用例数"无法由数据推出，`case_count` 成悬空值 | `problem_cases` + `problem_params` |
| `params` 数组只在文件里 | 类型推导依赖 `cpp_type`，不可查不可校验 | `problem_params` |

**保留的受控反范式**（刻意重复，必须有明确的维护契约）：

| 列 | 派生自 | 为何保留 | 维护契约 |
|---|---|---|---|
| `problems.case_count` | `COUNT(problem_cases)` | 列表页免 JOIN | 只由题库加载器在 upsert 用例行后同事务刷新；不从其他路径写 |
| `submissions.case_passed` | `COUNT(passed=1)` | 列表/历史页直接显示"通过 2/3" | 与用例行**同一事务**写入；写回后不单独 UPDATE |
| `submissions.case_total` | `GREATEST(COUNT(明细行), problems.case_count)` `[D-65]` | 同上；**CE 时也能显示"12 个用例未执行"** | 同上；读该列时须 JOIN `problems` |
| `submissions.total_ms` | `COALESCE(SUM(time_ms),0)` | 避免列表页 N+1 聚合 | 同上 |
| `submissions.max_mem_kb` | `COALESCE(MAX(mem_kb),0)` | 同上 | 同上 |
| `submission_case_results` 内的文本快照 | `problem_cases.args_json` | **不可改为外键引用** —— 题库可改，历史必须可回放（D-31） | 只写不改，随 submission 生命周期级联删除 |

> **聚合必须用 `COALESCE(... , 0)`，不能直接用 `SUM`/`MAX`**。TLE/MLE 用例的子进程被 `SIGKILL`，harness 来不及输出该用例的 `@@OJ@@` 行，因此它的 `time_ms` / `mem_kb` 是 NULL。`SUM` 遇 NULL 会跳过，`MAX` 也会跳过 —— 这正是期望行为（被杀的用例不该贡献耗时）。但若一次提交**所有**用例都被杀，裸 `SUM` 返回 NULL 而非 0，与列的 `NOT NULL` 语义冲突。故约定统一以 `COALESCE(...,0)` 收口，写回方与校验方用同一表达式。

**落库后的一致性校验**：`tests/run_tests.sh` 增加一条断言 —— 对每条 `status='done'` 的提交，

```sql
-- v1.5 修订版 [D-65] [D-67]：
--   ① 补 JOIN problems（case_total 契约已依赖 problems.case_count）
--   ② 覆盖范围从 done 扩到 done + failed（§5.7 不变量已扩展）
--   ③ case_total 改为 GREATEST 口径
SELECT COUNT(*) AS violations FROM (
  SELECT s.id
  FROM submissions s
  JOIN problems p ON p.id = s.problem_id            -- ← v1.5 新增
  LEFT JOIN submission_case_results r ON r.submission_id = s.id
  WHERE s.status IN ('done','failed')               -- ← v1.5 扩展：覆盖 failed
  GROUP BY s.id, s.case_passed, s.case_total, s.total_ms, s.max_mem_kb
  HAVING s.case_passed <> COALESCE(SUM(CASE WHEN r.passed = 1 THEN 1 ELSE 0 END), 0)
      OR s.case_total  <> GREATEST(COUNT(r.id), p.case_count)     -- ← v1.5 修订
      OR s.total_ms    <> COALESCE(SUM(r.time_ms), 0)
      OR s.max_mem_kb  <> COALESCE(MAX(r.mem_kb), 0)
) t;   -- 必须返回 0
```

> **三处修订各自的必要性**：
>
> | 修订 | 不改会怎样 |
> |---|---|
> | ① `JOIN problems` | `GREATEST` 引用 `p.case_count`，不 JOIN 则语法错误；用 `LEFT JOIN` 也不行 —— 题目被删时会退化成 NULL 比较 |
> | ② 覆盖 `failed` | §5.7 的不变量扩展到 `failed`（见下），但断言不覆盖 = **不变量没有校验就没有约束**。`failed` 若带残留明细行，这一条能抓住 |
> | ③ `GREATEST` 口径 | 写回方改了口径而断言还在比 `COUNT`，会**全线报错**。断言与写回必须用同一表达式（与 `COALESCE` 约定同理） |
>
> **`failed` 的明细行必须为 0** `[D-67]`：v1.4 只证明了「`status ∈ {pending, judging}` 恒有 0 条明细行」，**漏掉了 `failed`**。理由是 T2.11 的写回是单事务 —— 失败时**整体回滚**，所以 `failed` 记录**同样恒有 0 条明细行**。把不变量扩展为：
>
> > **`status ∈ {pending, judging, failed}` 恒有 0 条 `submission_case_results` 行。**
>
> 这条推论对重跑（D-58）也成立 —— 重跑是 `INSERT` 新记录，原 `failed` 记录不被触碰，故不会撞 `uk_sub_case`。
>
> ⚠️ **若哪天把写回改成分批提交，必须同时在本节补上明细行清理逻辑**，并重新审视这条不变量。

上表任何一条派生规则被改坏、或写回漏了同事务，都会被这条断言抓住。

**仍保留为原子文本、不拆表**：

- `submissions.compiler_output` — 编译器输出的原始全文，是一份不可分的产物，非结构化多值，拆行无收益（`submission_compile_errors` 只是它的结构化旁路）。
- `problem_cases.args_json` / `expected_json` — 单条用例的**固有复合参数**（一次调用传多个实参），不是"重复组"。这一列的原子性边界止于"一条用例"。

**`users.role` 用 ENUM 单列，不拆 `roles` + `user_roles` 关联表** `[D-37]`：这看着像少了 2NF/3NF，实际是本节立场的直接应用 —— 角色只有 2 个取值且几乎不变，拆表会在**每个鉴权请求**上多一次 JOIN，而可观测收益为零（没有需要按角色组合查询的报表）。ENUM 单列是原子的，满足 1NF；将来真出现第三种角色，一条 `ALTER TABLE ... MODIFY role ENUM('user','admin','moderator')` 即可，代价可控。**判据：只有当"按角色维度查询"成为真实需求时才拆表。**

### 3.5 统计类查询不落派生列的理由 `[D-38] [D-39]`

`GET /api/me/overview`（§6.3）要返回 `totals` / `byDifficulty` / `byTag` 三块聚合。看起来很该加派生列（如 `users.solved_count`、`tags.solved_count`）来避免每次现算，但本项目**刻意不加**：

| 判据 | 本项目的取值 | 结论 |
|---|---|---|
| 查询频率 | 首页概览条 + 我的 tab2，**仅登录用户触发**，个人单机量级 | 不构成高频读 |
| 数据规模 | 50 题 × 单人提交数千行 | 全表 `idx_user_verdict` 聚合在毫秒级 |
| 失效路径 | 派生列需在**每次提交写回**时同步维护，`verdict` 改写、`题库同步删题、用户注销**三处都要记得刷新** | 维护契约成本 > 查询收益 |
| 可观测收益 | 派生列与实时聚合**可能不一致**，且不一致时无法判断谁是错的 | 反向收益 |

这与 §3.4 的立场一致：受控反范式**只用在"高频读 + 写路径单一"的列**上（`case_count` / `case_passed` 等），而**不是"凡是能聚合就都物化"**。

> 因此 v1.3 **不新增任何表或列**，`§3.2` 的 DDL 逐字不变。若日后题库或用户量级增长到聚合成为瓶颈，再物化 `byTag` 口径 —— 但那是**性能问题**，不是设计问题，不应提前预支。

### 3.6 v1.4 的 DDL 变更：仅一列 `[D-42] [D-47] [D-49] [D-58]`

D-42 ~ D-61 共 20 项决策，落到数据层的改动是：

| 决策 | DDL 影响 | 说明 |
|---|---|---|
| D-47 题目提示 | **新增 `problems.hint_md`** | 见 §3.2，唯一的新列 |
| D-49 删除自己的提交 | **无** | `DELETE FROM submissions WHERE id=?`，明细行由 `fk_scr_sub` / `fk_sce_sub` 的 CASCADE 连带删 |
| D-56/D-57 注销账号 | **无** | `DELETE FROM users WHERE id=?`，`sessions` / `submissions` / `user_problems` 三条外键均为 CASCADE，二级 CASCADE 覆盖 `submission_case_results` / `submission_compile_errors` |
| D-58 重跑 = 新增记录 | **无** | 与普通提交走同一条 INSERT |
| D-60 队列位置 | **无** | 派生自 `submissions.status='pending'` 的计数，不落库（与 §3.5 同一立场） |
| D-42 diff / D-45 推荐 | **无** | 纯查询 + 前端计算 |
| 其余 15 项（前端行为/交互/文案） | **无** | — |

**仍然 11 张表。** 这不是巧合：D-42 ~ D-61 全部落在"已有数据的读取方式"或"已有数据的操作入口"上，没有一项需要新的持久化形态。

> **CASCADE 链的完整性是注销方案（D-57）能成立的前提**，必须验证：`users` → `submissions` → `submission_case_results` 是一条**两级 CASCADE 链**，InnoDB 支持且会在同一次语句中完成。若哪张子表被改成 `ON DELETE RESTRICT`，注销会直接失败 —— AC-33 专门验证这一点。


---

## 4. 题库 JSON Schema

题库文件位于仓库 `problems/<slug>.json`，是**唯一真源**。服务启动时读取，upsert 入库（§3.2 的 `problems` / `problem_cases` / `problem_params`），并灌入进程内只读缓存供 judge worker 使用。运行期不再读文件。

### 4.1 完整示例（数组题）

```json
{
  "slug": "two-sum",
  "title": "两数之和",
  "difficulty": "medium",
  "is_featured": true,
  "sort_no": 10,
  "tags": {
    "algorithm": ["数组", "哈希表"],
    "source": ["高频面试题"]
  },
  "statement_md": "## 题目描述\n\n给定一个整数数组 `nums` ...\n\n### 示例 1\n\n```\n输入: nums = [2,7,11,15], target = 9\n输出: [0,1]\n```\n\n### 约束\n\n- `2 <= nums.length <= 10^4`\n- 仅一个答案",
  "hint_md": "先想想要不要重复遍历数组。答案的下标之和是 `target`，试试边遍历边记录**已经见过的值 → 下标**。",
  "signature": "class Solution {\npublic:\n    std::vector<int> twoSum(std::vector<int>& nums, int target);\n};",
  "method_name": "twoSum",
  "params": [
    { "name": "nums",   "cpp_type": "std::vector<int>&" },
    { "name": "target", "cpp_type": "int" }
  ],
  "return_type": "std::vector<int>",
  "code_template": "#include <vector>\nusing namespace std;\n\nclass Solution {\npublic:\n    vector<int> twoSum(vector<int>& nums, int target) {\n        \n    }\n};",
  "time_limit_ms": 3000,
  "mem_limit_kb": 131072,
  "harness_prelude": "",
  "cases": [
    {
      "args": [[2, 7, 11, 15], 9],
      "expected": [0, 1],
      "brief": "基础用例"
    },
    {
      "args": [[3, 2, 4], 6],
      "expected": [1, 2],
      "brief": "答案不在开头"
    }
  ]
}
```

**JSON → 库的字段映射**（加载器按此 upsert，数组字段展开为多行，见 §3.4）：

| JSON 路径 | 目标列 | 展开 |
|---|---|---|
| `slug` `title` `difficulty` `statement_md` `hint_md` `signature` `method_name` `return_type` `code_template` `time_limit_ms` `mem_limit_kb` `harness_prelude` `is_featured` `sort_no` | `problems.*` 同名列 | 一题一行 |
| `tags.algorithm[]` / `tags.source[]` | `tags` + `problem_tags` | 按 `category` 分别落 tag 行 |
| `params[i].name` / `.cpp_type` | `problem_params` | 每形参一行，`param_idx = i` |
| `cases[i].args` / `.expected` / `.brief` | `problem_cases.args_json` / `.expected_json` / `.brief` | 每用例一行，`case_idx = i` |
| — | `problems.case_count` | 同步完成后回填为该题用例行数 |

> `harness_prelude` 是**可选**字段，缺省为空串。用到 `__expr` 逃生舱时填所需的前置声明/头文件，如 `"#include \"../common/treedef.h\"\nusing namespace oj;"`（见 §4.2）。
> `hint_md` 同为**可选** `[D-47]`，缺省为 `NULL`。为空时前端**不渲染提示 tab**（不是渲染一个空 tab）—— 50 道题里可能只有部分写了提示。`hint_md` 必须是**方向性**提示（"试试哈希表"），不是完整解法；不给答案是它存在的意义。
> ⚠️ 题面**禁图片** `[D-48]`：`statement_md` 与 `hint_md` 中**不得出现 `![](...)` 语法**。树/图/矩阵题一律用**字符画**（放在 `pre` 代码块里，保留等宽对齐）。理由：题库 JSON 是唯一真源且纯文本，图片会引入静态资源管理、容器内改宿主文件的部署耦合、无法做文本 diff 三个问题（与 §6.3.4 不做题库在线编辑同源）。前端 DOMPurify 白名单相应**移除 `img`**（见 §7.3）。
> 注意：上面的 `statement_md` 为节省篇幅做了缩写（值里有 `...`），因此**整块并非可直接粘贴的合法 JSON**。录入时把 `statement_md` / `hint_md` 换成完整的单行转义字符串（换写作 `\n`），其余字段可直接照抄。`cases` / `params` 的结构则是字面准确的。

**同步语义**（`problem/loader.h`，幂等，按 slug）：题库是唯一真源且只增不改，故采用 **delete-then-insert** 而非逐行 upsert —— 先 `DELETE FROM problem_cases WHERE problem_id=?` 与 `DELETE FROM problem_params WHERE problem_id=?`，再按数组顺序整体插入，最后刷新 `case_count`。整个过程单事务。这样"从 JSON 删掉一个用例"能被正确传播到 DB，不会留下幽灵用例行。

> 反向影响：删除用例行会级联影响 `submissions`？不会。`submission_case_results` 存的是快照且无外键指向 `problem_cases`（§3.1），历史提交详情不受题库改动影响。

### 4.2 字面量映射规则（生成器核心契约）

| JSON 值 | 生成的 C++ 字面量 | 备注 |
|---|---|---|
| `5` / `-3` | `5` / `-3` | 数字 |
| `true` / `false` | `true` / `false` | |
| `null` | `nullptr` | 用于指针类型 |
| `"abc"` | `"abc"` | **生成器负责转义**：`\` `"` 换行 制表符 非 ASCII→`\xNN` |
| `[1,2,3]` | `std::vector<int>{1, 2, 3}` | 元素类型由 `params` 中的 `cpp_type` **从外向内推导** |
| `[[1,2],[3,4]]` | `std::vector<std::vector<int>>{{1,2},{3,4}}` | 二维 |
| `{"__expr": "buildTree({1,null,2,3})"}` | 原样嵌入 | **逃生舱**，用于树/链表/图等需构造函数的自定义类型 |

**类型推导规则**：从 `params[i].cpp_type` 中剥离引用 `&`、取得基础类型名（如 `std::vector<std::vector<int>>` → `int`），据此把 JSON 嵌套数组转成对应层数的 C++ 初始化器。

**`__expr` 逃生舱约定**：使用前必须在 `harness_prelude` 字段中提供所需构造函数（如 `TreeNode* buildTree(std::vector<int>)`），生成器把它拼到 tester.cpp 顶部。

```json
"harness_prelude": "#include \"../common/treedef.h\"\nusing namespace oj;",
"cases": [
  { "args": [{ "__expr": "buildTree({1,null,2,3})" }], "expected": { "__expr": "serializeTree(buildTree({3,1,4}))" } }
]
```

**不支持的用例形态**（首版明确排除，遇到时启动同步应报错并跳过该题）：
交互式题（需多轮 IO）、需要 `cin >>` 读多行的程序设计题（USACO 风格）、特殊 Judge（多个答案都算对）。

### 4.2.1 生成期的类型一致性校验 `[D-64]`

**校验时机：生成 C++ 之前**（在 §5.6.1 的题库加载校验之外，这里是生成器自身的一道防线）。校验通过才生成文件，**失败则不产出任何文件**并让该次提交走 `JE`：

| 校验 | 规则 | 不通过时 |
|---|---|---|
| `args` 长度 == `params` 长度 | 一一对应 | 抛异常 → `JE`，`message` 指出第几个用例的参数个数不符 |
| `args[i]` 的容器深度 == `cpp_type` 的 `vector` 层数 | 见 §4.2 类型推导 | 抛异常 → `JE`，`message` 指出 `cpp_type` 与实际 JSON 形状不符 |
| `expected` 的容器深度 == `return_type` 的 `vector` 层数 | 同上 | 抛异常 → `JE`（**这是 `harness` 编译错误的最大来源**） |
| `__expr` 内已配对 `harness_prelude` | §4.2 约定 | 抛异常 → `JE` |

> **为什么生成期校验判 `JE` 而不是 `CE`**：题目是**题库配置问题**，不是用户提交的问题。判 `CE` 会让用户以为自己的代码写错了（CE 的 UI 是"编译错误 + 逐行定位"），而实际上他连一个字符都不用改。判 `JE` 并在 `message` 里说"本题配置有误"，是唯一不误导用户的表述。
>
> **两道防线的分工**：§5.6.1 是**题库加载期**（拦住明显错误，那题直接跳过，用户根本看不到它）；本节是**提交时**（拦住加载期漏掉的，如 `__expr` 语法错误）。两道都要有 —— 加载期能"让错误题不出现在列表里"，提交期能"让漏网的错误不伪装成用户错误"。

### 4.3 题目录入工作流 `[D-30]`

```
1. 新建 problems/<slug>.json，按 §4.1 填写
2. 本地起服务 → 启动时自动同步到 MySQL（problem_cases / problem_params 按 §4.1 映射表展开）
   ★ 启动时同步会先跑 §5.6.1 的类型一致性静态校验；不通过则跳过该题并在启动日志点名
3. 【v1.5 新增】校准 mem_limit_kb —— 默认 128MB 是 AS 口径（§5.5.1），
   树/图/大矩阵题必须逐题调大（如 262144），不要套用默认值
4. 打开该题页面提交一次，用官方/自写解法验证 12 个用例全绿
   ★ 若报 "harness" 编译错误 → 是题库错了，不是代码错了（§5.6）
5. 全绿则录入完成；否则按 judge 日志 /var/log/oj/judge.log 修正 JSON
6. 若改动了用例内容/数量，重启服务让 DB 与内存缓存刷新
   （提交历史不受影响：历史存的是快照，见 §3.4）
```

---

## 5. 判题流水线（系统最核心部分）

### 5.1 时序

```
前端          HTTP线程         调度器        Worker         fork子进程      MySQL
 │ POST /api/submissions                                             │
 ├───────────▶│ 校验登录/配额/代码长度                                │
 │            │ INSERT status=pending ───────────────────────────────▶│
 │            │ push(队列) ────────────▶│                            │
 │◀───201 {submissionId, status:"pending"}                          │
 │                                                        │          │
 │ GET /api/submissions/{id} (每1.5s)                      │          │
 ├───────────▶│ SELECT ◀─────────────────────────────────────────────┤
 │◀─200 {status:"judging"}                                      │    │
│                          │◀── pop ──────────────────│              │
│                          │                          │ 1.读题+用例  │
│                          │                          │  (内存缓存)  │
│                          │                          │ 2.生成 3 文件 │
│                          │                          │ 3.g++ 编译    │
│                          │                          │ 4.CE? → 写回  │
│                          │                          │ 5.逐用例 fork │
│                          │                          │   setrlimit  │
│                          │                          │   ┌────────▶│ 跑用例
│                          │                          │   │◀───────┘ stderr@@OJ@@
│                          │                          │ 6.单事务写回 ─────────────▶│
│                          │                          │  UPDATE submissions(汇总列)
│                          │                          │  + INSERT 用例行 N 条
│                          │                          │  + INSERT 诊断行 M 条
│                          │                          │                    │
 │ GET /api/submissions/{id}                     ◀──────┤                       │
 │◀─200 {status:"done", verdict:"WA", caseResults:[...]}│                       │
```

> 步骤 1 的用例取自题库加载器灌入的**进程内只读缓存**（内容与 `problem_cases` 一致），不查 DB、不读文件。原因是 worker 无需持 DB 锁（§2.3 的锁顺序约束），且题库运行期不变。
> 步骤 6 的三次写必须是**同一事务**：汇总列与明细行要么同时生效要么都不生效，否则详情页会出现"通过 2/3"但表里只有 2 行（见 §3.4 的维护契约）。

### 5.2 工作目录与文件

每次提交在 `/tmp/oj-judge/<submissionId>/` 下生成：

```
user.cpp              用户提交的代码（原样，不做任何改写）
solution_decl.h       【生成】Solution 类的声明（来自题库 signature）
tester_util.h         【生成】用例执行、结果比较、JSON 序列化、getrusage 工具
tester.cpp            【生成】harness：harness_prelude + main() + 逐用例调用
user.o                编译产物
run                   链接产物
```

**编译命令**（两步，见 §2.2）：

```bash
g++ -std=c++17 -O2 -Wall -c user.cpp -o user.o            # 阶段一：只编用户代码
g++ -std=c++17 -O2 user.o tester.cpp -o run               # 阶段二：链接 harness
```

两阶段的作用：用户代码的语法/类型错误在阶段一就暴露，此时 tester.cpp 尚未参与，报错信息干净，不会被 harness 的类型混在一起。签名不匹配在阶段二链接期暴露。

**编译超时**：10s（`alarm()` + SIGALRM handler → 记为 CE，message = "编译超时"）。

**产物清理**：无论成功失败，worker 结束前 `rm -rf` 工作目录。服务启动时先扫描 `/tmp/oj-judge/*` 清空残留（应对上次进程被 kill -9）。

### 5.3 harness 输出协议（stdout / stderr 分流）

利用两条流的天然分离，避免用户 `cout` 污染元信息：

| 流 | 承载内容 |
|---|---|
| **stdout** | 用户代码的所有输出。原样存入 `submission_case_results.output_text` 供用户调试 |
| **stderr** | 平台元信息。每完成一个用例输出一行 `@@OJ@@<JSON>` |

生成的 harness 逐用例输出：

```json
{"idx":0,"brief":"基础用例","args":"[2,7,11,15], target=9","time_ms":3,"mem_kb":2048,
 "output":"[0,1]","expected":"[0,1]","passed":true,"verdict":"AC","error":""}
```

**结果序列化规则**：容器类型（vector 等）序列化为人类可读格式 `0 1` 或 `[0, 1]`（由生成器按 `params` 类型决定）；标量按 `to_string`。字符串数组用空格分隔（LeetCode 风格）。

**比较规则**：
1. 用户 stdout 与期望值**先 trim 尾部空白**再比对
2. 数组/列表类型比对时**忽略元素间空白差异**（避免 `[0,1]` vs `[0, 1]` 误判 WA）
3. 浮点类型容差 `1e-6`

### 5.4 资源限制（setrlimit）

在每个用例的 fork 子进程中设置：

| 限制 | 值 | 超限表现 | 口径 |
|---|---|---|---|
| `RLIMIT_CPU` | `time_limit_ms / 1000 + 1` 秒 | `SIGXCPU` → 进程终止 → 判为 **TLE** | CPU 时间（与核数无关） |
| **`RLIMIT_AS`** `[D-63]` | `mem_limit_kb * 1024` 字节（默认 128MB） | `bad_alloc` / 启动失败 → 判为 **MLE** | **虚拟地址空间**，非 RSS |
| `RLIMIT_FSIZE` | 1 MB | 写盘超限 → 判为 **MLE** | 磁盘 |
| `RLIMIT_NOFILE` | 64 | 打开文件超限 → 判为 **RE** | 文件数 |
| `RLIMIT_CORE` | 0 | 禁止 core dump | — |
| **CPU 停滞检测** `[D-62]` | 连续 `STALL_POLLS` 次轮询（间隔 `STALL_INTERVAL_MS`）CPU 时间增量为 0 | 平台侧 `SIGKILL` 整个进程组 → 判为 **TLE** | CPU 时间**增量** |
| 兜底墙钟（极宽松）`[D-62]` | `time_limit_ms × 10 + 5000` ms | `SIGKILL` 进程组 → 判为 **TLE** | 墙钟 |

#### 5.4.1 为什么不能用「墙钟 > N 倍」来判阻塞 `[D-62]`

**v1.4 的写法是错的**：`墙钟 > time_limit_ms × 2 + 1000` 判 TLE。代数字就崩：

```
2 核 (§1.4) × 全局并发 8 (§1.4) = 每个进程最多分到 0.25 核
放大倍数 = 2 / 0.25 = 8

一个 CPU 耗时 2.9s 的合法用例（未超 3s 限制）
  在 8 并发下墙钟被拉长到 2.9 × 8 ≈ 23.2s
  旧阈值 = 3 × 2 + 1 = 7s
  → 23.2s > 7s  →  被 SIGKILL 判为 TLE

结论：旧阈值只允许约 2 倍放大，而并发闸门允许 8 倍 —— 差 4 倍。
      并发评测时，高 CPU 用量的合法解法会被系统性误杀。
```

注意 **`RLIMIT_CPU` 本身不会误判** —— CPU 时间与分核无关，2.9s 就是 2.9s。**唯一的误判源是这个墙钟保险阀。**

#### 5.4.2 改法：CPU 停滞检测 `[D-62]`

**判据从「墙钟超过 N」改为「CPU 时间增量为 0」**：

```c
// 平台侧：等待子进程时循环观测其 CPU 时间
const int STALL_POLLS      = 3;      // 连续 3 次不增长即判阻塞
const int STALL_INTERVAL_MS= 500;    // 观测间隔 500ms
                        // → 阻塞判定窗口 = 1.5s

long last = cpu_time_of_child();      // wait4/getrusage 读子进程
int  zero_streak = 0;
for (;;) {
    usleep(STALL_INTERVAL_MS * 1000);
    long now = cpu_time_of_child();
    if (now - last <= 0) {            // ← 关键判据：CPU 增量为 0
        if (++zero_streak >= STALL_POLLS) { kill(-pgid, SIGKILL); return TLE; }
    } else {
        zero_streak = 0;             // 一旦增长就清零
    }
    last = now;
    if (waitpid(pid, &st, WNOHANG) == pid) break;   // 正常结束
    if (wall_elapsed > time_limit_ms * 10 + 5000) { kill(-pgid, SIGKILL); return TLE; }
}
```

| 场景 | CPU 增量 | 判定 | 是否正确 |
|---|---|---|---|
| 8 并发下 CPU 耗时 2.9s 的合法解法 | **持续增长** | 不触发停滞；`RLIMIT_CPU` 未超 → **AC/WA** | ✓ 修复了误杀 |
| `while(true){}` 死循环 | 持续增长 | `RLIMIT_CPU` 超 → `SIGXCPU` → **TLE** | ✓ |
| `sleep(999)` | **恒为 0** | 1.5s 后停滞 → **TLE** | ✓ 这才是保险阀的真正目的 |
| `read()` 无输入的 stdin | **恒为 0** | 1.5s 后停滞 → **TLE** | ✓ |
| 极慢但不空转的 IO（罕见） | 缓慢增长 | 不触发，靠兜底墙钟兜住 | ✓ |

**为什么这个判据是对的**：`RLIMIT_CPU` 覆盖了"CPU 用得多"这一类，**唯一漏掉的是"完全不消耗 CPU 的阻塞"**。停滞检测精确地只补这一类，**不与 CPU 限制重复也不与之冲突**。

> **仍然保留兜底墙钟，但它不再是判阻塞的主力**。它只兜两种情况：进程完全僵死（连 `SIGKILL` 前都读不到 rusage）、以及系统时钟被异常修改。

#### 5.4.2.1 兜底墙钟与并发闸门的**定量关系式** `[D-62] [D-74]`

兜底墙钟必须满足一条不等式，否则它会退化成 §5.4.1 那个"系统性误杀"的元凶：

```
最大墙钟放大倍数 = 闸门 × (核心数 / 核心数) 的实际分母
                 = 闸门 × time_limit / (核心数 × 每进程可得核数)
                 简化为：放大倍数 = 闸门        （因为闸门 ≤ 核心数时进程各占一核，放大 = 1；
                                                闸门 > 核心数时每进程分 core/闸门 核，放大 = 闸门）

✅ 必须满足：  闸门 × time_limit_ms  <  time_limit_ms × 10 + 5000
               即：  闸门 < 10 + 5000 / time_limit_ms
```

**代入当前参数验算**：

| 闸门 | time_limit | 需要的墙钟 | 兜底阈值 | 余量 | 结论 |
|---|---|---|---|---|---|
| 4（v1.6 现值）`[D-74]` | 3000ms | 12.0s | 35.0s | **2.9×** | ✅ 安全 |
| 8（v1.0 原值） | 3000ms | 24.0s | 35.0s | 1.46× | ✅ 安全（但余量偏紧） |
| 10 | 3000ms | 30.0s | 35.0s | 1.17× | ⚠️ 临界 |
| 12 | 3000ms | 36.0s | 35.0s | **0.97×** | ❌ **开始误杀** |
| 20 | 1000ms | 20.0s | 15.0s | 0.75× | ❌ **误杀** |

> **结论一**：v1.6 把闸门从 8 降到 4，**兜底墙钟的余量从 1.46 倍升到 2.9 倍** —— 这次资源下调**没有连带削弱安全性**，反而更宽松。§1.4 的资源下调声明里"不需要下调系数"的结论，其依据就是这张表。
>
> **结论二**：**`× 10` 这个系数不是随手取的**，它的上界由"闸门 < 10"决定。若将来因换大机器而提高闸门（>10），**必须同步提高 `× 10`**，否则兜底墙钟会从"安全网"退化成"误杀器" —— 那就等于把 §5.4.1 修掉的 bug 又请回来。
>
> ⚠️ **这与 §6.3.6 的 `queueAhead` 依赖 FIFO 是同类的隐含契约**，两处都必须在代码里留注释：
> ```cpp
> // 【隐含契约 1】本阈值与「全局并发闸门」绑定。
> //   约束： 闸门 × time_limit_ms < 兜底阈值
> //   现状：  4 × 3000 = 12000 < 35000  ✓（余量 2.9 倍）
> //   改闸门前必须重算，改此系数前必须重算。
> // 【隐含契约 2】queueAhead 的 `id < ?` 依赖队列 FIFO，见 §6.3.6。
> ```

#### 5.4.3 进程组隔离与工作目录

**进程组隔离**：`fork()` 后在子进程中 `setpgid(0, 0)`，超时时 `kill(-pgid, SIGKILL)` 杀整组，防止用户代码 fork 出孙进程后逃逸超时判定。**停滞检测与兜底墙钟两条路径都必须走进程组 kill**，不能只杀直接子进程。

**工作目录隔离**：子进程 `chdir(/tmp/oj-judge/<id>/)`，stdin 重定向到该目录下的 `case_in.txt`。

**已知限制（诚实声明）** `[D-04]`：本方案不防恶意逃逸。用户在同机有 shell 权限时可读取任意可读文件（MySQL 数据、他人代码）。缓解措施仅限于 §1.4 的限流配额。系统定位为**个人单机使用**，若开放给不可信用户需替换为 Docker/nsjail 方案（见 §8 风险 R-1）。

### 5.5 耗时与内存度量 `[D-12]`

```cpp
// tester.cpp 中，每个用例前后各取一次
struct rusage r0, r1;
getrusage(RUSAGE_SELF, &r0);
sol.twoSum(nums, target);          // 执行业务逻辑
std::cout.flush();
getrusage(RUSAGE_SELF, &r1);
long cpu_ms = (r1.ru_utime.tv_sec  - r0.ru_utime.tv_sec)  * 1000
            + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1000
            + (r1.ru_stime.tv_sec  - r0.ru_stime.tv_sec)  * 1000
            + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1000;
long mem_kb = r1.ru_maxrss;        // 进程峰值，非增量
```

**时间**：`ru_utime + ru_stime` 的前后差值，精确反映业务逻辑耗时，不受 harness 自身与 IO 影响。

**内存精度限制**：`ru_maxrss` 是**进程生命周期峰值**而非单用例增量，因此第 N 个用例报出的内存是"跑到第 N 个用例为止的最高水位"。首版接受此精度，界面标注"内存为进程峰值"。若后续需要精确值，可改为平台侧 `wait4` 逐用例 fork（成本：每用例多一次 fork）。

#### 5.5.1 限制口径与度量口径**不是一回事** `[D-63]`

这是 v1.4 未声明、但**必然被用户撞见**的一处口径分裂：

| 角色 | 口径 | 位置 |
|---|---|---|
| **限制**（超了会死） | `RLIMIT_AS` = **虚拟地址空间** | §5.4 |
| **度量**（展示给用户） | `ru_maxrss` = **常驻集 RSS** | §5.5 |

```
一个只用 20MB 数据的 C++ 程序：
  ru_maxrss（RSS）              ≈  24 MB   ← 用户在界面上看到的
  虚拟地址空间（AS，含映射预留） ≈  96 MB   ← RLIMIT_AS 实际在拦的东西
                                         （libstdc++ 映射 + malloc arena
                                          + 线程栈预留 + 预分配的空闲页）
```

**后果（必须诚实告知，不能掩盖）**：

1. 用户可能看到「内存超限 (128MB)」，而 harness 报的峰值 RSS 只有 30MB —— **界面两个数字对不上**
2. `RLIMIT_AS` 128MB 比 LeetCode 的 128MB（RSS 口径）**严格得多**，留给业务数据的空间实际约 100MB 出头
3. C++ 运行时基线开销（AS 口径）约 60~80MB，具体随 glibc 版本、`MALLOC_ARENA_MAX`、是否多线程而变

**决策：保留严格值** `[D-63]`。理由与代价：

| 理由 | 说明 |
|---|---|
| 128MB AS 对绝大多数面试题足够 | 高频题型（数组/哈希/双指针/链表/树/二分/DP）的最大用例数据量在 10^4~10^5 量级，实际业务占用 < 20MB |
| 收紧的 AS 限制**恰好挡住了**一类真实逃逸 | 反复 `new`/`delete` 不释放（泄漏）会推高 AS，比单纯推高 RSS 更早触发，判 MLE 是合理的 |
| 换 RSS 口径要引入 cgroup / `RLIMIT_RSS` | `RLIMIT_RSS` 在 Linux 上**自 2.6.30 起就不再强制执行**（内核忽略），要真按 RSS 限只能上 cgroup v2 —— 那是**另一个部署形态**，与 D-33「双容器」冲突 |

**代价与必须配套的三件事**：

1. **UI 必须标注口径**：逐用例表的内存列表头写 `峰值RSS(参考)`，状态徽标写 `内存超限 · 地址空间 128MB`，**两者不并列显示成同一个数字**（否则用户会以为是 bug）
2. **题目录入时校准**（§4.3 新增一步）：树/图/大矩阵这类天然大内存的题，**必须逐题调大 `mem_limit_kb`**（§8 R-9 已列此风险），不能套用默认 128MB
3. **D-11「贴近 LeetCode 严格值」需要修正措辞** —— 见 §1.4 的口径声明

> **若将来 `mem_limit_kb` 需要放宽，优先放宽内存而不是 CPU**，因为 §5.4.2 的停滞检测已经让 CPU 侧对并发放开了；而内存侧的口径分裂是**不可消除的**（除非换 cgroup），只能靠标注和逐题配置来管理。

**被杀死用例的度量缺失**：度量代码在 harness 内逐用例执行，而 TLE/MLE 的子进程被 `SIGKILL` 杀死，harness 来不及输出该用例的 `@@OJ@@` 行 —— 这类用例的 `time_ms` / `mem_kb` 因此为 NULL（不是 0）。UI 按 §7.2 渲染为"超出时间限制 (3000ms)"跨列显示。存储侧统一用 `COALESCE(...,0)` 收口聚合，理由见 §3.4。

### 5.6 编译错误解析 `[D-11 衍生]`

解析 g++ 输出，抽取定位信息用于编辑器行号高亮：

```
/tmp/oj-judge/42/user.cpp:7:20: error: 've' was not declared in this scope
   7 |     vecto<int> ans;
     |     ^~~~
```

正则：`^(?<file>[^:]+):(?<line>\d+):(?<col>\d+):\s*(?<sev>error|warning|note):\s*(?<msg>.+)$`

解析结果写入 `submission_compile_errors`，每条诊断一行（`seq` 保序）：

```json
[{"file":"user.cpp","line":7,"col":20,"severity":"error","message":"'ve' was not declared in this scope"}]
```

上面是 API 层的呈现形式；DB 中对应两行 `file_name='user.cpp'` / `line=7` / `col=20` / `severity='error'` / `message='ve' was not declared in this scope'`。前端将编辑器对应行号标红，鼠标悬停显示 message，同时面板中展示**原始编译器输出全文**（不解析的 warning/note 也保留）。

**文件路径归一化与归属判定** `[D-64]`：把绝对路径前缀 `/tmp/oj-judge/<id>/` 剥掉，再按文件名分流到**两种归属**：

| 生成文件 | 归一化 `file_name` | 归属 | 前端表现 |
|---|---|---|---|
| `user.cpp` | `user.cpp` | **用户的代码** | 行高亮 + 可点击定位（原有行为） |
| `tester.cpp` | `harness` | **平台生成的测试代码** | **不高亮、不提供行号**；显示专属文案 |
| `solution_decl.h` / `tester_util.h` | `harness` | 同上 | 同上 |

**`harness` 归属的专属文案**（替换原始 message）：

> ⚠️ 这不是你的代码出错。平台为本题自动生成的测试代码编译失败，**通常说明该题的配置有误**。你的代码无需修改；若确认代码正确，请联系管理员检查题库。

**为什么必须这样分流** `[D-64]`：两阶段编译的**第二阶段会编译平台生成的 `tester.cpp`**。若题库有类型错误（如 `return_type: int` 但 `expected: [1,2]`，生成 `std::vector<int> __exp = {1,2}`），错误指向 `tester.cpp:123` —— 而**用户改 `tester.cpp` 毫无作用**（下次提交会重新生成）。若按 v1.4 的做法只保留 `user.cpp`，这类错误会落到"未识别的行"，留在 `compiler_output` 里裸奔，用户对着"你在 tester.cpp:123 出错"却**在编辑器里找不到任何可高亮的行**。

**链接期错误（`undefined reference`）的归属判定** `[D-64]`：第二阶段链接器的输出**没有 `file:line:col`**，正则匹配不到。v1.4 完全没定义它怎么呈现。规则：

```
若 compiler_output 命中 /undefined reference|undefined symbol/：
    verdict 仍为 CE，但 file_name = 'link'
    message = "你的 Solution 类的方法名或函数签名与题目要求不一致。
               请核对题面「函数签名」一节，确保类名、类名方法名、参数类型、返回类型完全一致。
               常见原因：漏写 public、少了 vector<int>& 的引用符号、用了 int 而非 long long。"
    前端：**不提供行高亮**（定位信息本来就不存在）
```

**解析失败降级**：正则未命中的行原样保留在 `compiler_output`，`submission_case_results` 不受影响；`submission_compile_errors` 只装成功解析的部分。**`compiler_output` 全文始终展示**，这是用户排障的最后一道兜底。

### 5.6.1 题库类型一致性静态校验 `[D-64]`

§5.6 的归属分流是**兜底**，不是根治。真正的根治是在**生成 C++ 之前**发现题库的类型错误 —— 否则每个用户第一次提交那道题时都会撞上 `harness` 编译错误。

**题库加载器（`problem/loader.h`）在同步入库前必须做的静态校验**：

| 校验 | 规则 | 失败处理 |
|---|---|---|
| `expected` 容器深度 | `expected` 是 JSON 数组 ⇔ `return_type` 含 `std::vector` 层 | **跳过该题**，`ERROR` 日志点名，`case_count=0` 且该题不出现在列表页 |
| `expected` 元素类型 | `return_type=std::vector<int>` 时元素必须是 number | 同上 |
| `args` 数量 == `params` 数量 | 一一对应，顺序即 `param_idx` | 同上 |
| `args[i]` 容器深度 | 与 `params[i].cpp_type` 的 `vector` 层数一致 | 同上 |
| `method_name` 在 `signature` 中出现 | 防止 harness 调用一个签名里没有的方法 | 同上 |
| `params`/`cases` 非空 | §4.1 已定 | 同上 |
| `__expr` 配对 | 用 `__expr` 的题必须有非空 `harness_prelude` | 同上 |

> **"跳过该题"而不是"整库启动失败"**：50 道题里 1 道写错，不该让整个服务起不来。该题 `problems` 行也不插（或插入后 `case_count=0` 并标记跳过），**启动日志汇总所有被跳过的题**。
>
> **实测修正（2026-10-15，`tests/run_problemsync_test.sh` 钉死）**："标记跳过"里的**"列表页过滤掉"当前并不成立** —— `GET /api/problems` 直接查 `problems` 表且没有失效标记列，所以校验失败的题**仍会出现在列表页**，只是一个 `caseCount=0`、点进去没有用例的空题。判题缓存（`problem::cache`）确实不再有它。补齐这个过滤需要给 `problems` 加失效标记列并同时改两处读侧，留作后续任务；在此之前，SPEC 这一句以本条修正为准。
>
> **静态校验查不出什么**：`__expr` 逃生舱的内容是任意 C++ 表达式，**无法静态校验**。所以 `harness` 归属的报错路径（§5.6）**必须保留**，两者是互补关系 —— 静态校验挡常见错误，归属分流兜住剩余。

### 5.6.2 verdict 的完备判定表 `[D-66]`

v1.4 **只画了"逐用例 fork"，从未定义崩溃怎么判**。后果是两种完全不同的情况在"没有 `@@OJ@@` 行"这一事实上无法区分：

```
情况甲：harness 跑第 3 个用例时用户代码崩了   → 有 @@OJ@@ ×2，第 3 个没有  → 应判 RE
情况乙：harness 在 main() 之前就崩了        → 有 @@OJ@@ ×0                  → 应判 JE
        （平台自身 bug，如生成器产出非法 C++、__expr 写错语法）
```

若全判 RE，用户会看到"运行错误"然后去逐行检查自己完全正确的代码。**判定表**：

| # | 观测到的事实 | verdict | 说明 |
|---|---|---|---|
| 1 | 两阶段编译失败（阶段一，`user.cpp`） | **CE** | 附 `user.cpp` 定位（§5.6） |
| 2 | 两阶段编译失败（阶段一，`user.cpp`）**且含超长编译**（>10s） | **CE** | `message = "编译超时"` |
| 3 | 两阶段编译失败（阶段二，`harness`） | **CE** | 附"题库配置有误"文案 `[D-64]` |
| 4 | 两阶段编译失败（第二阶段，链接器 `undefined reference`） | **CE** | 附签名核对文案 `[D-64]` |
| 5 | 编译通过，`@@OJ@@` 行数 **= 0** | **JE** `[D-66]` | **平台错误**，非用户错误。`message = "测试程序未能运行（平台错误），请重试或联系管理员"` |
| 6 | 编译通过，`@@OJ@@` 行数 > 0，某用例**非零退出码** | **RE** | 该用例 `error_msg` 取 stderr 摘要 |
| 7 | 编译通过，全部用例有 `@@OJ@@` 且 `passed` 全 true | **AC** | |
| 8 | 编译通过，至少一个用例 `passed=false` 且无 TLE/MLE/RE | **WA** | |
| 9 | 某用例 CPU 超限（`SIGXCPU`）或停滞检测触发 | **TLE** | 该用例无 `@@OJ@@` 行 |
| 10 | 某用例 `bad_alloc` / 启动失败 | **MLE** | 该用例无 `@@OJ@@` 行 |
| 11 | 用例跑完后 worker 侧异常（写回失败、DB 断连） | **JE** | `status='failed'` 而非 `done`（§5.7） |

> **第 5 条是本表的核心** `[D-66]`。判据是"**一条 `@@OJ@@` 都没产出**" —— 因为 harness 保证每个跑完的用例都会输出一行，0 行只可能是"根本没跑起来"。而第 6 条的 RE 至少有一行产出作证，**这个"至少一行"就是区分 RE 与 JE 的唯一依据**。
>
> **顶层 verdict 的收敛规则**：逐用例 verdict 存在**优先级** —— `JE > TLE > MLE > RE > WA > AC`（基础设施错误 > 资源超限 > 运行崩溃 > 答案错）。单个用例的 RE 不会让整次提交变成 RE，而是顶层报 `verdict='RE'` 且 `case_passed` 照常统计已通过数。
>
> **`case_passed` 在有 TLE/MLE/RE 的提交里仍然有意义**：它统计**已产出 `@@OJ@@` 且 `passed=1` 的用例数**。所以"通过 2/3，第 3 个超时"是自洽的，`case_total` 见 §5.6.3 的修正。

### 5.6.3 CE 的 `case_total` 语义修正 `[D-65]`

v1.4 定义 `submissions.case_total = COUNT(submission_case_results)`。但**编译失败时一条用例都没跑**，明细行为 0 行 → `case_total=0` → §7.2 结果面板显示「通过 0/0」，而题库里明明有 5 个用例。

**修正后的定义**：

```sql
-- case_total = 本次提交"涉及"的用例总数
case_total = GREATEST( COUNT(submission_case_results), 当时的 problems.case_count )
```

| 场景 | 明细行 | `problems.case_count` | `case_total` | 语义 |
|---|---|---|---|---|
| AC / WA | 12 | 12 | **12** | 跑了 12 个 |
| TLE（第 3 个被杀） | 12 | 12 | **12** | 尝试了 12 个（被杀的用例**仍有行**，因其前序用例已输出） |
| **CE**（编译失败） | **0** | 12 | **12** | 一个都没跑，但"本该跑 12 个" |
| 提交后题库删了 3 个用例 | 12 | 9 | **12** | `GREATEST` 恰好给出历史真实值 ✓ |

**`GREATEST` 而不是 `COALESCE`**：这不只是修 CE 的 0 值问题，它**同时解决了题库变更带来的历史错位** —— 提交时的用例数可能与现在的 `case_count` 不同，`GREATEST(实际跑过的, 现在的题库用例数)` 在两个方向上都给出"这次提交涉及的用例数"。

**`case_passed` 保持 `COUNT(passed=1)` 不变**（CE 时为 0）。

**UI 配套** `[D-65]`：CE 提交**不渲染逐用例表格**（`caseResults` 为空数组，没有可渲染的行），只渲染编译错误面板。状态行显示「编译错误 · 12 个用例未执行」。

**一致性断言同步改**（§3.4）：断言表达式里的 `case_total` 校验改为与 `GREATEST(COUNT(r.id), 题库用例数)` 比较，且断言**必须 JOIN `problems`**。见 §3.4 的修订版本。

### 5.7 异步任务状态机

```
                    ┌─────────┐
   POST /submissions │ pending │
                    └────┬────┘
                         │ 调度器 pop
                    ┌────▼────┐
                    │ judging │──── 编译/执行异常 ────┐
                    └────┬────┘                       │
                         │ 全部用例跑完              │
                    ┌────▼────┐                       │
                    │  done   │  verdict ∈ {AC,WA,TLE,MLE,RE,CE}
                    └─────────┘                       │
                                                 ┌────▼────┐
                                                 │ failed  │ message = 内部错误
                                                 └─────────┘
```

**启动恢复逻辑** `[D-20]`：

```sql
-- 1. judging 态的任务无法恢复上下文，直接判失败（重跑会重复执行副作用）
UPDATE submissions SET status='failed', verdict='JE',
       message='服务重启导致评测中断，请重新提交' WHERE status='judging';
-- 2. pending 态的任务重新入队
SELECT id FROM submissions WHERE status='pending' ORDER BY id;
```

pending 重放的幂等性由设计保证：judge 全流程无外部副作用（不写用户数据、不发通知），重复执行安全。

> **为什么无需清理明细行、也不会撞唯一键** `[D-67]`：`status` 的迁移与明细行插入在**同一事务**内（T2.11），InnoDB 保证全提交或全回滚。因此可以证明不变量：
>
> > **`status ∈ {pending, judging, failed}` 的 submission 恒有 0 条 `submission_case_results` 行。**
> >
> > 三个终态之外的都成立：① `pending`/`judging` 是迁移前态，明细行与迁移同事务尚未提交；② `failed` 是**事务回滚**的结果，明细行同样不存在。
> >
> > 推论：恢复逻辑只需 UPDATE `submissions` 本身；重新评测时 `uk_sub_case` 上不可能存在冲突行。
> > 这条推论对重跑（D-58）也成立 —— 重跑是 `INSERT` **新**记录，原 `failed` 记录不被触碰。
>
> ⚠️ **若哪天把写回改成分批提交，必须同时在本节补上明细行清理逻辑**，否则会出现"判题中"的提交带着上一次重放的残留结果，且 §3.4 的一致性断言（v1.5 已覆盖 `failed`）会开始报错。

### 5.8 配额与限流 `[D-22]`

| 规则 | 阈值 | 超限响应 |
|---|---|---|
| 单用户同时评测中（pending+judging） | 2 | 拒绝提交，`code=1007 提交过于频繁，请等待当前评测完成` |
| 全局同时评测中 | 8 | 任务**入队但不启动**（排队等待，不拒绝） |
| 单用户每分钟提交数 | 10 | 拒绝提交，`code=1006` |
| 单次提交代码长度 | 64 KB | 前端拦截 + 后端二次校验 |
| 用户名长度 / 密码长度 | 3–32 / 6–64 | 参数校验失败 |

限流计数用**内存滑动窗口**（每用户 deque 存最近 60s 的提交时间戳），不落库。**deque 的 key 是 `user_id`，必须在注销时清掉** `[D-70]` —— 否则注销的用户仍占着内存，且其计数会以陈旧值残留（虽然 `user_id` 不会复用，但纯属无谓占用）。

> ⚠️ **「同时评测数」配额是 DB 派生量，不是内存计数** —— 它由 `SELECT COUNT(*) FROM submissions WHERE user_id=? AND status IN ('pending','judging')` 得出。因此**用户注销后该计数自动归零**（CASCADE 删了 submissions），无需额外清理。**只有 60s 滑窗是纯内存的**，需显式清理。

#### 6.3.2.1 CSRF 与 Cookie 属性的依赖声明 `[D-69]`

v1.4 完全没提 CSRF。结论是**不需要额外防护，但依赖是隐式的**，必须写明：

| 机制 | 状态 | 说明 |
|---|---|---|
| `SameSite=Lax` | **已具备，是实际防线** | `Lax` 只对**顶级导航的 GET** 放行跨站 Cookie；跨站的 `POST`/`DELETE`（表单与 `fetch`）**不携带** Cookie，因此跨站 CSRF 请求到不了鉴权层 |
| 响应含敏感信息 | 否 | 全部业务数据在同源下取（§6.3 CORS 段：前后端同源，不开 CORS） |
| XSS | **有防线** | 题面走 marked + DOMPurify；代码显示走 `<pre>`/CodeMirror 文本节点（不用 `innerHTML` 拼代码） |
| `Secure` 标记 | **首版不加** | HTTP 部署下加了会导致 Cookie 永不发送。**这是 v1.4 遗留的显式待办** |

> **⚠️ 启用 HTTPS 时必须同步做的三件事**（§6.2 说"nginx 反代时再启用 HTTPS"，此处是该承诺的兑现清单）：
> 1. `Set-Cookie` 加 `Secure`（否则明文通道仍可携带 Cookie）
> 2. 静态资源与 API 全部走 HTTPS，否则会出现「同站不同协议」——`SameSite` 把 `https://` 与 `http://` 视为**不同站**，Lax 的放行规则会失效
> 3. 确认代码中无硬编码的 `http://` 资源（CDN 链接已是 https，无碍）
>
> **判据**：本项目定位单机自用（§5.4 已诚实声明不防逃逸），CSRF 的实际风险接近零。**显式写出依赖关系，是为了让"哪天去掉 SameSite"这个改动不能悄无声息地发生。**

---

## 6. 后端模块设计

### 6.1 目录结构

```
/home/ZL/project/cpp-oj-vibcoding/
├── SPEC.md
├── CMakeLists.txt                # 唯一构建规则（v1.8）：依赖发现 + ojcore 静态库 + oj 可执行 + 测试开关
├── Makefile                      # 【薄封装】只转发 cmake --build / ctest，保留 make build/run/test/clean（D-71）
├── .env                       # OJ_ALLOW_REGISTER / OJ_ADMIN_USERNAMES（D-37 角色的唯一真源）
├── .gitignore
├── docker-compose.yml         # 【路线 B 可选】D-71
├── Dockerfile                 # 【路线 B 可选】D-71
├── db/
│   └── init/01-schema.sql            # 路线 B 挂载到 /docker-entrypoint-initdb.d；
│                                    # 路线 A 由应用启动时幂等执行同一份 SQL
├── problems/                         # 题库 JSON（唯一真源）
│   ├── two-sum.json
│   └── ...
├── include/
│   ├── common/         config.h  log.h  response.h  errors.h
│   ├── db/             mysql_pool.h  dao.h
│   ├── model/          user.h  problem.h  submission.h
│   ├── problem/        loader.h  json_schema.h
│   ├── judge/          queue.h  scheduler.h  worker.h
│   │                  generator.h   # tester.cpp 代码生成器
│   │                  runner.h       # fork + setrlimit + CPU 停滞检测
│   │                  compiler.h     # g++ 调用 + 输出解析 + 归属分流
│   └── web/            routes.h  session.h  ratelimit.h  admin.h  # admin.h = require_admin 守卫 + overview
├── src/                （对应 .cpp）
├── third_party/              # 【可选】自部署头文件（仅当 §6.7.1 的 apt 版本不满足时）
│   ├── httplib/httplib.h
│   └── nlohmann/json.hpp
├── static/
│   ├── index.html                     # 落地页（大屏门面，v1.10 新增，见 §7.1）
│   ├── login.html  list.html  problem.html  history.html
│   ├── admin.html                       # 仅 admin，只读（D-37）
│   ├── css/  app.css  problem.css  admin.css
│   └── js/   api.js  auth.js  list.js  problem.js  history.js
│           editor.js  markdown.js  poll.js  common.js  admin.js
│           overview.js                  # v1.3：概览条 + 掌握分布的共享渲染与缓存（D-38/D-39）
│           diff.js                      # v1.4：LCS 行级 diff，纯计算无网络（D-42/D-50）
│   └── vendor/                        # 【前端三库 vendored，D-73】—— 完整清单见 §6.7.2
│       ├── cm/
│       │   ├── codemirror.min.js           # 核心（不含任何语言模式）
│       │   ├── mode/clike/clike.min.js     # C++ 高亮（注册 text/x-c++src）
│       │   ├── addon/edit/matchbrackets.min.js
│       │   ├── addon/edit/closebrackets.min.js
│       │   └── addon/comment/comment.min.js
│       ├── marked.min.js
│       ├── purify.min.js
│       └── LICENSES/                 # 三个库的 MIT LICENSE，vendored 进产物必须附带
├── logs/               # 路线 A：仓库内目录；路线 B：volume 挂载点（D-71）
└── tests/
    ├── CMakeLists.txt              # 单测目标：GTest + gtest_discover_tests（v1.8）
    ├── config_test.cpp             # config 模块单测（T0.4）
    ├── run_unit_tests.sh           # 单测薄封装：转调 ctest（v1.8）
    ├── run_tests.sh                # 端到端验收脚本（起服务 + curl + SQL，T5.3）
    └── problems/                   # 验收用特制题目
```

> **`overview.js` 为什么单列一个文件** `[D-38] [D-39]`：概览条（`list.html`）与掌握分布 tab（`history.html` tab2）消费的是**同一个端点的同一份数据**，AC-24 要求两者数值必然一致。把请求 + 聚合口径 + 渲染都收在这一个模块里，一致性就由"只有一份实现"保证，而不是靠约定。模块内含一个页面生命周期级的缓存，避免切 tab 或翻页时重复请求。
> `overview.js` 不引入任何框架，与其余 `js/` 模块同为原生 ES Module，由 `<script type="module">` 引入。
> **`diff.js` 同样是纯计算模块** `[D-42]`：只做「两段代码 → hunks」，不发请求、不碰 DOM。这样 AC-28 的性能要求（§1.4 的 P95 < 100ms）可被独立测量，也让它能脱离页面单独测。
>
> **`<script>` 的加载顺序有依赖** `[D-73]`：CodeMirror 的 5 个文件必须按 `codemirror.min.js` → `mode/clike` → `addon/*` 顺序加载（addon 依赖核心已注册），且全部加 `defer`。这五个文件缺一的后果各不相同 —— **最坑的是 addon：缺失时配置项静默失效不报错**（§6.7.2 有说明）。

### 6.2 第三方依赖

**服务端**（apt 包名、noble 实际版本、头文件路径的完整清单见 **§6.7.1**）：

| 库 | apt 包 | 版本 | 用途 | 说明 |
|---|---|---|---|---|
| cpp-httplib | **无 apt 包**（自装单头文件） | **0.60.1** `[D-72, v1.9 修订]` | HTTP 服务 | header-only 单文件，落在 `/usr/local/include/httplib.h`（**不是** apt 的 `/usr/include`）。`CPPHTTPLIB_OPENSSL_SUPPORT` 关闭（单机 HTTP） |
| MySQL C Connector | `libmysqlclient-dev` | 8.0.46 | 数据库 | `/usr/include/mysql/mysql.h`（`libmysqlclient-dev` 是其元包，装哪个都行） |
| nlohmann/json | `nlohmann-json3-dev` | 3.11.3 | JSON 解析 | header-only，`/usr/include/nlohmann/json.hpp` |
| **OpenSSL libcrypto** | `libssl-dev` | 3.0.13 | 密码哈希（Blowfish）`[D-75]` | **替代 `libbcrypt`** —— 24.04 官方源无 `libbcrypt-dev`；`/usr/include/openssl/blowfish.h` 已就位 |
| ~~bcrypt (`libbcrypt`)~~ | ❌ | — | — | **24.04 官方源不存在此包**，`apt-cache search bcrypt` 无结果。见 §6.2.1 |
| cpp-httplib OpenSSL | `libssl-dev` | 3.0.13 | HTTPS | **首版关闭**（§6.2 尾注），nginx 反代时再启用 |

> **cpp-httplib 0.60.1（自装单头文件）** `[D-72]` `[v1.9 修订]`：v1.6~v1.8 写的是"apt 的 0.14.3"，理由是**消除"宿主机编过、容器编不过"的版本漂移**（§6.7.1）。实测本机 apt 包**从未安装**，真正在用的是 `/usr/local/include/httplib.h` **0.60.1**，故真源改用它。代价与处置：
> - **代价**：0.60.1 晚于 SPEC 早期引用的 0.15.x，T0.5 接线时以**实际头文件**为准，不以本文档为准；
> - **漂移风险仍在，只是换了形式**：`libcpp-httplib-dev`（0.14.3）若被误装，`find_path` 可能命中 `/usr/include` 而**静默用了旧版**。CMake 因此在 configure 时打印**实际命中的路径与版本**，命中 vendored 兜底时额外 `WARNING`；
> - **路线 B 必须自备同一份 0.60.1**（把该头文件放进镜像），否则正是这条决策想消除的漂移；
> - `third_party/httplib/httplib.h`（0.14.3）**降级为环境缺失时的兜底**，不再是版本真源；两处 include 写法同为 `#include <httplib.h>`，**源码零差异**这条仍然成立。

**前端**（全部 **vendored** 到 `static/vendor/` `[D-73]`，**不在运行时引 CDN**；文件清单与下载命令见 §6.7.2）：

| 库 | 版本 | 用途 | 文件数 |
|---|---|---|---|
| **Ace**（**已取代 CodeMirror 5**，见下方说明） | 1.32.7 | 代码编辑器 | **4 个**（核心 + `mode-c_cpp` + `theme-tomorrow` + `ext-language_tools`） |
| CodeMirror 5 | 5.65.16 | ~~代码编辑器~~ **已弃用** | 5（`static/vendor/cm/`，**当前无引用**，待清理） |
| Marked | 12.0.2 | 题面 Markdown 渲染 | 1 |
| DOMPurify | 3.1.6 | XSS 过滤（配合 marked） | 1 |

> **编辑器从 CodeMirror 5 换成 Ace（2026-10-15，T4.5 实施时）**：这是一次**实现层的替换，不改任何接口与规格**。两条理由：
> 1. **行级诊断要能带 message**。§7.3 要求"编译错误高亮后**悬停显示 message**"，而 CodeMirror 的 `addLineClass` 只能改背景色，悬停要额外自己实现；Ace 的 `session.setAnnotations` 天然携带文本。
> 2. **少一半的加载顺序坑**。§6.7.2 记着 CodeMirror 必须按"核心 → mode → addon"顺序加载 5 个文件，且 addon 缺失时**静默失效不报错**；Ace 只需 4 个文件，且本页把 `useWorker` 显式关掉后不再有任何按需拉取。
>
> **连带的三点**：
> - **关闭 Ace 的 C++ 语法检查 worker**：① 编译诊断的真源是服务端 g++ 输出（`submission_compile_errors`，§5.6），两套诊断同时开着会让编辑器红线与判题结论不一致 —— 那是把权威交给了次优来源；② cdnjs 的 ace 包**不含 `worker-*.js`**，而本机到 `raw.githubusercontent.com` 不通，开了只会 404 且不报错。
> - `static/vendor/cm/` 下 5 个 CodeMirror 文件当前**无任何引用**，保留仅为回退可能；清理时连带 §6.7.2 的 5 条下载命令一起删。
> - 体积：CM 约 236KB → Ace 约 545KB（含 BSD LICENSE），对本机无影响（静态文件不常驻内存）。
>
> ⚠️ **CodeMirror 5 不是单文件**（保留此条以备回退）：实测 `codemirror.min.js` 中 `clike` 出现次数为 **0** —— 它只是核心与 JS 基础语法。说明见 §6.7.2。
>
> **为什么 vendored 而非 CDN** `[D-73]`：① R-7 的"CDN 不可达"风险**从源头消除**，降级到 textarea 的路径随之降为防御性冗余、不强制实现；② §1.1 的"离线"定位在演示场景下（断网）只有 vendored 能挡；③ 代价是镜像多约 550KB 与附带 LICENSE 文件，量级可忽略。

### 6.2.1 密码哈希：为什么是 OpenSSL Blowfish 而非 libbcrypt `[D-75]`

§6.2 早期写的是「bcrypt (`libbcrypt`) 或用 OpenSSL `EVP` 自行实现 Blowfish」。在 Ubuntu 24.04 上做了实测，**备选变成了唯一选项**：

```bash
$ apt-cache search bcrypt
libcrypt-dev            # glibc 的 crypt()，不是 bcrypt
libcrypt-openssl-*      # Perl 模块
...                     # 无任何 libbcrypt 包

$ apt-cache policy libbcrypt-dev
（无 Candidate）
```

| 方案 | 24.04 可用性 | 结论 |
|---|---|---|
| `libbcrypt-dev` | ❌ 官方源无此包 | 只能从源码编译 bcrypt（引入 vendored C 源码 + 自维护） |
| **OpenSSL `libcrypto` 的 Blowfish** | ✅ `/usr/include/openssl/blowfish.h` 已就位 | **采用** |
| glibc `crypt()` | ⚠️ 可用但**不保证算法是 bcrypt** | 不可控，不采用 |

**实现要点（4 条）**：

1. **自己实现 bcrypt 的 cost 调度**。OpenSSL 只提供 Blowfish 底层原语，**没有 bcrypt 的 EksBlowfishSetup / 盐值编码 / base64 变体**。这部分约 200~300 行，需要仔细对照 RFC 6234/Provos 的测试向量。
2. **必须实现自带的测试向量校验**。这一点不可省 —— 上述实现最容易出的错是 base64 变体的字母表顺序（bcrypt 用的是 `./A-Za-z0-9` 顺序，**不是标准 base64**），错了会导致"自己算的 hash 与预期不符"且极难察觉。
3. **cost=10**（`users.password_hash` 的注释保持不变）。1967MB 机器上单次 bcrypt 约 60~100ms，与 §6.3.5 的改密接口在全局 `DB mutex` 内的耗时可接受（< 200ms，远小于 §5.8 的单用户提交限制）。
4. **`password_hash` 列宽 255 够用**。bcrypt 的标准输出是 60 字符（`$2b$10$` + 22 盐 + 31 哈希），255 有充足余量，无需改 DDL。

> **为什么不为省 200 行代码而降低算法强度**：不能退回 MD5/SHA-256（§8 曾列为不可接受）。自实现 bcrypt 是一次性成本，而密码哈希是全系统唯一不可逆的信任根。
>
> **验收锚点**：AC-1（注册登录）之外，新增 **AC-49** 用 RFC 6234 官方测试向量验证自实现与参考实现逐字节一致。

### 6.3 API 契约

**统一响应包络** `[D-29]`：

```json
{ "code": 0, "message": "ok", "data": { } }
```

失败时 `data` 可携带字段级错误：

```json
{ "code": 1001, "message": "参数校验失败", "data": { "field": "password", "reason": "长度不足 6 位" } }
```

#### 错误码表

| code | HTTP | 含义 | 典型场景 |
|---|---|---|---|
| 0 | 200/201 | 成功 | — |
| 1001 | 400 | 参数校验失败 | 用户名为空、密码过短、代码超长、slug 不存在格式 |
| 1002 | 401 | 未登录 | 未带 Cookie 或会话过期 |
| 1003 | 403 | 无权限 | 访问他人提交详情；**非 admin 访问 `/api/admin/*`** |
| 1004 | 404 | 资源不存在 | 题目/提交 ID 不存在 |
| 1005 | 409 | 冲突 | 用户名已注册 |
| 1006 | 429 | 请求过于频繁 | 每分钟提交数超限 |
| 1007 | 429 | 配额超限 | 单用户同时评测数超限 |
| 1008 | 500 | 服务器内部错误 | DB 异常、未捕获异常 |
| 1009 | 503 | 判题服务不可用 | judge 调度器未启动/队列满 |
| 1010 | 403 | 注册已关闭 | `OJ_ALLOW_REGISTER=0` 时的注册请求 `[D-37]` |
| 1011 | 409 | 当前状态不允许该操作 | 重跑一条**非** `failed`/`JE` 状态的提交 `[D-58]`；未登录状态下点提交/收藏 `[D-43]` |
| 2001 | 200 | 编译错误 | verdict=CE，非 API 错误 |
| 2002 | 200 | 运行超时 | verdict=TLE |
| 2003 | 200 | 内存超限 | verdict=MLE |
| 2004 | 200 | 答案错误 | verdict=WA |
| 2005 | 200 | 运行错误 | verdict=RE |

> **约定**：2xxx 段的"错误"是**判题业务结果**，通过 HTTP 200 + `data.verdict` 表达，不作为 API 异常处理。

#### 端点清单

**认证**

| Method | Path | Auth | 说明 |
|---|---|---|---|
| POST | `/api/auth/register` | 否 | `{username, password}` → `201 {userId, username, role}`。**先查 `OJ_ALLOW_REGISTER`**，为 0 时返回 1010 且不落库。**本端点不涉及提权** —— 角色一律由启动同步决定（§6.3.1），新注册用户恒为 `user` |
| POST | `/api/auth/login` | 否 | `{username, password}` → `Set-Cookie: sid=...; HttpOnly; SameSite=Lax; Path=/` `[D-69]`（**首版不加 `Secure`**，启用 HTTPS 时必补，见 §6.3.2.1）+ `{userId, username, role}`。**顺带触发过期会话回收**（§6.3.2） |
| POST | `/api/auth/logout` | 否 | 删除会话行，清 Cookie |
| GET | `/api/auth/me` | 是 | `{userId, username, role}`；未登录返回 401 |
| POST | `/api/auth/password` `[D-40]` | 是 | `{oldPassword, newPassword}` → `200 {message:"密码已修改，其他设备的登录已失效"}`。详见 §6.3.5 |
| DELETE | `/api/auth/account` `[D-56] [D-57]` | 是 | `{password}` → `200 {message:"账号已注销"}`。**硬删除**，详见 §6.3.7 |

> **本组不新增错误码**：旧密码错误与新密码长度不足复用 **1001**，靠 `data.field` 区分（`oldPassword` / `newPassword`）。这与 §5.8「用户名/密码长度」共用 1001 的既有约定一致 —— 同一类问题不占两个码位。

#### 6.3.1 角色声明与同步 `[D-37]`

**设计取向：声明式，而非事件式。** 管理员身份不是"某个注册时刻恰好是第一个"这种隐式、一次性、不可逆的状态，而是**运维在配置里写明的一句话**。`OJ_ADMIN_USERNAMES`（逗号分隔的用户名）是 `users.role` 的**唯一真源**，服务启动时同步一次。

```sql
-- 启动时执行一次，幂等
UPDATE users SET role='admin' WHERE username IN (?,?,...);     -- 名单内提权
UPDATE users SET role='user'  WHERE username NOT IN (?,?,...);  -- 名单外降权
```

**为什么不用"首个注册用户自动提权"**：该规则依赖"`users` 表为空"这个前提，而数据卷是持久的（§2.1 / D-33）。首次部署后条件立刻不再成立，于是：① 测试期间注册的临时账号会永久占用 admin 名额；② 真实用户之后注册永远是 `user`；③ `docker compose down && up`（NF-6）不清卷，卡死状态持续存在。而唯一修复途径是 shell 手改 SQL —— 等于"设计了角色却没有管理路径"。声明式名单彻底消除这个坑，且**能修复任何存量数据**。

**实现约束（4 条，缺一不可）**：

1. **必须区分「变量未设置」与「设置为空」**。
   - `getenv("OJ_ADMIN_USERNAMES") == NULL`（变量不存在，`.env` 缺失或名字拼错）→ **完全跳过同步**，一条 UPDATE 都不执行，`role` 列零变化。
   - `getenv(...)` 返回 `""`（显式留空）→ 执行同步，结果为**零 admin**。
   两者语义不同：前者是"我没配置，别动我"，后者是"我要收回所有权限"。若混为一谈，一次 `.env` 丢失 + `docker compose restart` 就会静默把所有人降权，且日志里看不出异常。
2. **双向同步，不能只提权不降权**。单向提权下，从 `.env` 删掉一个名字**不会回收**其管理员身份，越积越多。既然声明为唯一真源，就必须双向。风险（手滑清空名单把自己降权）由约束 1 的安全阀与约束 4 的回显兜住。
3. **名单内不存在的用户名逐个 `WARN` 点名，同步本身不失败**。`OJ_ADMIN_USERNAMES=alise`（拼错）会导致目标用户没被提权，而若只记一句"同步完成"就成了静默失败 —— 这是本方案唯一真正的失败模式，必须让它可见。缺失名单不影响其他用户提权。
4. **同步后无条件回显最终 admin 名单；结果为空时额外 `WARN` 提示去 `.env` 配置**。降权是**不可见**的操作（用户不会收到任何通知，下次访问 `/api/admin/*` 突然 403），没有启动日志回显就没有反馈。

**执行位置**：§9 T3.9 启动顺序中，建表校验之后、题库加载之后（与题库同步同一时机，复用同一套「改文件/改配置 + 重启」的运维模式）。**只在启动时执行**，运行期不轮询 —— 角色变更一律通过重启生效，这是刻意选择：单机工具不需要热更新，而「改了要重启」是显式且可预期的。

**如何切换**（这张表是本节存在的意义）：

| 我想做的事 | 操作 | 需重启 |
|---|---|---|
| 切换**账号** | `logout` → `login` | 否 |
| 把某人升为管理员 | `.env` 的 `OJ_ADMIN_USERNAMES` 加名字 → restart | 是 |
| 取消某人的管理员 | `.env` 删名字 → restart | 是 |
| 查谁是管理员 | 启动日志回显 / `GET /api/admin/overview` | 否 |
| 确认自己是不是管理员 | `auth/me` 的 `role` 字段 | 否 |
| 以他人身份登录（impersonation） | **不做**，见下 | — |
| 管理员查看他人提交 | **不做**，见下 | — |

**为什么不做 impersonation**：它要求平台能以任意用户身份操作，审计要求最高（"这条记录是谁做的"变得不可追溯），而本项目定位是个人单机自用（D-01 / §5.4），收益远小于风险。

**为什么 admin 不绕过「仅本人」**：§5.4 已诚实声明用户持 shell 即可直连 MySQL 读取任意 `submissions` 行。在这种威胁模型下，"admin 能看全部提交"**不构成安全边界**，只是运维便利。AC-13 的归属校验因此保持严格不变。

**唯一恢复路径**：若 `.env` 配错导致无人是 admin（或管理员密码丢失），没有自助页面，只能走 shell（§5.4 已假定操作者有 shell）：

```bash
docker compose exec mysql mysql -uoj -poj oj \
  -e "UPDATE users SET role='admin' WHERE username='someone';"
```

> 注意这条命令的修改**会被下次启动的同步覆盖**（env 是唯一真源）。真正的修复是改 `.env` 再重启；shell 命令只用于救急。

**不做的事**（ conscious omission）：无题库在线编辑（与 D-30 冲突，见 §6.3.4）、无 impersonation、无 admin 越权查看他人提交、无用户禁用/删除、无注册审核或邀请码、无页面上修改角色（角色只经配置调整，避免出现"env 与页面两个写路径互相覆盖"）。

#### 6.3.2 过期会话惰性回收

**问题**：`sessions` 表有 `expires_at` 与 `idx_expire` 索引，但若没有回收任务，该索引**没有任何查询会用到**，表只增不减 —— 浏览器关掉后其会话行永久驻留。logout 只删自己那一行。

**方案：惰性回收**（不引入定时器、不新增线程）

| 触发点 | 动作 |
|---|---|
| 服务启动时 | 删一次全部过期会话 |
| 每次 `POST /api/auth/login` 成功路径 | 删一次全部过期会话 |

```sql
DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP();
```

**为什么惰性够用**：这张表**只因登录而增长**。没人登录时既没有新增行、也没有需要回收的行。所以"登录时顺手清"能覆盖全部实际增长场景 —— 唯一残留情况是"最后一次登录之后长期不再使用"，此时那批行的增速也恰好是零。

**为什么不用定时器/线程**：

- 独立线程：2 核机器上多一个常驻线程，换来的确定性对单机自用无价值。
- 复用调度线程的 `wait_for(1h)`：零线程，但把"会话回收"和"判题调度"耦在一起，§2.3 的线程职责会变模糊。

**执行位置与约束**：

- 走**同一个全局 `DB mutex`**（§2.3），不要绕开。回收发生在登录路径内，必须与"插入新会话行"处于**同一事务**或明确的前后关系 —— 若回收与插入分开提交，并发登录时可能删掉刚落库、尚未被客户端持有的会话。
- `idx_expire` 正是为这条语句设计的，**无需新增索引**（`EXPLAIN` 应为 `type=range`）。
- `expires_at` 与 `created_at` 存 UTC（§1.4 / D-17），故比较用 `UTC_TIMESTAMP()` 而非 `NOW()`，避免容器时区不是 UTC 时误删未过期会话。**这是本条唯一的正确性要点。**
- 回收是 `DELETE` 而非 `SELECT` 后删，表小（个人量级）且有索引，单条语句足够。
- 失败**不影响登录**：回收异常仅记日志，登录照常返回。

#### 6.3.3 只读管理端点 `[D-37]`

这是 `users.role` **第一次真正参与放行决策**的地方 —— 在此之前 `role` 只是被记录、无人读取（那会是一列死代码）。

| Method | Path | Auth | 说明 |
|---|---|---|---|
| GET | `/api/admin/overview` | 是 + `role='admin'` | 返回系统级运行状态 + 用户清单，见下 |

**授权守卫** `require_admin()`：解析 session Cookie → 取 `users.role` → 非 `admin` 返回 **1003**（复用现有码，不新增）。未登录仍返回 1002，顺序上先判登录再判角色。

**响应结构**：

| 块 | 字段 |
|---|---|
| `system` | `userCount` `adminCount` `problemCount` `caseCount` `submissionCount` `acCount` `failedCount` `judgingCount` `queueLength` `activeJudges` `judgeAlive` |
| `users[]` | `username` `role` `createdAt` `submissionCount` `acCount` `solved` `attempted` `favorites` `lastSubmitAt` `lastVerdict` |

`users[].solved` / `attempted` **直接复用 §6.4 的推导口径**（AC 优先，其次有提交），不另立一套算法 —— 否则管理页与列表页会显示不一致的进度。

**`system` 里有两项不在 DB 里**：

- `queueLength`（内存队列长度）、`activeJudges`（当前评测并发数）、`judgeAlive`（调度器存活）—— 都是 §2.1 判题调度器的**进程内状态**。

由此带来**两条实现约束（易被忽略）**：

1. **必须打通路由层到 scheduler 的访问路径**。§2.1 中 HTTP 层与 Judge 调度器是分离组件，T3 阶段需显式提供句柄或 accessor（如 `judge::scheduler::instance()`），否则该端点无法返回这三项。
2. **锁顺序**：读这三项需拿 scheduler 的锁，**绝不能在持有 `DB mutex` 时去拿**（§2.3 的既有约束：DB 锁仅在 DAO 调用期间持有）。**建议实现顺序**：先取 scheduler 快照 → 释放其锁 → 再取 `DB mutex` 查 DB 拼 `users[]`。反过来做（先查 DB 再补 scheduler）会引入一条 `DB mutex → scheduler lock` 的嵌套边，与既有锁顺序冲突。

**为什么是只读**：增删改（删用户、禁用、改角色、在线编辑题库）都需要额外设计：删除用户要决定 `submissions` 是留是清；禁用要有生效中间态；改角色会与 §6.3.1 的 env 声明式产生**两个写路径互相覆盖**的歧义。首版一律不做，角色只经配置调整（§6.3.1 的"不做的事"）。

#### 6.3.4 为什么不做题库在线编辑

若将来加管理后台，"在线编辑题目与用例"是最直觉的功能，但它直接违反 D-30 与 §2.2：题库 JSON 是**唯一真源**，且"运行时改题需重启"。在线编辑必须先回答一个问题：**改完写回 JSON 文件，还是让 DB 变成第二真源？**

- 写回 JSON：与 §5.2"题库代码永不改动"的安全边界冲突，且在容器内改宿主文件带来部署耦合。
- DB 为第二真源：需要新增"哪边优先"的仲裁、版本化与冲突检测 —— 远超单机工具的复杂度。

在这个问题有明确答案之前，此功能**不进 v1.2**。

**题目**

| Method | Path | Auth | 说明 |
|---|---|---|---|
| GET | `/api/tags` | 否 | `{algorithm:[...], source:[...]}` |
| GET | `/api/problems` | 否 | query: `keyword` `difficulty` `tags`(逗号分隔) `progress`(none/attempted/solved) `favorite` `sort`(default/featured/easiest/hardest) `page` `size` → `{list:[...], total, page, size}` |
| GET | `/api/problems/{slug}` | 否 | `{...题目详情, hint_md, tags, isFavorite, myProgress, nextRecommendation, recentSubmissions:[最近5条]}` `[D-42] [D-47] [D-59]` |
| POST | `/api/problems/{slug}/favorite` | 是 | 切换收藏 → `{isFavorite: bool}` |

**提交**

| Method | Path | Auth | 说明 |
|---|---|---|---|
| POST | `/api/submissions` | 是 | `{problemSlug, language, code}` → `201 {submissionId, status:"pending", queueAhead}` `[D-54] [D-60]` |
| GET | `/api/submissions/{id}` | 是（仅本人） | `{id, status, verdict, message, caseResults, compileErrors, compilerOutput, casePassed, caseTotal, totalMs, maxMemKb, code, createdAt, canRetry}` |
| GET | `/api/submissions` | 是（仅本人） | query: `problemSlug` `verdict` `page` `size`（默认 `size=20`）→ `{list:[...], total, ...}` `[D-61]` |
| GET | `/api/submissions/diff` `[D-42] [D-50]` | 是（两条**均**为本人） | query: `?from=12&to=15` → `{from:{id,code,createdAt}, to:{...}, hunks:[...]}`。见 §6.3.6 |
| DELETE | `/api/submissions/{id}` `[D-49]` | 是（仅本人） | `200 {message:"提交记录已删除"}`。见 §6.3.6 |
| POST | `/api/submissions/{id}/retry` `[D-49] [D-58]` | 是（仅本人） | `201 {newSubmissionId}` —— **新增一条**，原记录不变。见 §6.3.6 |

#### 6.3.6 队列位置、diff、删除与重跑 `[D-42] [D-49] [D-54] [D-58] [D-60]`

**① 队列位置 `queueAhead`**

`POST /api/submissions` 的响应新增一个字段：

| 字段 | 含义 |
|---|---|
| `queueAhead` | **你前面还有几个 pending 任务**。`0` = 马上开跑；`n` = 前端显示「前面还有 n 个评测」 |

计算方式（**不落库**，与 §3.5 同一立场）：

```sql
SELECT COUNT(*) FROM submissions
WHERE status = 'pending' AND id < ?;   -- ? = 本次新建的 submission id
```

> **为什么用 `id < ?` 而不是读队列真实长度**：内存队列在多 worker 场景下**同时存在 `pending`（未启动）与 `judging`（正在跑）两组**，只有 `pending` 的数量才回答"还要等多久"。用 `id` 比较之所以成立，是因为队列是 **FIFO 且按 `id` 升序出队**（§2.1），更小的 `id` 必然排在前面。
> ⚠️ **这条等价关系依赖 FIFO 假设** —— 若将来队列改为优先级队列，此处必须改为从 scheduler 读真实位置。这是一条需要在代码里留注释的隐含契约。
>
> **为什么不给全局队列长度**（D-60）：全局长度会泄露其他用户的并发负载（"现在有 7 个人在提交"），而这个数字对用户判断自己还要等多久毫无帮助。`queueAhead` 只回答自己的问题。

**② 归属校验**（diff / 删除 / 重跑三个端点共用，**不放宽 admin**）

```
① 取 session → 无 → 1002
② SELECT ... WHERE id = ? AND user_id = ?   ← 归属校验写进 WHERE，不单独查再比
   无行 → 1003
③ 再执行业务逻辑
```

**③ diff**

```
GET /api/submissions/diff?from=12&to=15
```

| 校验 | 失败返回 |
|---|---|
| `from` / `to` 缺失或非数字 | 1001 |
| 任一条不属于当前用户（含 admin 查他人） | 1003（AC-20 的立场不变） |
| `from == to` | 1001（无意义，diff 为空） |

**响应**：

```json
{
  "from": { "id": 12, "createdAt": "...", "code": "..." },
  "to":   { "id": 15, "createdAt": "...", "code": "..." },
  "hunks": [
    { "type": "equal",  "leftStart": 1, "lines": ["class Solution {", "public:"] },
    { "type": "remove", "leftStart": 3, "rightStart": null, "lines": ["    vector<int> twoSum(...);"] },
    { "type": "add",    "leftStart": null, "rightStart": 3, "lines": ["    vector<int> twoSum(...) {"] }
  ]
}
```

> **为什么 diff 在服务端算**：两条记录的归属校验必须走 SQL（不能信任前端传来的 id），所以这次请求本来就要打一次 DB。既然已经在服务端了，**把两段代码一起返回、由前端算 diff** 才是对的分工 —— 服务端算 diff 只会多一次传输（两边都传了原文）和一个额外的 N×M CPU 消耗位置。
> **diff 算法在纯前端**：LCS 动态规划，最坏 O(n·m)。§1.4 已论证 64KB 代码上限使这个规模可接受（典型 200 行 × 200 行 = 4 万次比较，毫秒级）。

**④ 删除**

```
DELETE FROM submissions WHERE id = ? AND user_id = ?
```

`submission_case_results` / `submission_compile_errors` 由外键 CASCADE 连带删除，**无需显式 DELETE**（与 §3.6 注销同一条 CASCADE 链）。

**⑤ 重跑**

| 前置条件 | 不满足时 |
|---|---|
| 该 submission 属于当前用户 | 1003 |
| `status = 'failed'` 且 `verdict = 'JE'` | **1011**「只有失败的提交可以重跑」 |

成功时**新增一条**记录（`D-58`）：

```sql
-- 同一事务；复用原代码，但不复用原 status / verdict / 明细行
INSERT INTO submissions (user_id, problem_id, language, code, status, created_at)
SELECT user_id, problem_id, language, code, 'pending', UTC_TIMESTAMP()
FROM submissions WHERE id = ?;
```

> **为什么不覆盖原记录**：覆盖会破坏 §3.1 确立的"提交历史不可变"前提；且 §3.4 的一致性断言（汇总列与明细行的同事务契约）要求覆盖时先删明细再重写，凭空多一个失败点。新增记录让重试走与普通提交**完全相同**的代码路径 —— **判题侧零改动**。
>
> **代价**：重跑与手动重提在历史里**无法区分**。要区分就得加 `retry_of` 自引用外键列（§3.6 明确不做）。个人自用场景下这个信息价值低，**用可解释性换简洁性**。
>
> **`canRetry` 字段**：`GET /api/submissions/{id}` 的响应带该布尔值，前端据此决定是否渲染「重跑」按钮，避免用户点了才收到 1011。

**提交详情的数据组装**（`GET /api/submissions/{id}`）：

```
1. SELECT * FROM submissions WHERE id=?              → id/status/verdict/message/
                                                        casePassed/caseTotal/totalMs/
                                                        maxMemKb/compilerOutput/code/createdAt
2. SELECT case_idx, verdict, passed, args_text, output_text,
          expected_text, time_ms, mem_kb, error_msg
   FROM submission_case_results
   WHERE submission_id=? ORDER BY case_idx ASC        → 序列化为 caseResults[]
3. SELECT file_name, line, col, severity, message
   FROM submission_compile_errors
   WHERE submission_id=? ORDER BY seq ASC            → 序列化为 compileErrors[]
```

三步都是预处理语句（§1.4 强制要求），字段名与行名一一对应，**API 契约对前端不变** —— 拆表是服务端实现细节。前端只感知 `caseResults` / `compileErrors` 两个数组，无需改动。步骤 1 的四个汇总列直接取用，不必在应用层从步骤 2 的行里现算（§3.4 的受控反范式）。

**统计**

| Method | Path | Auth | 说明 |
|---|---|---|---|
| GET | `/api/stats` | 是 | `{total, solved, attempted, favorites}` —— 轻量计数，供顶栏等处取一个数字用 |
| GET | `/api/me/overview` `[D-38] [D-39]` | 是 | `{totals, byDifficulty[], byTag[], recent[]}` —— **概览条与「我的」tab2 共用的唯一数据源**，见 §6.5 |

> **为什么保留 `/api/stats` 而不直接用 `/api/me/overview` 顶替**：`/api/stats` 是 4 个标量的 `COUNT`，`/api/me/overview` 要跑 `byTag` 的多表 `GROUP BY`（§6.5）。两者代价差一个数量级，而顶栏这类"只要一个已通过数"的场景不该为此付聚合的钱。**但凡要展示分布的，一律走 `overview`** —— 单一数据源保证概览条与 tab2 数值必然一致（AC-24）。

**静态资源**：`GET /*` → `static/` 目录。`/api/*` 之外的路径若文件不存在 → 返回 `404.html` 或重定向到 `list.html`。

**CORS**：前后端**同源部署**（同一 nginx/httplib 服务），无需 CORS 配置。开发期若前端单独起 http-server，需在 httplib 侧开启 `set_cors` 允许 `http://localhost:5500`。

#### 6.3.5 改密码端点设计 `[D-40]`

```
POST /api/auth/password   Cookie: sid=...
Body: { "oldPassword": "...", "newPassword": "..." }

200  → { "code":0, "message":"密码已修改，其他设备的登录已失效" }
1001 → data.field = "oldPassword"  （旧密码不匹配）
1001 → data.field = "newPassword"  （长度不在 6–64）
1002 → 未登录
```

**实现要点（6 条，缺一不可）**：

1. **必须验旧密码**。`bcrypt compare` 失败即 1001，不改任何数据。这既是防止误改，也是本端点唯一的实质安全约束。
2. **改密 + 踢会话必须在同一事务**：

   ```sql
   -- 同一事务，走同一个全局 DB mutex（§2.3 锁顺序约束）
   UPDATE users SET password_hash = ? WHERE id = ?;
   DELETE FROM sessions WHERE user_id = ? AND token <> ?;
   ```

3. **必须保留当前会话**（`AND token <> ?` 中的 `<>` 是要害）。删掉自己这一行等于「改完密码立刻被登出」，用户会以为改密失败。AC-21 专门验证这一点。
4. **旧密码错误与长度错误都要限速**。这里**不加**独立限流器（理由见下），但**必须**靠 `§1.4` 的"改密时人在已登录态"这一前提 —— 未登录根本进不来。攻击者拿不到会话就无法试探旧密码。
5. **成功后前端必须静默重拉 `auth/me`**。因为 `role` 的真源是 `.env`（§6.3.1），改密期间若运维改了名单，用户看到的角色徽标 `[D-41]` 可能已过期。
6. **不返回也不接受用户名**。改用户名会破坏 `OJ_ADMIN_USERNAMES` 的绑定（D-37：名单按 username 匹配），属于非目标。

> **为什么不加改密限流**：威胁模型是"已登录用户改自己的密码"，攻击者需先持有一个有效会话。旧密码正确性校验 + 登录态门槛已足够，再加"每小时 N 次"只是形式 —— 且会误伤"改错密码后再改回来"的正常用户。**若日后要开放给不可信用户，与 §5.4 的隔离升级一并处理**，不单独为改密加锁。
>
> **与「登出其他设备」的关系**：`D-40` 选的是"改密时自动踢掉其他会话"，**没有**独立的"登出其他设备"按钮。理由：两者是同一个动作的两种触发方式，独立按钮省下的那点代码不足以抵消一次额外的接口设计。若用户只是想起草稿还在别人电脑上，改密码比找按钮更快。

#### 6.3.7 注销账号端点 `[D-56] [D-57]`

```
DELETE /api/auth/account   Cookie: sid=...
Body: { "password": "..." }

200 → { "code":0, "message":"账号已注销" }
1001 → data.field = "password"   （密码不匹配）
1002 → 未登录
```

**实现要点（5 条）**：

1. **必须验密码**。与改密同理（§6.3.5 要点 1）—— 防止用户离开时电脑被人直接点了注销。
2. **一条 SQL 完成级联**，不逐表删：

   ```sql
   -- 走同一个全局 DB mutex（§2.3）
   DELETE FROM users WHERE id = ?;
   ```

   外键链：`users` →`CASCADE` `sessions` / `submissions` / `user_problems`，`submissions` →`CASCADE` `submission_case_results` / `submission_compile_errors`。InnoDB 在同一条语句内完成整条链。
3. **不动 `problems` / `problem_cases` / `tags`**。题库是全局共享资源（D-30 唯一真源在文件里），不属于任何用户。注销只删"这个人的痕迹"，题库对其他用户完好。
4. **草稿清理在前端做**，后端无从下手（草稿在 localStorage）。前端需遍历 `localStorage` 中所有 `draft:*` 与 `draftAt:*` 键删除 —— 因为草稿键**不含用户名**（§7.3），注销后若不清会留给下一个登录的人。**这是本端点最容易漏的一步。**
5. **内存限流计数器同步清理** `[D-70]`：`ratelimit::forget(user_id)` 清掉 §5.8 的 60s 滑窗 deque。"同时评测数"配额是 DB 派生量（随 CASCADE 自动归零），**只有滑窗需要显式清**。
6. **注销后清当前 Cookie**，并跳转 `/login.html?deleted=1`（让登录页显示一句"账号已注销"）。

> **硬删除 vs 软删除**：选硬删除（`D-57`），代价是**不可恢复**。这个选择能成立的前提是 §1.4 那条约束 —— 二次确认弹窗必须**逐项列出**将被删除的数据量（提交 N 条 / 收藏 M 题 / 会话 K 个），而不是一句"确定吗？"。若做成软删除（加 `status` 列 + 匿名化 username），用户可以"反悔"，但代价是：① 改 DDL 加列；② 匿名化后 `OJ_ADMIN_USERNAMES` 按 username 匹配的语义要重新定义（§6.3.1）；③ "已注销用户"的提交历史要不要计入统计口径（§6.4）又成一个新问题。**单机自用场景下，为"反悔"付这三笔不划算。**


### 6.4 进度自动推导 `[D-16]`

题目的个人进度不独立存储，由提交历史实时 JOIN 得出：

```sql
SELECT p.*,
  CASE
    WHEN EXISTS(SELECT 1 FROM submissions s
                WHERE s.problem_id=p.id AND s.user_id=? AND s.verdict='AC')
      THEN 'solved'
    WHEN EXISTS(SELECT 1 FROM submissions s
                WHERE s.problem_id=p.id AND s.user_id=?)
      THEN 'attempted'
    ELSE 'none'
  END AS my_progress
FROM problems p ...
```

收藏状态来自 `user_problems.favorite`，列表页 LEFT JOIN 一次取回，避免 N+1。

### 6.5 掌握分布的聚合口径 `[D-38] [D-39]`

`GET /api/me/overview` 的三块聚合由**两条 SQL + 一次轻查询**构成，全部在 SQL 内完成（§3.5 已论证不做派生列，§1.4 要求 P95 < 200ms）。

#### ⚠️ 头号陷阱：按标签聚合会重复计数

`problem_tags` 是多对多。一道挂了「数组 + 哈希表 + 链表」三个标签的题，如果先 JOIN `problem_tags` 再 `COUNT(*)`，它会被**计三次**；`solved` 若用 `SUM(verdict='AC')` 同样翻倍。表现为"标签通过率虚高、各项 `total` 之和远超题目总数"。

**正确写法：先各自 `DISTINCT problem_id` 收敛成集合，再 JOIN**：

```sql
-- byTag：按标签的掌握分布
SELECT t.name, t.category,
       COUNT(DISTINCT p.id)   AS total,
       COUNT(DISTINCT ac.problem_id) AS solved,     -- 有过 AC 的题
       COUNT(DISTINCT at.problem_id) AS attempted   -- 有过任意提交的题
FROM tags t
JOIN problem_tags pt ON pt.tag_id = t.id
JOIN problems     p  ON p.id     = pt.problem_id
LEFT JOIN (SELECT DISTINCT problem_id FROM submissions
           WHERE user_id = ? AND verdict = 'AC')  ac ON ac.problem_id = p.id
LEFT JOIN (SELECT DISTINCT problem_id FROM submissions
           WHERE user_id = ?)                       at ON at.problem_id = p.id
GROUP BY t.id, t.name, t.category
ORDER BY total DESC, t.name;
```

`byDifficulty` 同理，只把基表从 `JOIN problem_tags` 换成 `FROM problems`（不 JOIN 标签表）：

```sql
SELECT p.difficulty,
       COUNT(*)                          AS total,
       COUNT(ac.problem_id)              AS solved,
       COUNT(at.problem_id)              AS attempted
FROM problems p
LEFT JOIN (SELECT DISTINCT problem_id FROM submissions
           WHERE user_id = ? AND verdict = 'AC') ac ON ac.problem_id = p.id
LEFT JOIN (SELECT DISTINCT problem_id FROM submissions
           WHERE user_id = ?)                    at ON at.problem_id = p.id
GROUP BY p.difficulty;
```

> `byDifficulty` 基表不 JOIN 标签表，故无重复问题，`COUNT(*)` 即可（不必加 DISTINCT）。**`byTag` 基表必须过标签表，必须加 DISTINCT。** 两者写法不同不是笔误，AC-22 会分别验证。

#### 口径一致性契约

| 数字 | 唯一定义 | 复用点 |
|---|---|---|
| `solved` | 该用户对某题**存在至少一条** `verdict='AC'` 的提交 | §6.4 列表页进度、§6.3.3 `admin.html` 的 `users[].solved`、本节的 `byTag`/`byDifficulty` |
| `attempted` | 存在至少一条**任意状态**（含 CE/JE）的提交 | 同上 |
| `total` | `problems` 表中该难度/标签下的**题目总数**（与用户无关） | 仅本节 |

**「存在至少一条」而非「最后一次」是刻意选择**：用户某次蒙对了、后来改坏又提交失败，不应该把已通过的题从 `solved` 降级回 `attempted`。这与 LeetCode 的一致，且避免了"最后一次结果"口径在统计上更脆弱（一次误操作就抹掉记录）。

> ⚠️ `attempted` 包含 `verdict='CE'`（编译错误）。这符合 §10.1 AC-11 已定的口径（「仅编译错误的题标记尝试过」），**两处不得分叉**。

#### 索引与性能

| 查询 | 走索引 | 备注 |
|---|---|---|
| 两个 `DISTINCT problem_id` 子查询 | `submissions.idx_user_verdict (user_id, verdict)` | 覆盖 `WHERE user_id=? AND verdict='AC'` |
| `byTag` 的 `JOIN problem_tags` | `problem_tags` 主键 `(problem_id, tag_id)` + `idx_tag (tag_id)` | 双向都有索引，JOIN 方向不敏感 |
| `byDifficulty` 基表 | 全表扫描 `problems` | 50 行，无所谓 |

`AC-22` 会验证「一道挂 3 标签的已 AC 题在 3 个标签的 `total`/`solved` 中各计 1 次」，以及「`byDifficulty` 三个 `total` 之和 = 题目总数」。

### 6.6 「下一题」推荐的规则 `[D-45] [D-59]`

推荐卡片放**题目页底部**（`D-59`），数据由 `GET /api/problems/{slug}` 的 `nextRecommendation` 字段带回 —— **不单独开端点**，因为题目详情本来就要查，用户在同一屏内看到题面与建议，一次请求即可。

**规则（纯规则计算，无随机、无机器学习）**：

```
候选集 = 该用户「未通过」的题（progress ≠ 'solved'，含从未做过的）
        且该题至少挂了一个 category='algorithm' 的标签（D-68：无主标签者不参与）

对每个候选题 T：
    主标签(T) = T 的 algorithm 标签中「薄弱度最高」的那个              ← D-68
    score(T)  = 1000 / (1 + 薄弱度(主标签(T)))
    薄弱度(tag) = (tag_total - tag_solved) / tag_total
                 —— 0 表示全通过（不薄弱），1 表示全未通过（最薄弱）
同分时：difficulty ASC（先易后难）→ sort_no ASC（尊重题库编排顺序）
```

#### 6.6.1 主标签的选取规则 `[D-68]`

v1.4 三处引用了「主标签」却**从未定义怎么选** —— 一道题挂 3 个标签时无从下手，而 AC-34 要求「理由字符串与选中题的主标签一致」，没有规则就无法核对。

**规则：取该题所有 `category='algorithm'` 标签中，通过率最低的那个。**

| 情况 | 选取结果 | 理由 |
|---|---|---|
| 题挂 `数组`（8/10 通过）与 `哈希表`（3/10 通过） | **`哈希表`** | 推荐的目的就是补最薄弱项，选最强标签等于推荐了一个"你已经做得不错"的题 |
| 多个标签并列（都是 2/5） | `tags.id` 最小者 | 保证**结果稳定可复现**（否则同分排序受 JOIN 顺序影响，AC-34 无法核对） |
| 该题**无** `algorithm` 标签（只有 `source` 类） | **无主标签 → 该题不进入候选集** | `source` 类标签（"高频面试题"）不构成能力维度，用它算薄弱度无意义 |
| 该题未通过 | 照常选主标签 | — |

> **为什么不按题库里 `tags.algorithm[]` 的数组顺序取第一个**：那个顺序是**录题人随手写的**，与用户弱点无关。而推荐算法的整个目的是"把用户推向最该补的地方"，主标签必须由**用户的通过率**决定。
>
> **可解释性因此是内生的** `[D-59]`：理由字符串直接用主标签的通过率拼装，与选标签的规则同源 —— 不可能出现"推荐理由说的标签和实际算分的标签不是同一个"这种自相矛盾。

**可解释性是硬要求**（`D-59`）—— 卡片上必须写出推荐理由，用**算式本身**作为文案：

> **下一题建议：反转链表**
> 理由：`链表` 标签你通过 2/5，尚未通过且难度更低的候选题已刷完

**边界情况**：

| 情况 | 卡片表现 |
|---|---|
| 全部题目已通过 | **不显示推荐**，改为「🎉 题库 50/50 全部通过」+ 刷新题库的建议 |
| 无未通过的题但有从未做过的题 | 走正常规则（从未做过 = `progress='none'`，属未通过） |
| 用户未登录 `[D-43]` | **不显示推荐卡**（推荐依赖个人薄弱项，无 `user_id` 无法计算）。不显示而非显示"登录后推荐" —— 那是虚假的承诺 |
| 薄弱项并列（多个标签同为 1/5） | 按难度→`sort_no` 决定，展示的理由仍用实际选中的那个标签 |

> **为什么不用随机轮换**：面试备考的刷题路径应该是**可预期的** —— 用户点了"下一题"，期望看到的是系统认为最该补的那道。随机会让"薄弱优先"这个价值主张失效，页面退化成普通题库列表。
>
> **为什么不做学习路径/先修依赖**：那需要为题库额外维护 `prerequisites` 图（D-55 范围外的更大功能），且 50 道题的规模下价值有限。当前规则已能产出"先补最薄弱且最简单的"这一合理顺序。


---

### 6.7 环境依赖与安装（Ubuntu 24.04 / 路线 A） `[D-71] [D-72] [D-73] [D-75]`

按**空白 Ubuntu 24.04** 口径给出。开发机实测 2 核 / 1967MB（`D-74` 的下调依据）。

> ⚠️ **在这台机器上编译时，并发上限见 §0.0「开发机硬约束」：日常 `-j2`、sanitizer `-j1`。** 依赖装齐 ≠ 可以放开并发 —— 真正卡住的是内存而不是 CPU。

#### 6.7.1 包清单（12 个必需）

| 用途 | 包 | 版本 | 头文件 |
|---|---|---|---|
| 引导 | `ca-certificates` `curl` `git` | — | — |
| 工具链 | `g++-12` `gcc-12` `make` | 12.4.0 / 4.3 | — |
| | `libc6-dev` | 2.39 | `sys/{resource,wait,prctl}.h`、`unistd.h` |
| | **`cmake`** `[v1.8]` | 3.28.3 | — |
| 第三方库 | `nlohmann-json3-dev` | 3.11.3 | `nlohmann/json.hpp` |
| | **`httplib.h`（自装单头文件，非 apt）** `[v1.9]` | **0.60.1** | **`/usr/local/include/httplib.h`** |
| | `libmysqlclient-dev` | 8.0.46 | `mysql/mysql.h` |
| | `libssl-dev` | 3.0.13 | `openssl/blowfish.h` |
| 测试 | **`libgtest-dev`** `[v1.8]` | 1.14.0 | `gtest/gtest.h` |
| 数据库 | `mysql-server` `mysql-client` | 8.0.46 | — |

**明确不装**：`pkg-config`（库路径固定）、`build-essential`（连带装 g++-13，与 D-13 冲突）、`libbcrypt-dev`（24.04 官方源无此包，见 §6.2.1）、**`libcpp-httplib-dev`**（v1.9：apt 的 0.14.3 不是真源，且误装会让 `find_path` 命中旧版头文件，见 §6.2 尾注）。

> **`cmake` 曾经被列在这里，v1.8 起移回必需**。原理由是"构建用 Makefile，两者并存只会让人不知用哪个"—— 该理由已被"Makefile 降为薄封装、CMakeLists 成为唯一构建规则"消解。详见 §0 的 v1.8 说明。
> **`httplib` 不走 apt**（v1.9）：cpp-httplib 是 header-only 单文件库，安装动作就是把 `httplib.h` 放进 `/usr/local/include/`，无需编译与链接（命令见 §6.7.2）。实测本机已在该路径且为 0.60.1。
> `libc6-dev` 虽由 `g++-12 → libstdc++-12-dev → libc6-dev` 传递带入，仍显式安装 —— judge 的 `getrusage`/`wait4`/`prctl` 头文件全靠它。
> **`make` 仍需要**：它是 `make build/run/test/clean` 这层入口（D-71 的运维契约），不是构建工具本身。

#### 6.7.2 全部安装命令

```bash
# ---------- apt ----------
sudo apt-get update && sudo apt-get install -y \
  ca-certificates curl git \
  g++-12 gcc-12 make cmake libc6-dev \
  nlohmann-json3-dev libmysqlclient-dev libssl-dev \
  libgtest-dev \
  mysql-server mysql-client

# ---------- cpp-httplib（header-only 单文件，不走 apt）----------
# ⚠️ 装 0.60.1，与宿主一致；不要装 libcpp-httplib-dev（0.14.3，见 §6.7.1）
curl -sSL -o /tmp/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.60.1/httplib.h
sudo install -m 644 /tmp/httplib.h /usr/local/include/httplib.h
grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h   # 应为 "0.60.1"

# ---------- 构建（唯一构建入口是 CMakeLists.txt，Makefile 只是转发）----------
make build        # == cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-12 && cmake --build build -j
make test         # == ctest --test-dir build --output-on-failure（44 条 config 单测）

# ---------- 前端 7 个文件（vendored）----------
# ⚠️ 必须用 /npm/ 路径：/gh/ 实测对 codemirror.min.js 与 marked.min.js 均 404
cd /home/ZL/project/cpp-oj-vibcoding
V=5.65.16; B=https://cdn.jsdelivr.net/npm
mkdir -p static/vendor/cm/{mode/clike,addon/edit,addon/comment}

curl -sSL -o static/vendor/cm/codemirror.min.js                $B/codemirror@$V/lib/codemirror.min.js
curl -sSL -o static/vendor/cm/mode/clike/clike.min.js         $B/codemirror@$V/mode/clike/clike.min.js
curl -sSL -o static/vendor/cm/addon/edit/matchbrackets.min.js $B/codemirror@$V/addon/edit/matchbrackets.min.js
curl -sSL -o static/vendor/cm/addon/edit/closebrackets.min.js $B/codemirror@$V/addon/edit/closebrackets.min.js
curl -sSL -o static/vendor/cm/addon/comment/comment.min.js     $B/codemirror@$V/addon/comment/comment.min.js
curl -sSL -o static/vendor/marked.min.js                       $B/marked@12.0.2/marked.min.js
curl -sSL -o static/vendor/purify.min.js                       $B/DOMPurify@3.1.6/dist/purify.min.js

# ---------- MySQL 初始化 ----------
sudo mysqld --initialize-insecure --user=mysql
sudo mysqld_safe &
sudo mysql -e "SET GLOBAL innodb_buffer_pool_size = 67108864;"   # 1967MB 机器必调 64MB，否则与 judge 抢内存
sudo mysql -e "CREATE DATABASE IF NOT EXISTS oj DEFAULT CHARSET utf8mb4;
                CREATE USER IF NOT EXISTS 'oj'@'127.0.0.1' IDENTIFIED BY 'oj';
                GRANT ALL ON oj.* TO 'oj'@'127.0.0.1'; FLUSH PRIVILEGES;"
```

**CodeMirror 5 不是单文件** —— 核心 `codemirror.min.js` 实测 `clike` 出现 0 次，必须额外加载 `mode/clike`（C++ 高亮）与 3 个 addon。加载顺序 `core → mode → addon`，全部加 `defer`。**addon 缺失时配置项静默失效不报错**（如漏 `matchbrackets` 则 `matchBrackets: true` 无效）。

#### 6.7.3 `.env`

```bash
OJ_ALLOW_REGISTER=1
OJ_ADMIN_USERNAMES=alise        # 角色唯一真源（D-37），须为已存在的用户名
MYSQL_HOST=127.0.0.1
MYSQL_PORT=3306
MYSQL_USER=oj
MYSQL_PASSWORD=oj
MYSQL_DB=oj
OJ_LISTEN=0.0.0.0:8080
OJ_JUDGE_WORKERS=2              # [D-74]
OJ_JUDGE_GATE=4                 # [D-74] 必须 = 2 × workers
OJ_FENCE_MULTIPLIER=10          # [D-62] §5.4.2.1 的 ×10，改闸门前必须重算
```

> `OJ_JUDGE_GATE` → 墙钟放大倍数 → 兜底墙钟余量 → 压测口径（AC-38）**是一条链**，改一个要重算其余（§5.4.2.1）。

#### 6.7.4 验证（8 项，全绿才继续）

```bash
g++-12 --version | head -1                             # 应为 12.x
cmake --version | head -1                              # 应为 ≥ 3.20（实测 3.28.3）
ls /usr/include/gtest/gtest.h                           # 单测依赖（libgtest-dev）
grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h   # 应为 "0.60.1"（不是 apt 的 0.14.3）
ls /usr/include/{nlohmann/json.hpp,mysql/mysql.h,openssl/blowfish.h}
ls /usr/include/sys/{resource.h,wait.h,prctl.h}
find static/vendor -name '*.js' -size -1k | wc -l      # 应为 0（404 响应体只有几十字节）
mysql -h 127.0.0.1 -uoj -poj oj -e "SELECT VERSION();"
```

> **构建系统自检**（v1.8 新增）：`make build && make test` 应产出 `build/oj` 与全部单测，`ctest` 44 条全绿。
> **httplib 双路径**（v1.9）：CMake 优先用 `/usr/local/include/httplib.h`（0.60.1，configure 时打印实际命中路径与版本）；缺失则 `WARNING` 并回退 `third_party/httplib`（0.14.3，**版本不一致，视为环境未就绪**）；两者都没有才 `FATAL_ERROR`。

#### 6.7.5 可选：路线 B（Docker）

```bash
sudo usermod -aG docker $USER     # ⚠️ 必须注销重新登录才生效
sudo dockerd &                    # 无 systemd 时手动拉起
docker info >/dev/null && echo "docker 可用"
```

未执行第一步时的典型报错：`permission denied while trying to connect to the docker API at unix:///var/run/docker.sock`。

---

## 7. 前端设计

### 7.1 页面结构 `[D-32] [D-36] [D-37] [D-38] [D-39] [D-52]`

| 页面 | 路径 | 内容 |
|---|---|---|
| **落地页**（门面） | `/index.html` | **v1.10 新增**：Hero + 能力四卡 + 五步流程 + 三个技术决定 + 实时题库数字。**纯展示，不承担任何工作台职责**，见下方「与 `D-38` 的关系」 |
| 登录/注册 | `/login.html` | 切换式表单，登录成功跳 `list.html`。**不预填上次用户名** `[D-46]` |
| 题库列表（**兼首页**）`[D-38]` | `/list.html` | **顶部个人概览条** + 筛选栏 + 题目表格 + 分页 |
| 题目详情 | `/problem.html?slug=xxx` | 左右分栏工作台 + **提交 diff 面板** `[D-50]` + **下一题建议卡** `[D-59]` |
| **我的**（原「提交历史」）`[D-39]` | `/history.html` | 三 tab：**提交记录** `#records`（含删除/重跑）`[D-49]` / **掌握分布** `#progress` / **账户设置** `#account`（含注销）`[D-56]` |
| 运维概览 `[D-37]` | `/admin.html` | **仅 admin**：系统运行状态卡片 + 用户清单表。无任何写操作 |

页面间用普通 `<a>` 链接跳转。每个页面独立引入所需 JS 模块，无构建步骤、无框架。**页面总数仍是五页** —— v1.4 的 20 项决策**没有一项新增页面**。

> **⚠️ v1.10 起实际是六页：新增 `index.html` 落地页（对 `[D-38]` 的一次显式偏离）**
>
> | | `[D-38]` 原取向 | v1.10 实装 |
> |---|---|---|
> | 首页 | `list.html` **兼**落地页，一页两职 | `index.html` 只做门面，`list.html` 退回**纯工作台**（概览条 + 筛选 + 表格） |
> | 为什么当时不拆 | v1.3 的工作重心是"把数据凑齐"，对外的门面不是交付重点 | 页面闭环后，"这系统是什么 / 凭什么用它"需要一个独立落点 |
> | 边界 | —— | **不新增任何接口、不改任何决策、不新增任何 list.html 做不到的操作**；功能入口仍然只有 `list.html` |
> | 撤销成本 | —— | 删掉 `static/index.html` 即可，`/` 立刻回到 404（httplib 的目录索引行为） |
>
> **`D-38` 那句「不新增首页」的原始意图没有被推翻**：它要防的是"为了个人统计再造一个 profile.html"，而不是"不能有门面页"。个人统计仍然全部在 `list.html` 概览条与 `history.html` tab2，本页**不含任何个人数据**。
>
> **落地页的"实时数字"只用匿名可读的端点**：`GET /api/problems`（取 `total`）与 `GET /api/tags` 都是 `Auth=否`（`D-43`）；`GET /api/stats` 需要登录（1002），落地页恰恰是未登录访客先看到的那一屏，**不能用它**。取不到就整行隐藏，**不显示 0** —— 0 会被读成"题库里一道题都没有"。

#### 顶栏的两套形态 `[D-52]`

顶栏是**同一个组件**（`common.js` 的 `renderTopbar()`），按登录态条件渲染，**不做两套模板**：

```
未登录：
┌────────────────────────────────────────────────────┐
│  CppOJ      题库                     [登录] [注册]  │
└────────────────────────────────────────────────────┘

已登录（普通用户）：
┌────────────────────────────────────────────────────┐
│  CppOJ   题库   我的              alice   登出     │
└────────────────────────────────────────────────────┘

已登录（管理员）[D-41]：
┌────────────────────────────────────────────────────┐
│  CppOJ   题库   我的   运维      alice [管理员] 登出│
└────────────────────────────────────────────────────┘
```

- **普通用户不渲染任何角色徽标**（不显示"普通用户"字样）—— 那是一条对每个用户都无信息量的标签
- 顶栏的登录态由 `auth/me` 的返回决定：**1002 → 未登录形态**，其他 → 已登录形态
- `login.html` 页**不渲染顶栏**（否则会同时出现"登录"按钮和"登出"按钮）
- 题目页的顶栏压缩为单行（§7.2 线框），但**用的是同一个组件**，不做第二个实现

#### 首页概览条 `[D-38]`

`list.html` 顶部一条**横向卡片区**（未登录时显示引导，见 §7.4），数据全部来自 `GET /api/me/overview`：

```
┌──────────────────────────────────────────────────────────────────────────┐
│ 你好，alice  [管理员]           已通过 18/50      尝试中 7   收藏 12      │
│                                                                          │
│  简单  ████████░░░░  8/14    中等  ████░░░░░░░░  7/25    困难  ██░░░░░░░░ 3/11 │
│                                                                          │
│  最近提交  10:24 两数之和 · 通过 ✓    10:02 反转链表 · 答案错误 ✗   ...   │
└──────────────────────────────────────────────────────────────────────────┘
```

- 「你好，{用户名} + 角色徽标」即 `[D-41]` 的顶栏角色显示，同一组件，不重复实现
- 三个迷你进度条 = `byDifficulty`，宽度 = `solved/total`；`total=0` 的难度不渲染该条
- 最近 3 条 = `recent`，每条可点击跳 `/problem.html?slug=xxx`
- 整条概览条**不随分页/筛选变化**（`overview` 独立于 `problems` 查询），避免翻页时闪烁
- 概览条与其下题目表格之间留 24px 间距，用浅色背景卡片与白底表格区隔

> **为什么概览条放首页而不是放「我的」页独占**：个人统计是**打开就想看**的信息，而「我的」是**主动去找**的信息。备战工具的打开动机是"我今天刷什么"，概览条回答"我已经刷了多少、还剩多少"恰好是这个动机的第一问。而"掌握分布"这种需要停下来细看的数据，放进「我的」tab2 更合适 —— **常看与细看分离**。


> **`admin.html` 严格受限**：只读，两个区块（`system` 状态卡 + `users[]` 表格），没有任何表单提交或按钮写操作。角色调整**不在此页**，只经 `.env` + 重启（§6.3.1）—— 刻意避免"env 声明式"与"页面写"两条路径互相覆盖。详见 §6.3.3。

> **角色对前端的影响范围** `[D-37] [D-41]`：仅两处 —— ① 顶栏在 `auth/me` 返回 `role==='admin'` 时渲染「管理员」徽标 + 「运维」入口，普通用户两者都不渲染（**后端仍必须独立校验** `/api/admin/overview`，前端隐藏只是体验，不是安全边界）；② `admin.html` 本身。`login.html` / `list.html` / `problem.html` / `history.html` 的既有交互**完全不受影响**。
> `OJ_ALLOW_REGISTER=0` 时 `login.html` 的注册 tab 仍显示，用户提交后收到 1010 的明确报错 —— 接受这个体验，不加"探测注册是否开放"的额外端点。
> 顶栏是各页共用组件（§6.1 的 `common.js`），角色徽标与 admin 入口用 `role` 做条件渲染即可，不需给各页加特判。**首页概览条的"你好，{用户名} [角色]"复用顶栏同一段渲染逻辑**，不另写一份。

#### 「我的」页三 tab `[D-39]`

```
┌──────────────────────────────────────────────────────────────────────┐
│ 顶栏:  Logo   题库   我的   [运维]      alice [管理员]  登出         │
├──────────────────────────────────────────────────────────────────────┤
│  [ 提交记录 ]  [ 掌握分布 ]  [ 账户设置 ]        ← tab，hash 定位       │
├──────────────────────────────────────────────────────────────────────┤
│  tab1 提交记录（#records）— 现有内容，不变                            │
│    筛选: 题目 ▾ | 结论 ▾ [全部/AC/WA/TLE/MLE/RE/CE]     分页         │
│    表格: 时间 | 题目 | 结论 | 用例 | 耗时 | 内存 | 展开                │
│                                                                          │
│  tab2 掌握分布（#progress）                                             │
│    ┌─────────── 难度 ───────────┐  ┌────────── 标签 ──────────┐      │
│    │ 简单  8/14   57%  ████░░    │  │ 标签      已过  总数  通过率 │      │
│    │ 中等  7/25   28%  ██░░░░    │  │ 数组       12   18    67%   │      │
│    │ 困难  3/11   27%  █░░░░░    │  │ 哈希表      9   12    75%   │      │
│    └────────────────────────────┘  │ 动态规划     4   10    40%   │      │
│                                     └────────────────────────────┘      │
│    标签表按 total 倒序；只渲染 category=algorithm，                     │
│    source 类标签折叠在「展开更多」里                                    │
│                                                                          │
│  tab3 账户设置（#account）                                              │
│    用户名   alice                    (只读，不可改 — §6.3.5 要点 6)   │
│    角色     管理员                   (只读 badge，来自 auth/me)      │
│    ─────────────────────────────────────────────────────────────      │
│    修改密码  旧密码 [______]  新密码 [______]  [确认修改]              │
│              提示: 修改后其他设备的登录将失效                          │
│    ─────────────────────────────────────────────────────────────      │
│    [ 登出当前账号 ]                                                   │
└──────────────────────────────────────────────────────────────────────┘
```

- **tab 状态用 hash**（`#records` / `#progress` / `#account`），因此可直接分享链接到某个 tab，刷新后停留在原 tab
- 三个 tab **共用一次** `GET /api/me/overview` 的结果：切 tab 不重新请求（前端内存缓存，页面生命周期内有效）
- tab1 保持与 v1.2 完全一致的提交记录 UI，不引入任何变化
- tab3 改密成功后：toast 提示 + **静默重拉 `auth/me`**（§6.3.5 要点 5），表单清空

> **为什么合并而不是新建 profile.html**：`byTag`/`byDifficulty`/提交记录三者**同源**（都从 `submissions` 聚合），拆成两页意味着同一份数据在两个页面重复请求、且两处口径迟早会漂移。而"账户设置"只有两个控件，独立成页的边际收益低于它带来的导航复杂度。**代价是页面职责变宽** —— 用 hash 分 tab 缓解，这是本项目一贯的"多页 + 页内分区"取向（与题目页的左右分栏同源）。

### 7.2 题目详情页布局 `[D-26]`

```
┌─────────────────────────────────────────────────────────────────┐
│ ← 返回   两数之和  [中等]  ⭐收藏          提交记录 | 题面 | 提示  │  ← 顶栏(压缩单行)
├──────────────────────────┬──────────────────────────────────────┤
│                          │  [ C++ ▾ ]  [重置]  [提交 Ctrl+↵]    │  ← 工具栏
│  Markdown 题面            ├──────────────────────────────────────┤
│  (marked + DOMPurify      │                                      │
│   渲染, 无 img)            │        CodeMirror 5                  │
│                          │        C++ 语法高亮                   │
│  ### 题目描述             │        行号 / Tab 缩进                │
│  ### 示例 1               │        (localStorage 自动保存)        │
│      输入: ...            │                                      │
│      输出: ...            │                                      │
│  ### 约束                 │                                      │
│  ### 提示 [D-47]         │                                      │
│    (点击展开, 免费无限)   │                                      │
│    [D-48] 树/图题用字符画  │                                      │
│                          │                                      │
│  ── 你的最近提交 [D-42] ─│  ⣾ 评测中…  前面还有 1 个评测         │
│    ☑ 10:24  通过   3ms    │        (每 1.5s 轮询, sessionStorage) │
│    ☐ 10:02  答案错误      │                                      │
│  ┌─── diff (勾选两条展开) ┌┴─────────────────────────────────────┐ │
│  │ −  vector<int> two(…) { │ +  vector<int> twoSum(...) {       │ │
│  │ +  for (i…) if (…)      │ +      if (mp.count(t-n)) {       │ │
│  │ [回填到编辑器]           │ [回填到编辑器]                      │ │
│  └─────────────────────────┴────────────────────────────────────┘ │
│  ── 下一题建议 [D-59] ──                                      │
│  反转链表（简单）  理由: 链表 标签你通过 2/5    [去做这题 →]      │
├──────────────────────────┴──────────────────────────────────────┤
│ 结果面板（可折叠，占详情页下半屏）                                  │
│  状态: ❌ 答案错误   通过 2/3   总耗时 7ms   峰值RSS 2.0MB [D-63]  │
│  ┌──┬────────┬──────────┬──────────┬─────┬──────────┐            │
│  │# │ 输入    │ 你的输出  │ 期望输出  │耗时 │峰值RSS   │            │
│  ├──┼────────┼──────────┼──────────┼─────┼──────────┤            │
│  │0 │[2,7..] │[0,1]     │[0,1]     │ 3ms │2.0MB     │            │
│  │1 │[3,2..] │[1,0]     │[1,2]     │ 2ms │2.0MB     │ ❌          │
│  │2 │[3,3..] │ 超出时间限制 (3000ms)              │ ❌          │
│  └──┴────────┴──────────┴──────────┴─────┴──────────┘            │
│  ⓘ 限制口径: 地址空间 128MB ｜ 列为实测峰值RSS，仅供参考 [D-63]       │
└─────────────────────────────────────────────────────────────────┘
```

**结果面板的三种形态** `[D-65]` `[D-66]`：

| verdict | 状态行 | 逐用例表 | 附加区块 |
|---|---|---|---|
| AC/WA/TLE/MLE/RE | 「通过 X/Y · 总耗时 · 峰值RSS」 | **渲染** | 口径说明（`ⓘ` 脚注） |
| **CE** | 「❌ 编译错误 · **Y 个用例未执行**」 | **不渲染**（`caseResults` 为空） | 编译错误面板（§5.6，含 `harness` 归属文案） |
| **JE** | 「⚠️ 平台错误」 | **不渲染** | 原始 `compiler_output` / judge 日志指引 + 「重试」按钮 |

> **内存列头写「峰值RSS」而不是「内存」** `[D-63]`：限制是 AS 口径、度量是 RSS 口径（§5.5.1），两者不是同一个量。若都写「内存」，用户看到"报 MLE 但表里只显示 30MB"会以为是 bug。**把口径差异写在界面上，是"保留严格值"这个决策能成立的前提。**
>
> **CE 不渲染用例表** `[D-65]`：因为一条用例都没跑，没有可渲染的行。状态行里的「Y 个用例未执行」由 `case_total = GREATEST(0, problems.case_count)` 得出（§5.6.3）—— 这正是修正 `case_total` 语义的实际收益。

左右分栏比例可拖拽调节（`mousedown` 改 `flex-basis`，比例存 localStorage）。**diff 面板与建议卡都在左栏**（题面侧），因为它们都属于"复盘/导航"信息，与题面同属阅读流；结果面板占下半屏全宽，因为逐用例表格需要横向空间。

### 7.3 关键交互

| 交互 | 实现 |
|---|---|
| 草稿保存 `[D-27]` | CodeMirror `change` 事件 debounce 800ms → `localStorage["draft:"+slug]`。切题/刷新/关闭浏览器均保留；登录不涉及 |
| 提交 `[D-28]` | `Ctrl/Cmd+Enter` 或点按钮；提交前校验：trim 后非空、≤64KB，否则前端提示不发请求 |
| 轮询 `[D-10]` | 提交后每 1.5s `GET /api/submissions/{id}`；`status` 变 `done`/`failed` 时停止并渲染结果。**组件卸载时清除定时器**；页面停留超过 5 分钟无结果则提示"评测耗时较长，可稍后在历史页查看" |
| CodeMirror 设置 | `mode: text/x-c++src`, `lineNumbers: true`, `indentUnit: 4`, `tabSize: 4`, `indentWithTabs: false`, `matchBrackets: true`, `viewportMargin: 30` |
| 编译错误高亮 | `compileErrors` 中 **且 `file_name='user.cpp'`** 的诊断 → `cm.addLineClass(line, "background", "cm-error-line")`；悬停显示 message。**`file_name='harness'` 或 `'link'` 的诊断不高亮**，改显示专属文案 `[D-64]`（见 §5.6） |
| Markdown 渲染 | `marked.parse()` 后经 `DOMPurify.sanitize()`，白名单允许 `pre/code/h1~h6/table/ul/ol/li/strong/em/a/blockquote` —— **`img` 已移除** `[D-48]`（题面禁图片，字符画走 `pre`） |
| 字符画对齐 `[D-48]` | 树/图/矩阵题用 `pre` 代码块内的等宽字符画；`pre` 样式设 `overflow-x: auto` + `white-space: pre`（窄屏横向滚动而不换行，换行会破坏图形） |
| 时间显示 `[D-17]` | 接口返回 UTC ISO8601；前端 `new Date()` 转本地，先显示相对时间（"刚刚"/"3 分钟前"/"昨天 15:24"），`title` 属性放完整本地时间 |
| 401 处理 | 任意接口返回 1002 → 全局跳转 `/login.html?redirect=<当前路径>` |
| 运维入口 `[D-37]` | `common.js` 启动时调 `auth/me`，`role==='admin'` 时在顶栏渲染「运维」链接指向 `/admin.html`；否则不渲染。**后端 `/api/admin/overview` 独立校验 role**，前端隐藏不作为安全边界 |
| 角色徽标 `[D-41]` | 与运维入口同一段 `auth/me` 渲染逻辑：`role==='admin'` 时在用户名右侧渲染 `管理员` 徽标（accent 色底 + 12px 字号），普通用户**不渲染任何徽标**（不显示"普通用户"字样 —— 那会让每个用户都看到一条无信息量的标签）。首页概览条复用这段逻辑 |
| 首页概览条 `[D-38]` | `list.html` 挂载时调 `GET /api/me/overview`；三个迷你进度条宽度 = `solved/total * 100%`，`total=0` 的难度整条不渲染；`recent` 每条渲染为「时间 + 题名 + 结论徽标」，点击跳 `/problem.html?slug=`。**不随筛选/分页重取**（分页只重发 `GET /api/problems`） |
| 三 tab 切换 `[D-39]` | `history.html` 读 `location.hash` 决定激活 tab；点 tab 改 hash（触发 `hashchange`，不重新请求）。`overview` 结果缓存在模块级变量，**页面生命周期内只请求一次**，切 tab 与 tab1 分页都不重取 |
| 改密码 `[D-40]` | tab3 表单：`type="password"` 两个输入框 + 提交。校验：非空、6–64 位、两次一致（不一致时前端拦下并标红，不发请求）。请求体 `{oldPassword, newPassword}`。1001 时按 `data.field` 把错误文案挂到对应输入框下方；成功则 toast + 清空表单 + **静默重拉 `auth/me`** |
| 题目提示 `[D-47]` | `hint_md` 为空/缺省 → **不渲染提示 tab**；非空 → 渲染，点击展开，**免费无限看、无次数限制、无解锁流程**。`hint_md` 与 `statement_md` 走**同一条** marked + DOMPurify 渲染管线 |
| 提交 diff `[D-42] [D-50]` | 题目页「你的最近提交」每行前置 checkbox，最多选 2 条。选满 2 条即调 `GET /api/submissions/diff` 并在上方展开并排双栏；LCS 行级 diff 在前端算（`diff.js` 独立模块）。每栏底部有「回填到编辑器」按钮，**会覆盖当前草稿**，点击前弹确认 |
| 下一题建议 `[D-59]` | 题目页底部卡片，数据取自题目详情响应的 `nextRecommendation`。显示题名 + 难度 + **理由字符串**（服务端算好的可解释文案）+ 跳转按钮。全部通过时替换为完成度文案；**未登录时不渲染整张卡** |
| 删除提交 `[D-49]` | 历史页每行「删除」按钮 → 确认弹窗（"这条提交记录将被永久删除，含 N 个用例结果"）→ `DELETE` → 移除该行并把 `total` 减一。**题面页的最近提交列表同步移除** |
| 重跑提交 `[D-49] [D-58]` | 仅当该条 `canRetry=true`（`status='failed'`）时渲染「重跑」按钮 → `POST .../retry` → 用返回的 `newSubmissionId` 启动轮询。**原记录保持 `failed` 态不变**，历史里多出一条 `pending` |
| 队列位置 `[D-54] [D-60]` | 提交返回的 `queueAhead`：为 0 时状态区只显示「评测中…」；`n>0` 时显示「前面还有 n 个评测」。**每秒递增一次**（轮询时用上一次的 `queueAhead - 1` 推算，不额外请求） |
| 刷新恢复轮询 `[D-53]` | 提交成功后 `sessionStorage["judging:"+slug] = submissionId`；`problem.html` 挂载时若该键存在且对应提交 `status ∈ {pending,judging}` → 自动重建轮询与状态区；终态后删除该键。**用 sessionStorage 而非 localStorage** —— 评测态是 per-tab 的，不该在另一个标签页也转圈 |
| 草稿冲突检测 `[D-55]` | 草稿写入时同存 `localStorage["draftAt:"+slug] = Date.now()`。页面挂载时：若 `draftAt` 晚于本页编辑器已加载草稿的时间 → 弹窗「此代码已在另一个标签页更新，是否载入？」→ 用户选后才替换，**不自动覆盖** |
| 会话过期恢复草稿 `[D-44]` | 401 时跳转 `/login.html?redirect=<题目路径>&expired=1`。重新登录后跳回该题，`problem.html` 检测 `expired=1` → 若 `localStorage` 有该题草稿则自动载入并提示「已恢复你上次未完成的代码」 |
| 顶栏两套形态 `[D-52]` | `common.js` 启动调 `auth/me`；1002 → 渲染「登录/注册」形态；200 → 渲染「我的/运维/用户名/角色徽标/登出」形态。`login.html` 不挂顶栏。题目页用同一组件的 compact 变体 |
| 注销账号 `[D-56] [D-57]` | tab3 底部「注销账号」→ 弹窗要求**输密码** → 二次确认弹窗**逐项列出**待删数据量（提交 N 条 / 收藏 M 题 / 会话 K 个）+ 红字「不可恢复」→ 确认 → `DELETE /api/auth/account` → **清空 localStorage 中所有 `draft:*` 与 `draftAt:*`** + 清 Cookie → 跳 `/login.html?deleted=1` |

### 7.4 空态与错误态

| 场景 | 表现 |
|---|---|
| 题库列表无结果 | "没有匹配的题目，试试放宽筛选条件" + 清除筛选按钮 |
| 无提交历史 | "还没有提交记录" + 去刷题按钮 |
| 未登录访问需鉴权页 | 跳转登录页并带 redirect |
| 提交后 1.5s 轮询遇 503 | 弹 toast"判题服务繁忙，评测可能延迟"，继续轮询 |
| 网络错误 | toast + 3 次指数退避重试 |
| **未登录访问首页** `[D-38]` | `GET /api/me/overview` 返回 1002 → 概览条**不隐藏**，改为渲染「登录后查看你的刷题进度」+ 登录按钮，**题库列表照常渲染**（`GET /api/problems` 的 `Auth=否`）。不隐藏的理由：空着会让首页显得残缺，用户也无从知道有这功能 |
| **新用户零数据** `[D-38]` | `solved=0` 时概览条三个进度条渲染为空槽（0%），「最近提交」区显示「还没有提交记录，去做第一题 →」；tab2 的难度卡显示 `0/0` 而非空白 |
| **`byTag` 为空** `[D-39]` | 题库未挂任何标签时 tab2 标签区显示「题库暂无标签分类」，不渲染空表头 |
| 改密旧密码错 `[D-40]` | 旧密码输入框标红 + 「旧密码不正确」，`newPassword` 框保持已填值不清空（便于只改旧密码后重试） |
| **未登录点提交** `[D-43]` | 提交按钮文案变为「登录后提交」；点击**不跳转**，而是弹登录框（内联小表单）→ 登录成功后自动跳回当前题并恢复提交意图（`sessionStorage` 暂存代码）。**不做**"点了就跳走"—— 正在写的代码会丢 |
| **未登录点收藏** `[D-43]` | 星标渲染为描边态；点击弹登录框，登录后**不自动补收藏**（避免隐式写操作），只跳回该题 |
| **未登录看个人内容** `[D-43]` | 「我的」入口在未登录形态下**不渲染**（顶栏条件渲染，`D-52`）；直连 `history.html` → 跳登录并带 redirect |
| **diff 只选了一条** `[D-42]` | 提示「再选一条提交以对比」，不发起请求（省一次无意义的往返） |
| **回填覆盖草稿** `[D-42]` | 「回填到编辑器」点击时弹确认「当前编辑器内容将被替换，草稿会被覆盖」→ 确认则写入 CodeMirror 并**立即触发草稿保存**（debounce 800ms 后落 localStorage） |
| **重跑一条进行中的提交** `[D-58]` | 按钮不渲染（`canRetry=false`）；若绕过前端直接调接口 → 1011 |
| **删除别人的提交** `[D-49]` | 1003；**admin 也不例外**（§6.3.6 ②，AC-20 立场不变） |
| **注销时草稿残留** `[D-57]` | 注销成功后遍历 `localStorage` 删净所有 `draft:*` / `draftAt:*`（草稿键**不含用户名**，不清理会留给下一个登录的人） |
| **全部题目已通过** `[D-59]` | 建议卡位置改为「🎉 题库 N/N 全部通过」+ 「想扩充题库？编辑 `problems/*.json` 后重启服务」（呼应 §6.3.4 声明式题库） |
| **编译错误指向 harness** `[D-64]` | 显示「⚠️ 这不是你的代码出错…该题配置有误」+ 原始 g++ 输出，**编辑器不高亮任何行**，并提供「复制错误详情」按钮（便于用户反馈给管理员） |
| **链接错误** `[D-64]` | 显示签名核对清单（类名/方法名/参数类型/返回类型四项）+ 原始链接器输出，不提供行高亮 |
| **平台错误 JE** `[D-66]` | 状态区显示「⚠️ 平台错误，本次评测未能完成」+ 「重试」按钮（复用 §6.3.6 的 retry）+ 「这通常是系统问题，不是你的代码问题」 |
| **内存口径脚注** `[D-63]` | 逐用例表下方常驻一行 `ⓘ 限制口径: 地址空间 128MB ｜ 列为实测峰值RSS，仅供参考`。**不折叠**（用户第一次看到"报 MLE 但 RSS 才 30MB"时必须能立刻找到解释） |

---

## 8. 风险与降级方案

| ID | 风险 | 影响 | 概率 | 降级方案（触发即执行） |
|---|---|---|---|---|
| **R-1** | **fork+setrlimit 无法防逃逸**，恶意用户可读同机文件 | 严重（仅当开放给不可信用户） | 低（定位个人单机） | 文档显式声明；若需开放 → 替换为 Docker 隔离，runner 层接口不变，改动局限于 `runner.cpp` |
| **R-2** | **代码生成器类型覆盖不足**（链表/树/图/自定义结构） | 阻塞题目录入 | **高**（自评最大风险） | ① 限定支持类型白名单：`int/long long/double/bool/string/vector/vector2d`；② 超出白名单一律用 `__expr` 逃生舱 + `harness_prelude`；③ 两者都不够时该题改走 stdin/stdout 范式（保留扩展位） |
| **R-3** | **g++ 输出格式解析不稳定** | 编译错误无法定位到行 | 中 | 原始输出全文始终展示；解析失败降级为纯文本面板，不影响判题正确性 |
| **R-4** | **judge 资源泄漏**（僵尸进程/残留目录/磁盘） | 长期运行后机器劣化 | 中 | ① `prctl(PR_SET_PDEATHSIG, SIGKILL)` 绑定 worker；② `waitpid` 必调；③ 工作目录 finally 块清理；④ 启动时扫描清理 `/tmp/oj-judge/*`；⑤ 预留启动时的 `/tmp` 空间检查 |
| **R-5** | **题库录入量不足**（50 道题工时超预期） | 演示效果差 | 中 | 里程碑 M3 先录 20 道覆盖高频题型（数组/双指针/链表/树/动态规划/二分），M5 视进度补到 50 |
| **R-6** | **并发下 DB 锁竞争** | 列表页变慢 | 低 | 全局互斥量已串行化；必要时对 `problems` 表加查询缓存（TTL 60s，题库启动时同步后基本不变） |
| **R-7** **已消除** | ~~CDN 依赖不可达（CodeMirror/marked 加载失败）~~ | ~~编辑器不可用~~ | ~~中~~ | **v1.6 已消除** `[D-73]`：三库 vendored 到 `static/vendor/`，**运行时不再有外网依赖**。`editor.js` 的 textarea 降级路径因此降为**防御性冗余，不强制实现**（保留是因为它仍能兜住"文件被误删"） |
| **R-8** | **MySQL 首次初始化失败**（initdb 脚本仅首次执行） | 容器起来但无表 | 中 | 服务启动时检测表是否存在，不存在则**幂等执行** schema（`CREATE TABLE IF NOT EXISTS`）作为兜底 |
| **R-9** | **严格资源限制误杀**（LeetCode 同级限制在本机偏紧） | 用户频繁 TLE | 中 | 题目 JSON 逐题可配 `time_limit_ms`/`mem_limit_kb`，默认取 LeetCode 同级值，录入时按需放宽 |
| **R-10** | **v1.4 新增 6 个端点 + 1 列 DDL 落在原定 M3/M4 窗口内**，挤占题库录入时间 | 进度落后 | **中高** | 见 §8.1 的 v1.4 专项降级 |
| **R-11** | **diff 的 LCS 在极端输入下 O(n·m) 内存爆炸** | 页面卡死 | 低 | 64KB 上限（§5.8）已封顶；再加「行数 > 2000 则降级为整体替换显示」的硬闸门（§1.4 的 100ms 约束） |
| **R-12** | **注销的 CASCADE 链被后续改动破坏**（某张子表误改为 RESTRICT） | 注销静默失败或部分删除 | 低 | AC-33 专门验证级联完整性；所有 DDL 外键统一 `ON DELETE CASCADE` 作为团队约定 |
| **R-13** | **`queueAhead` 的 FIFO 等价关系被打破**（队列改优先级后无人察觉） | 位置显示错误但不报错 | 低 | 依赖 FIFO 这条契约在 §6.3.6 显式标注为「需在代码里留注释的隐含契约」；AC-30 验证 FIFO 下取值正确 |
| **R-14** | **CPU 停滞检测的观测窗口过短**，进程在 500ms 间隔内恰好没被调度（如机器 8 并发满载）→ CPU 增量 0 被误判为阻塞 | 并发评测时误判 TLE | **中** | `STALL_POLLS=3 × 500ms = 1.5s` 窗口已远大于调度延迟；且**必须在连续 3 次都为 0 时才触发**（单次为 0 不触发）。AC-38 专门压测"8 并发 + 机器满载"下的误判率 |
| **R-15** **已修复** | ~~墙钟保险阀与并发闸门矛盾~~ | — | — | **v1.5 已修**：`D-62` 改为 CPU 停滞检测，见 §5.4.1 的推演 |
| **R-16** **已修复** | ~~`RLIMIT_AS` 与 `ru_maxrss` 口径混用~~ | — | — | **v1.5 已修**：`D-63` 保留严格值 + 界面标注口径，见 §5.5.1 |
| **R-17** **已修复** | ~~平台生成文件的编译错误无法归属~~ | — | — | **v1.5 已修**：`D-64` 归属分流 + §4.2.1 生成期校验，见 §5.6 |
| **R-18** **已修复** | ~~CE 的 `case_total=0`~~ | — | — | **v1.5 已修**：`D-65` 改 `GREATEST` 口径，见 §5.6.3 |
| **R-19** **已修复** | ~~RE 与 JE 无法区分~~ | — | — | **v1.5 已修**：`D-66` 0 条 `@@OJ@@` → JE，见 §5.6.2 |
| **R-20** | **自实现 bcrypt 写错**（base64 变体字母表顺序是最易错点） | 密码校验全线失败或全被绕过 | 中 | ① 实现前先跑 RFC 6234 官方测试向量（AC-49）；② 一次注册后**立刻**验证登录，不把"注册成功"当成"哈希正确" |

#### 8.1.1 v1.5 审计的四个硬漏洞（已全部修复）

审计方法是"**假设它会出错，把两个数字代进去算一遍**"。四个漏洞的共同特征是**两处各自合理的设计放在一起就矛盾**，逐条读文档都看不出来：

| # | 漏洞 | 矛盾的两处 | 后果 | 修复 |
|---|---|---|---|---|
| 1 | 墙钟保险阀 | §5.4 `×2+1s` ↔ §1.4 并发 8 × 2 核 | 墙钟放大 8 倍 > 阈值 2 倍 → **高 CPU 合法解法被系统性误杀 TLE** | `D-62` CPU 停滞检测 |
| 2 | 内存口径 | §5.4 `RLIMIT_AS` ↔ §5.5 `ru_maxrss` | 报 MLE 但 RSS 只 30MB → **界面自相矛盾**；AS 128MB 比 LeetCode 严得多 | `D-63` 保留值 + 标注 |
| 3 | 报错归属 | §5.2 编译 `tester.cpp` ↔ §5.6 只归一化 `user.cpp` | 题库类型错误指向用户改不了的文件 → **用户无法自救** | `D-64` 归属分流 + 生成期校验 |
| 4 | 用例数语义 | §3.4 `case_total=COUNT(明细行)` ↔ CE 无明细行 | 编译错误却显示「通过 0/0」 | `D-65` `GREATEST` 口径 |

> **教训值得记进 AR**：数字之间、章节之间的**交叉一致性**比单章节的正确性更难保证，因为没有任何一次阅读会同时把两个数字放在手边算。下次改并发数或资源限额时，**必须重算 §5.4.1 与 §5.4.2 的乘数关系**。

### 8.1 v1.4 专项降级 `[D-42] [D-49] [D-59]`

v1.4 一次性加了 20 项需求，风险 R-10 是真实的。**降级顺序按「增量成本」排，不按重要性排**（与 §8 既有的 admin 降级同一原则）：

```
若 M4 结束时代码生成器或判题引擎尚未验收（进度落后 > 3 天）
  → 第一刀：砍 diff（D-42/D-50）。它需要一个独立前端模块 + 并排布局 + 回填交互，
           成本最高；而「展开看代码」的原功能（v1.2 已有）已能覆盖基本复盘。
           砍后：历史页仍可逐条展开代码全文，闭环不受影响。
  → 第二刀：砍「下一题建议」卡（D-45/D-59）。规则本身很轻，但需要新的候选集查询
           + 理由文案拼接；砍后：题面页底部区块直接不渲染。
  → 第三刀：砍注销与删除（D-49/D-56/D-57）。属于"用户自主权"而非核心闭环。
  → 永不砍的四项（成本极低或直接补已有功能的坑）：
       · 提示 hint_md（D-47）—— 一个字段 + 一个折叠块
       · 队列位置 queueAhead（D-54/D-60）—— 一条 SQL + 一个 span
       · 刷新恢复轮询（D-53）—— sessionStorage 读写
       · 草稿冲突检测（D-55）—— 两个时间戳比较 + 一个 confirm
           ↑ 这三项都是"已有功能在异常下的正确行为"，砍掉等于留 bug
  → 端点层同步降级（同 T4.14 的"先砍页面后砍端点"原则）
```

> **为什么把「注销与删除」排在 diff 与推荐之后**：它们是**用户自主权**类功能（我想删/我想注销），属于锦上添花；diff 与推荐则是**复盘效率**类功能，直接影响备战体验的顺畅度。两者都不是 §1.2 闭环链路上的环节，所以砍掉都不影响成功标准。

### 降级触发规则

```
若 M2 结束时 R-2 未达标（生成器仅支持白名单类型）
  → 冻结功能范围，题库录入只用白名单类型 + __expr，不再扩展生成器
若 M3 结束时题库 < 20 道
  → 砍掉 M4 的历史页筛选功能，优先保证题目录入完成
若整体进度落后 > 3 天
  → 砍：收藏功能、题内最近提交、来源标签筛选（保留核心闭环）
若整体进度落后 > 3 天 且 M4 的 T4.14 尚未开工
  → 砍：admin.html 页面（T4.14）。**保留 `GET /api/admin/overview` 端点** ——
    砍页面后管理能力用 curl 仍完全可用，只是没有 UI。顺序必须是"先砍页面、后砍端点"，
    因为端点是能力的本体，页面只是它的展示层。
若整体进度落后 > 4 天（个人页功能 `[D-38~D-41]` 的降级顺序）
  → ① 先砍 tab2 掌握分布（T4.16）与「我的」页改名 —— 后者只是导航文案，tab1 本就是原提交历史
  → ② 再砍 tab3 账户设置（T4.17），改密回退为非目标
  → ③ 保留首页概览条（T4.15）与角色徽标（T4.18）—— 二者共用 `auth/me`，
    增量成本极低，且概览条是"打开就想看"的信息，砍掉等于首页失去个性
  → ④ 无论砍到哪一步，**`GET /api/me/overview` 端点都保留**（同 T4.14 的理由：端点是能力的本体）
若整体进度落后 > 3 天（v1.4 新增功能 `[D-42~D-61]` 的降级顺序，详见 §8.1）
  → ① 砍 diff → ② 砍下一题建议 → ③ 砍注销与删除
  → 永不砍：hint_md / queueAhead / 刷新恢复轮询 / 草稿冲突检测（成本极低或直接补已有功能的坑）
```

> **个人页的降级顺序是按「增量成本」排的，不是按「重要性」排** `[D-38~D-41]`。tab2 掌握分布虽然最有信息量，但它是唯一需要新写聚合 UI（难度卡 + 标签表 + 通过率条）的一块，成本最高；概览条只画三个进度条，且角色徽标直接复用已有的 `auth/me` 渲染分支。因此吃紧时先砍最贵的。**这与 §1.2「功能闭环可用」的成功标准一致** —— 概览条与徽标都不在闭环链路上，砍掉不影响 §1.2 的验收，而掌握分布砍掉也只是少一个可视化入口。

> **admin 功能的降级顺序是刻意安排的** `[D-37]`。角色同步（T3.11）与管理端点（T3.12）属于**能力**，成本低（1 个端点 + 1 个守卫 + 1 段启动同步 SQL），且补的是"无法指定管理员"这个真实缺口；`admin.html`（T4.14）属于**展示**，成本相对高（一个页面 + 前端条件渲染）。因此进度吃紧时先砍页面 —— 丢失的只是可视化，能力与数据一条不少。**这与 §1.2「管理后台仍是非目标」的立场一致：admin 从来不是交付重点，只是别把权限做成一列没人读的死代码。**

---

## 9. TODO 清单

### M0 — 地基（Day 1–2）

- [√] T0.0 安装依赖 `[D-71] [D-72] [D-73] [D-75]`：执行 **§6.7.2 的全部命令** + **§6.7.4 的 8 项验证**
  - **验证必须全绿才继续**，尤其 fork+setrlimit+prctl+wait4 那一项 —— 它失败意味着 §5 全部机制不可用
  - 初始化本机 `mysqld`，**把 `innodb_buffer_pool_size` 设为 64MB**（1967MB 机器上不调会与 judge 抢内存，见 §2.1-A 第 3 条）
  - 装 **不要** `pkg-config` / `build-essential` / `libcpp-httplib-dev`（§6.7.1 的「明确不装」行，理由见该节）；**`cmake` 改为必装**（v1.8，见下）
  - **httplib 装 0.60.1 单头文件到 `/usr/local/include/`**（v1.9），不要用 apt 的 0.14.3
- [√] T0.1 目录骨架 + **构建入口** `[D-71]`
  - `make build` / `make run` / `make test` / `make clean`（契约不变）
  - `make run` 负责拉起 `mysqld_safe`（若未运行）+ 启动 `build/oj`
  - **v1.8：`Makefile` 降为薄封装**，真实构建规则在 `CMakeLists.txt`（`ojcore` 静态库 + `oj` 可执行 + 测试开关）与 `tests/CMakeLists.txt`（GTest + `gtest_discover_tests`）
  - `make test` == `ctest --test-dir build --output-on-failure`，每个 `TEST_F` 是 ctest 里一条独立用例
  - **编译并发固定 `-j2`、sanitizer 构建 `-j1`**（§0.0「开发机硬约束」）：`Makefile` 里 `JOBS ?= 2`，传 `JOBS=1` 可临时覆盖。**不要**改回 `-j` / `-j$(nproc)` —— 2G 内存下 sanitizer 并发会表现为"构建卡死"
  - ctest 的 `WORKING_DIRECTORY` 固定为**仓库根**（`tests/CMakeLists.txt`）：本项目的 `./problems`、`.env`、`./logs` 都是相对当前工作目录的约定，ctest 默认用 `build/tests`，会让"手动跑通过、ctest 跑失败"
- [√] T0.1.1 **CMake 构建规则** `[v1.8]`
  - 依赖全部 `find_package` / `find_path` 发现：Threads、OpenSSL(libcrypto)、nlohmann_json、mysqlclient、httplib、GTest
  - **httplib 双路径**：`/usr/local/include/httplib.h`（**0.60.1，版本真源**，v1.9）优先，缺失则 `WARNING` + 回退 `third_party/httplib`（0.14.3，版本不一致，视为环境未就绪），两者都没有才 `FATAL_ERROR`
  - **源文件用显式列表而非 GLOB**：漏加文件时报错，而不是静默不编译
  - `gtest_discover_tests` 固定 `PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`（超时防挂死；工作目录与 `make run` 对齐）
  - **不在 CMake 里写死并发**（`CMAKE_BUILD_PARALLEL_LEVEL` 留给调用方）：并发上限是**机器属性**而非项目属性，写进构建系统就换台机器得改代码。约束落在 `Makefile` 的 `JOBS` 与 §0.0 的约定上
  - `docker-compose.yml` 与 `Dockerfile` **标为路线 B 可选**，本阶段不实现
- [ ] T0.2 【路线 B 可选】`Dockerfile` + `docker-compose.yml`：`ubuntu:22.04` 装 `g++-12`、MySQL Connector、cmake
  - **不在关键路径上** —— 若 T0.1 的构建入口跑通，本任务可延后到 M5 或直接砍掉
  - 路线 B 里构建同样走 `cmake -S . -B build`（v1.8 后 Docker 镜像不必自带 make）
  - ⚠️ **镜像必须自带 httplib 0.60.1**（v1.9）—— 宿主用的就是这份，不放进去就正是 `[D-72]` 想消除的版本漂移
- [√] T0.3 `db/init/01-schema.sql`（§3.2 全量 DDL，**仍 11 张表**，注意外键依赖顺序：`problems`/`users`/`tags` 须先建；含 v1.4 新增的 `problems.hint_md`）
  - 路线 A：应用启动时**幂等执行**这份 SQL（§2.1-A 第 1 条），不用 initdb 机制
  - **已接线**：`main()` 按 §2.3 启动顺序在连接池就绪之后执行 —— 读文件 → `split_statements()` 切分 → `exec_script()` 逐条执行 → **核对 11 张表确实存在于当前库**（`information_schema`）。DDL 文件本身早于本轮就已存在且已用 `IF NOT EXISTS`，本轮补的是**执行侧**
  - **三道「响亮地失败」**：DDL 文件读不到 / 切不出任何 `CREATE TABLE` / 切出条数与实际执行条数不等 / 表没真的建出来 —— 任一命中都打印人可读原因并 `exit(1)`，而不是留一张半套的库让 T1.2 去炸
  - 表名**从 DDL 文本里扫出来**而非写死 11 个：写死就多出一份必须手工同步的副本，漂移无声。切分复用 T3.1 的 `db::split_statements()`（同一套去注释规则）—— 若自己扫原文，本文件头部注释里出现的 `CREATE TABLE` 字样会被误当成一条建表语句（**本轮实际踩到过**）
  - 空库 → 11 张表、二次启动幂等，均已实测；`db/reset.sql` 保持不动（它不能进 `init/`，否则首启建完表又删掉）
- [√] T0.4 `common/`：`config.h`（环境变量读取）、`log.h`（双文件按天切割 logger）、`response.h`（响应包络）、`errors.h`（错误码枚举）
  - `config.h` 新增两个变量 `[D-37]`：`OJ_ALLOW_REGISTER`（默认 1，0 = 关闭注册端点）、`OJ_ADMIN_USERNAMES`（逗号分隔；**须区分「未设置」与「空串」**，见 §6.3.1 约束 1）
  - 配 `.env` 文件并在 README（T5.6）中说明这两个变量的作用
  - **已完成 `config.h`/`.env` 部分**：`tests/config_test.cpp` 45 条单测全绿
  - **已完成 `log.h` 部分**：`tests/log_test.cpp` 42 条单测全绿，覆盖 `[D-34]` 双文件隔离、按天切割（含改名失败降级）、保留 N 天清理（只删本模块形状的文件）、行不变量与 UTF-8 安全截断、11 个并发写者；`-DOJ_SANITIZE=address,undefined` 下同样全绿（本机 LSan/TSan 受限，见 CMakeLists 注释）
    - **修掉两处跨天边界缺陷**（均为单测可判别的确定性问题，不靠实跑撞运气）：
      ① **秒与毫秒必须来自同一次时钟读取**：早先是 `now_epoch_seconds()` 与 `now_millis()` 两次独立读时钟，秒边界上会拼出 `23:59:59.007` 这种"秒取自 T、毫秒取自 T+1"的行 —— 症状是每晚至多一行带着前一天的日期戳却写进当天的文件。现在统一为 `now_wall()` 一次 `clock_gettime` 返回 `{seconds, millis}`，且**行前缀与切割判据共用这同一个值**（`entry → write_line → rotate_if_needed` 一路传下去），否则仍会出现"行时间戳与所在文件日期不一致"
      ② **保留期清理必须认带序号的归档名**：切割时同名冲突会追加序号（`app.log.2026-10-12.1`），而解析器只接受长度 10 的后缀，于是这些文件被判成"不是本模块的切割文件"而**永远留着**，日志目录只涨不消。现按 `<日期>` 与 `<日期>.<数字>` 两种形状解析（序号必须至少一位且全为数字，`app.log.<日期>.` 与 `.x` 仍不动）；另把"非法"哨兵从 `0` 改为 `-1`，因为 `0` 是 1970-01-01 这个合法日期的天序号，用它当哨兵会让那个日期的过期文件逃过清理
      - 判别力已实测：把两处修复各自退回后，`TimestampSecondsAndMillisComeFromOneClockRead` / `LineStampAndItsFileDateNeverDisagreeAcrossMidnight` / `RetentionAlsoDeletesSequenceSuffixedRotatedFiles` 三条立即变红
      - 为此给 `log_test_set_time()` 加了可选的 `millis` 参数：早先覆盖时钟时毫秒被写死 0，等于把"两次读取"这件事藏了起来，真实时钟下它只在秒边界那一瞬发生（每晚一行），不给出可冻结的毫秒值就无从单测
    - **回归**：`ctest --test-dir build` **320/320 通过**；`-DOJ_SANITIZE=address,undefined` 下同样 **320/320 通过**（构建 `-j1`）
  - **`errors.h` 已完成**（T3.3 轮补上：§6.3 的 20 个码 + `http_status()` + `message()`，`2xxx` 段一律映射 200）
  - **`response.h` 已完成**（本轮补上，T0.4 的最后一块）：`include/common/response.h` + `src/common/response.cpp`，四个入口 `ok()` / `created()` / `fail()` ×2 / `fail_with_data()`，包络一律由 `nlohmann::json` dump 而非手工拼串
    - **四个入口而不是一个带一堆默认参数的**：失败的两条路（只有码 / 带 `data.field`+`data.reason`）在 §6.3 里是两种**不同的响应体**，用默认参数合并会让"忘了传 field"产出一个 `"field":""` 的半截数据，而前端按 `data.field` 决定把红字挂到哪个输入框下 —— 空串会让它两处都不挂又都像挂了。故 `field` 为空时**退化成只带码的形态**
    - **`created()` 单列**：§6.3 端点表写死注册成功是 **201**，而 `errors.h` 的 `kOk` 是 200。不给 `http_status` 加"看端点决定"的参数 —— 那会把"哪个端点返回 201"的决定权散到全部端点里。现状只有注册一个调用方，加第二个时该重新想一次
    - **`data` 永远是对象**：成功无内容时给 `{}`、失败无字段时给 `{}`，绝不省略。前端 `api.js` 因此可以无条件读 `data.field`，不必在每个调用点写 `data ?? {}` —— 那个兜底一旦散到十几处就会有一处漏
    - **`Content-Type` 由 `write_body()` 无条件覆盖**：静态文件 404 走 httplib 的 error handler，那条路径上响应里可能已带着 `text/html`，不覆盖就会让"按 content-type 分流的工具"把一段 JSON 误判
    - **HTTP 状态码一律转调 `errors.h::http_status`，本文件不存第二份映射表** —— `[D-29]` 要求两者同步，两份表迟早对不上一处，而这种漂移不会崩，只会让 curl 拿到的状态码与业务码矛盾
    - **⚠️ 本轮新写单测时立刻查出两处真问题（都不是"测试没过"，是"代码与注释/契约不符"）**：
      ① **键序注释是错的**：注释写"顺序即输出顺序（nlohmann 默认按插入序）"，实测字节序是 `code → data → message`（`json` 用 `std::map` 存对象，输出按**字典序**）。`tests/response_test.cpp` 的 `SuccessUsesTheExactBytesOfSection63` 逐字节断言把它抓了出来。**没有改用 `ordered_json` 也不手工拼串** —— 那要么换掉全项目共用的 json 类型，要么把转义责任接回包络层（本文件用 nlohmann 正是为了不做后者）
      ② **`fail(err, "", reason)` 会把 reason 静默丢掉**（真 bug）：早先的耦合写法是"field 为空就整体退化成 `fail(err)`"，而 `routes.cpp` 的 **413 分支恰好走这条路径**（field 空 + reason = 「请求体超出允许的大小」）——**那句文案从未真正到达客户端**。`send_failure()` 把 `AuthFailure` 的两个字段原样递进来，而服务端原因（DB 报错摘要）**只填 reason 不填 field**，所以这不是理论问题。改为**两个键各自可选**，两个都空才退化成 `fail(err)`
    - **单测** `tests/response_test.cpp` **17 条**（纯计算、无 DB、不碰网络，故**连不上 mysqld 的机器上照跑不误**）：包络逐字节形状 / `created()`=201 与 `ok()`=200 的差异 / 11 个失败码的 HTTP 状态逐行核对 §6.3 / **两条退化分支**（field 空→不产出该键但保留 reason；reason 空→不产出该键）/ `data` 传 array/null/string/number **恒为对象**的不变量 / reason 含 `"` `\` 换行 `<script>` 时输出仍是合法 JSON / 三个契约键不多不少 / 20 个码表逐行 / **2xxx 段一律 200**（`[D-29]`）
  - **本条勾选**：四个文件（config / log / errors / response）全部落地且各自有单测或 curl 级验收；README 说明（T5.6）尚未写，随 T5.6 一并补
- [√] T0.5 httplib 起服务，静态文件映射跑通 `list.html`
  - **已完成 `include/web/routes.h` + `src/web/routes.cpp`**：httplib 0.60.1（`[D-72]` v1.9 真源）起服务、静态挂载、§6.3 **认证组四端点**接线（`register`/`login`/`logout`/`me`）
  - **验收按本条原文勾选的实测**：`make run` 后 `curl -i localhost:8080/login.html` → **200**（3841 字节）、`/css/app.css` `/js/api.js` `/js/auth.js` `/js/common.js` 均 200。⚠️ **`list.html` 本身仍是 0 字节（T4.3 未开工）**，故本条的"跑通 `list.html`"只兑现了"静态文件映射链路通了"，页面内容待 T4.3；**登录成功后跳 `/list.html` 会看到 1004 包络，这是 T4.3 的欠账，不是路由缺陷**
  - **`main.cpp` 已推进到 §2.3 第 8 步**（第 9~10 步角色同步 / judge 仍未接线），且**本进程自此常驻阻塞** —— 在此之前 `make run` 跑完就退出，那正是本条验收的前提
  - **`SIGINT`/`SIGTERM` 两个信号接到 `web::stop_server()`**，listen 被打断后按「log → 连接池 shutdown → 日志 shutdown」的正常顺序收尾。信号处理器里**只调这一个函数、不写日志不加锁**（`std::signal` 的平台实现决定了加锁会在处理器里死锁）；实测 `Ctrl-C` 后日志里依次出现「收到停止信号」与「MySQL 连接池已关闭」，退出码 0
  - **线程池写死 8 而不是读 `config().http_threads`**：`CPPHTTPLIB_THREAD_POOL_COUNT` 是 httplib.h 的**编译期宏**，而 `http_threads` 是运行期配置。两者恰好都是 8（§2.3 的 N=8、config 默认值）但**不是同一件事**，宏改的是每个 HTTP 线程 8MB 的**虚拟**栈；把不同步这件事写在 routes.h 里而不是留成隐含假设（§0.0 提醒过别过量申请虚拟内存：8×8MB=64MB 虚拟、不是常驻）
  - **两处收口只有这一处**（`set_error_handler` + `set_exception_handler`），但**收口的判据是「body 为空」而不是「status == 404」**：
    - httplib 对**任何 `status >= 400`** 的响应都会调 error handler，**包括我们自己的业务错误**。按"凡是 4xx 就重写包络"写，会把每一个 1002 未登录 / 1005 用户名已注册改写成 1004 —— 症状是"未登录时 `auth/me` 永远返回资源不存在"，前端连"要不要跳登录页"都判断不了。这个坑**实测踩到过**：首版按 status 判，`GET /api/auth/me` 实测返回 `404 {"code":1004}` 而非 `401 {"code":1002}`
    - httplib 自己造的 404/400/413 有一个共同且唯一的特征：**body 从未被填过**。于是判据成了"body 为空 ⇒ 这不是我写的响应，交给收口；否则返回 `Unhandled` 原样放过"
  - **不做「未知路径 → index.html」的重定向**：`/` 已被静态挂载占住且 httplib 对不存在的静态文件返回 404、不回落任何首页。五页前端靠**显式文件名链接**（顶栏「题库」→ `/list.html`、登录成功也跳 `/list.html`）而不是那条常见规则 —— 它会让 `/api` 的拼写错误也返回 200 + HTML，curl 验收时看不出端点不存在，错的反而更难查
  - **【先 send 后 set_header】是硬顺序**：登录的 `Set-Cookie` 在包络写完之后追加。一旦有人给 send 加上"统一清头"的行为，Cookie 会被抹掉，而症状是"登录返回 200 但永远未登录"，极难查
  - **分层与既有三条业务层一致**：本文件不含任何 SQL / 校验规则 / bcrypt，只做「HTTP 语法 → 业务层调用 → 包络」。四道闸门在 `auth_api.h`、会话签发回收在 `session.h`、包络在 `response.h`，故这三层各自的可测性互不影响
  - **curl 验收实测**（`curl -s -o /dev/null -w '%{http_code}'`）：未登录 `auth/me` → **401**（包络 `code=1002`）/ 重复用户名注册 → **409**（`1005`）/ 弱密码注册 → **400**（`1001`）/ 错密码登录 → **400**（`1001`）/ 登录 → **200** + `Set-Cookie: sid=<64 hex>; HttpOnly; SameSite=Lax; Path=/`（逐字符合 `[D-69]`）/ 带 Cookie 的 `auth/me` → **200** / 登出 → **200** / 登出后用旧 Cookie → **401** / 未知端点 → **404**（`1004`）/ 请求体非 JSON → **400**。**AC-1 的四条（1005 / 1001 / Cookie 维持会话 / 登出后 1002）全部达成**
  - **仍待接线**（本条已勾选但不是全做完）：§6.3 其余 16 个端点（T3.6~T3.18）逐个**追加**到同一个 `register_routes()`；~~`purify.min.js` 仍是 43 字节的下载失败占位文件~~ —— **已在 T4.6 修复**（换成 DOMPurify 3.1.6 真库）
- [ ] T0.6 **vendored 前端库接入** `[D-73]`
  - 5 个 CodeMirror 文件按 **`defer` + 依赖顺序**引入（见 §6.1 末尾的加载顺序说明）
  - 页面内断言 `typeof CodeMirror !== 'undefined' && CodeMirror.modes.clike` —— **C++ 模式没加载时要立刻报错**，不能等到用户发现没高亮
  - 三个 MIT LICENSE 放进 `static/vendor/LICENSES/`
  - **【新增】vendored 文件的落盘后校验**（本次故障的直接对策，§6.7.2 的下载清单里逐条执行）：
    ```bash
    # ① 体积下限：低于阈值的必定是错误页/占位文件（purify 的那次就是 43B）
    # ② 首行特征：应有该库的 license/版本头
    # ③ 语法可解析：node --check
    # ④ 关键符号存在：grep 到 UMD 的全局挂载
    ```
    只看 `curl` 的退出码不够 —— **cdnjs 对不存在的文件有时仍返回 200 + 一段提示文本**
  - **【已实施】静态资源一律 `Cache-Control: no-cache`**（`src/web/routes.cpp` 的 `set_mount_point("/", static_dir, {{"Cache-Control","no-cache"}})`）：
    - **为什么必须做**：原生 ES Module 改一行 `.js` 就要动所有引用它的页面，浏览器一旦缓存住旧模块，报错会指向**没改过的那个文件**，症状是 `The requested module './api.js' does not provide an export named 'xxx'`（实测踩到：新加的 `invalidateCurrentUser` 已经在磁盘上，浏览器仍在执行缓存里的旧 `api.js`）或 `Unexpected identifier 'to' at purify.min.js:1:8`（缓存里躺着那个 43 字节占位文件）。排查成本极高而根因一行响应头可根除
    - **为什么是 no-cache 而不是 max-age=3600**：no-cache 的语义是"每次先向服务器确认"，httplib 的静态处理器会比对 `If-None-Match`/`If-Modified-Since`，未修改时回 **304 空响应**（实测确认），流量几乎不变，但改动一定立刻可见
    - **不选 no-store 的理由**：no-store 会连协商都省掉，每次都重传 500KB 的 ace.js；单机单人使用下这点流量无所谓，但 304 是白拿的，没理由不要
  - **【已实施】静态资源 URL 带版本号 `?v=20261015-1`**（HTML 的 `src`/`href` + **JS 模块之间的相对导入**）：
    - **为什么 `no-cache` 之外还要版本号**：浏览器里可能还存着**加 `no-cache` 之前**取到的响应。那批响应没有 `Cache-Control`，Chrome 按 `Last-Modified` 的 10% 推算出**启发式新鲜期**（实测那几个文件算出来约 9 小时），期间 F5 根本不会向服务器发请求 —— 实测症状就是控制台仍打印 `Unexpected identifier 'to' at purify.min.js:1:8` 与 `does not provide an export named 'invalidateCurrentUser'`，而 `curl` 取到的是正确内容
    - **版本号必须加在【模块图内部】，不只是 HTML 上**：ES 模块的相对导入 `'./api.js'` 会解析成**不带父级查询串**的 `/js/api.js`，只给 HTML 加版本号的话，`problem.js` 照样会 import 到那个被缓存的旧 `api.js`，第二个报错原样复现
    - **【改前端时必须 bump 版本号】**，一条命令覆盖全部位置：
      ```bash
      # 把 20261015-1 换成新的日期串；HTML 与 js/ 两处都要改
      grep -rl '20261015-1' static | xargs sed -i 's/20261015-1/<新版本号>/g'
      ```
      漏改的后果不是报错而是**"改了没生效"**，比报错更难发现
  - **已落盘 6 / 7 个文件**：`static/vendor/cm/` 下 5 个 CodeMirror（核心 174KB + clike 模式 + 3 个 addon）与 `marked.min.js`（35KB），实测 `codemirror.min.js` 里 `clike` 出现 **0** 次（印证 §6.2 尾注"核心不含语言模式"，因此 5 个文件缺一不可）
  - ~~**`purify.min.js` 只有 43 字节**，内容是 `Failed to fetch version info for DOMPurify.`~~ —— **已修复（2026-10-15，T4.6）**：换成 `dompurify@3.1.6` 的 `purify.min.js`（21496 B，官方 license 头 + `globalThis.DOMPurify` 挂载实测确认）。**根因是包名写错**：cdnjs 上的库名是 `dompurify` 而非 `purify`，按错名字下载会拿到一个 43 字节的"占位"文件而 **HTTP 仍是 200**
    - **这次的故障形态值得单独记**：缺库 ⇒ `DOMPurify is not defined` ⇒ `renderMarkdown` 抛错 ⇒ `renderHead` 中断 ⇒ **题面、三个 tab、编辑器、提交按钮全部不出现**，页面只剩标题与难度条。控制台里只有一行 ReferenceError，界面上只弹一条 toast（`ReferenceError: DOMPurify is not defined`），而真正的原因（vendor 里躺着一个占位文件）要 `ls -l` 才看得到
    - **两条防复发措施**（已落地）：① `markdown.js` 开头 `assertDeps()` 显式判 `typeof marked / DOMPurify`，缺库时抛出**说人话**的报错（指明是哪个文件、可能是占位文件）；② `problem.js` 的 `renderHead` 把渲染两段 Markdown **单独 try/catch**，失败只在该区块显示一条 notice 并 toast，**页面其余部分照常初始化** —— 一个区块的依赖问题不该让整页瘫掉
    - **通用教训（比这次修复更重要）**：**vendored 文件的"存在"不等于"可用"**。43 字节的垃圾文件同样能通过"文件存在"的检查与 `curl -o` 的 0 退出码。T0.6 当时的下载步骤需要一个**落盘后校验**（见下方新增项），而不是只看 curl 是否成功
  - **`LICENSES/` 目录尚未创建**（三个 MIT LICENSE）
  - **HTML 已不再是"全部 0 字节"**：`login.html` 随 T4.2 落地（它**不需要** CodeMirror/marked/purify，故对 T0.6 无依赖）；`list/problem/history/admin.html` 仍是 0 字节。**本条的真正工作量在 T4.1~T4.5 —— 这是 T4 的前置依赖，不是 T0.6 单独能做完的事**：题目页要同时引用那 5 个 CodeMirror 文件，`purify.min.js` 在换成真库前 §7.3 的 XSS 过滤也不能算数
- [ ] **验收**：`make run` 后浏览器访问 `localhost:8080` 看到页面；`./logs/app.log` 正常输出；**编辑器有 C++ 高亮**（目视 + 上面那条断言）
  - **前两项已达成**（随 T0.5 落地）：浏览器打开 `localhost:8080/login.html` 看到登录页；`./logs/app.log` 逐请求输出访问行（`POST /api/auth/login → 400` 这种）与启动回显
  - **第三项仍不成立**：题目页（`problem.html`）还是 0 字节，CodeMirror 那 5 个文件还没有任何页面引用 —— 这一项与 T4.1~T4.5 同批落地

### M1 — 题库与生成器（Day 3–6）★ 最高风险

- [√] T1.1 `nlohmann/json` 引入 + `problems/two-sum.json` 首个样例题
  - **已完成**：`nlohmann_json`（3.11.3）由根 `CMakeLists.txt` 的 `find_package` 发现并链进 `ojcore`，首个使用者是 `src/problem/json_schema.cpp`；`problems/two-sum.json` 已落盘并被加载器实际读入（启动实测"扫描 1 个文件，入库 1 题"）
- [√] T1.2 `problem/loader.h`：读目录 → 校验必填字段 → 同步 MySQL（幂等，按 slug）
  - **已完成 `include/problem/loader.h` + `src/problem/loader.cpp`（563 行）与 `problem/json_schema.h/.cpp`（647 行）**，六项要求逐条落地：
  - **校验分两层** `D-64`：① 必填字段（slug/signature/method_name/params/cases/return_type 非空）；② **类型一致性静态校验**（§5.6.1 的 7 条）。第 ② 层失败 → 跳过该题 + `ERROR` 点名，**不阻塞其他题**（层 ① 与层 ② 都在 json_schema，写库之前跑完）
    - **不 fail-fast，一次收齐全部问题**：录题的人一次只想改一处；只报第一个会变成"改→重启→看到下一个错"，50 题里的一道可能重启十几次（§4.3 的工作流就是重启驱动的）。代价是每个检查点都要能在已有错误的前提下继续跑，已写进该模块的头注释
    - 另有三条**本轮定的、SPEC 没单列**的：顶层未知字段（抄写/拼错的唯一可观测信号）、**题面禁图片** `D-48`、`slug` 与文件主名不一致（URL 与题库对不上）
  - `cases[]` / `params[]` 数组按 §4.1 映射表**展开为多行**写入 `problem_cases` / `problem_params`
  - 同步用 delete-then-insert 单事务语义，保证 JSON 删用例能传播（不留幽灵行）—— `db::Transaction` 守卫，失败即整题回滚
  - 同步完成后同事务回填 `problems.case_count`
  - 全部入库后灌入进程内只读缓存（`cache()` / `find_by_slug()` / `find_by_id()`；缓存是一份 `shared_ptr<const ProblemBundle>` **快照**，运行期只读，故不需要任何锁）
  - **题库后来被改错时不清库**：把旧行标记失效（清空用例 + `case_count=0`），**不删 `problems` 行** —— 用户的提交历史靠外键挂在这行上，删题会连带删掉历史（§3.1 的取舍）
  - **已接线**：`main()` 的 §2.3 第 5 步。实测输出：`题库同步 完成（./problems）：扫描 1 个文件，入库 1 题（用例 2 / 形参 2 / 标签关联 3），跳过 0 题`，随后 `判题缓存已就绪：1 题`
  - **单测** `tests/problem_loader_test.cpp` **17 条** + `tests/json_schema_test.cpp` **37 条** 全绿（真库组连不上 MySQL 时整体 `GTEST_SKIP`）；本模块共 54 条，占全量 345 条执行用例的 15%
  - `LoadOptions.write_to_db = false` 提供"只解析校验 + 灌缓存、不碰库"的模式，给题库自检与单测用
- [ ] T1.3 `judge/generator.h` 核心：JSON 值 → C++ 字面量转换器（含字符串转义）
- [ ] T1.4 类型推导：从 `problem_params.cpp_type` 推导数组嵌套层数
- [ ] T1.5 生成 `solution_decl.h` / `tester_util.h` / `tester.cpp`
- [ ] T1.6 `__expr` 逃生舱 + `problems.harness_prelude` 支持
- [ ] T1.7 单元测试：`tests/gen_test.cpp` 覆盖 int/string/vector/vector2d/嵌套/转义/空数组
- [ ] **验收**：给定 two-sum.json，生成的 tester.cpp 手写答案后能编译通过并输出 `@@OJ@@` 协议行
- [ ] **验收**：改 JSON 删掉第 3 个用例后重启，`problem_cases` 只剩 2 行且 `case_count=2`（验证同步语义）

### M2 — 判题引擎（Day 7–10）

- [√] T2.1 `judge/compiler.h`：`alarm` 超时 + 两阶段编译 + 输出捕获
  - **已完成 `include/judge/compiler.h` + `src/judge/compiler.cpp`**：`compile_submission()` 依次跑 §5.2 的两条命令（`g++-12 -std=c++17 -O2 -Wall -c user.cpp -o user.o` → `g++-12 -std=c++17 -O2 user.o tester.cpp -o run`），任一阶段失败**立即返回**不做第二阶段；返回体是 `{ok, stage, timed_out, exit_code, output, diagnostics, commands, user_object_created, binary_created}`
  - **spawn 与回收只有一个函数 `run_command()`**，两阶段只差 argv：超时/截断/进程组 kill 的逻辑写两遍就会在不同时期各自演化 —— 那正是 §8.1.1 说的"两处各自合理的设计放在一起就矛盾"的形状
  - **超时 = `alarm()` + 墙钟双判，两者都走 `kill(-pgid, SIGKILL)`**。handler 只置 `volatile sig_atomic_t` 标志、不做系统调用（信号处理函数里调 `kill`/`waitpid` 都非异步信号安全，且出错只会更难查）；墙钟是 alarm 的兜底（alarm 粒度是 1 秒）。ESRCH（子进程还没来得及 `setpgid`）时退回 `kill(pid)`，否则会漏下真正的孤儿进程
  - **输出捕获三条硬规则**：① stdout 与 stderr 合并到同一根管道（顺序即原序，省掉事后按时间戳归并）；② **有上限地读**（默认 256KB，超限即截断 + 杀进程组 + 在末尾留一行显式标记 —— 截断必须可见，否则用户看到一段"看起来完整"的输出却少了尾巴）；③ 原始输出**永远保留**（§5.6 最后一句的兜底）
  - **子进程环境只改三样，每样都写明理由**：`LC_ALL=C`（正则关键词是英文的，工具链若输出本地化关键词会静默变成"零条诊断"）、`LANG=C`、`MALLOC_ARENA_MAX=1`（glibc 每个 arena 在 64 位上预留 64MB 地址空间，1967MB 机器上这一步能显著压掉 cc1plus 的虚拟占用）；stdin 接 `/dev/null`
  - **阶段二刻意不加 `-Wall`**：那些 warning 全来自 harness 自己（生成代码的未用参数、有符号/无符号比较），会灌进用户看到的输出却与用户代码无关
  - **本模块自带一把进程级【编译闸门】，容量默认 1**（§0.0 开发机实测）。⚠️ 它与 §2.3 的"全局评测闸门 4"**不是同一个东西**：评测闸门限的是受 `RLIMIT_AS 128MB` 约束的轻量子进程，编译闸门限的是**编译器自身**（不受限、重）。闸门的第二个作用是让 SIGALRM 可用 —— `alarm()` 是进程级的，多线程各设一个会互相覆盖。容量调大前必须重算本机内存
  - **`stage=kNone` 单独成一路**：工作区缺 `user.cpp` 时返回 `kNone`（而不是 `kUserObject`），让调用点能判 **JE 而非 CE** —— 那是"上游没准备好文件"，§5.6.2 判定表里没有这一行
  - **本模块只做观测、不判 verdict**：§5.6.2 的第 5 行（0 条 `@@OJ@@` → JE）不是编译阶段的事，把 verdict 放进本模块会诱使调用点"拿到 `CompileResult` 就直接填 verdict"，那正好绕过第 5 行
  - **单测** `tests/compiler_test.cpp` **20 条全绿**（14 条纯解析 + 4 条真跑 g++ + 超时 + 闸门串行化）。真跑只留覆盖四种结局的最少用例 —— §0.0 的机器上每多一条真编译就多一次峰值内存，而**容易写错的是解析**（归属分流错时不崩，只会静默把 `tester.cpp` 的错误当成用户代码的行号），因此解析用穷举字符串覆盖
  - **两条超时/串行化用例用"假编译器脚本"而不是"很难编译的源文件"**：后者在 2 核机器上要吃真实内存与数秒 CPU，且"多久算久"依赖机器 —— 用例就只在慢机器上成立。脚本走的是同一段 spawn / 回收 / kill 路径
  - 尚未接线：`worker` 调用本模块属 T2.7，工作目录创建/清理属 T2.9，`@@OJ@@` 之外的三文件生成属 T1.5
- [ ] T2.2 g++ 输出正则解析 → `submission_compile_errors` 行（`seq` 保序），路径归一化
  - **归属分流** `D-64`：`user.cpp` → 正常高亮；`tester.cpp`/`solution_decl.h`/`tester_util.h` → `file_name='harness'` + 专属文案，**不提供行号**；链接器 `undefined reference` → `file_name='link'` + 签名核对清单
  - ⚠️ 阶段二（链接 `tester.cpp`）的报错**不代表用户代码有错**，是题库类型错误（§5.6）
  - **已完成解析与分流**（同 T2.1 的 `compiler.cpp`：`parse_compile_output()` 纯函数，可穷举单测）
    - **归属判据只有一条：文件名**。用户代码在工作区里只有 `user.cpp` 一个，其余一切路径（生成文件、**系统头**、库）都归 harness —— 因此 g++ 回显相对路径还是绝对路径都得同一结论（判据不依赖路径前缀）
    - 命中不了 `file:line:col` 的行（`In function ...`、源码摘录、`^~~~`、`collect2` 摘要）**不进诊断表**，原样留在 `output`；`seq` 从 1 起保序
    - **`link` 诊断的 `line`/`col` 必须是 0**：填一个假的行号会让编辑器去标红一行无关代码（AC-43 要求 `compileErrors` 不含行号项）。它**追加在末尾**而非插在命中位置 —— 它与行号诊断不是同一类
    - 只认 `undefined reference|undefined symbol` 两个关键词（§5.6 原文），**刻意不**把 `ld returned 1 exit status` 算进来：它只是摘要，把它算进来会让"链接器因别的原因失败"也被贴上"你的签名不一致"这张标签 —— **一个不准确的归因比没有归因更糟**
    - 正则分组编号写在代码注释里（1=文件 2=行 3=列 4=等级 5=正文）：错位时 severity 会静默变成 `error`，而这不会崩
    - 行号转成 `unsigned int` 用手写钳位而非 `std::stoul`：后者溢出时抛异常，而这是 worker 的判定路径 —— **一条诊断不该有能力把评测线程抛下去**
  - **本条仍不勾选**：解析产出的是结构体，落 `submission_compile_errors` 表的那一步归 **T2.11（写回事务）**
- [√] T2.1.1 **题库加载期静态校验** `D-64`：按 §5.6.1 的 7 条校验；不通过的题**跳过并 `ERROR` 点名**，不阻塞整库启动；启动日志汇总所有被跳过的题
  - **已完成，就落在 T1.2 的层 ②**（`src/problem/json_schema.cpp` 的 `parse_problem_json`），故与 T1.2 同批交付、同一批单测。7 条的落点：第 1~4 条在 `parse_cases()` 里按 `params[].cpp_type` 的 vector 层数**逐用例**核对（`expected` 深度 / 元素类型 / `args` 数量 == `params` 数量 / `args[i]` 深度）；第 5 条 `method_name` 必须是 `signature` 里的**独立标识符**（`signature_declares_method()`，只做子串匹配会把 `twoSum2` 误判为声明了 `twoSum`）；第 6 条非空在层 ①；第 7 条用了 `__expr` 就必须有非空 `harness_prelude`
  - **失败处理按 SPEC 原文**：整题跳过 + `ERROR` 点名。加载器据此把库里旧行标记失效（用例清空 + `case_count=0`），**不删 `problems` 行**（用户的提交历史挂在这行上）
  - **问题的种类是分类的**（`IssueKind`）：`kMissing` / `kType` / `kValue` / `kConsistency` / `kImage` …… 这样"【值域超长】"与"【类型不一致】"在 50 道题的启动日志里一眼可辨，而不是混成一句"校验失败"
  - **单测** `tests/json_schema_test.cpp` 37 条覆盖 7 条各自的正反例（深度差一层 / 元素不是数字 / `args` 比 `params` 多一个 / 方法名不在 signature / 有 `__expr` 无 prelude …）
  - **与 §5.6 的关系没变**：静态校验挡常见错误，**归属分流（`harness` / `link`）必须保留** —— `__expr` 里是任意 C++ 表达式，静态校验挡不住（§5.6.1 末段）
- [ ] T2.3 `judge/runner.h`：fork + `setpgid` + setrlimit 五项 + stdin 重定向
- [ ] T2.4 `wait4` 回收 + **CPU 停滞检测** `D-62` + 进程组 kill + 退出码→verdict 映射
  - 判据是「**连续 3 次、间隔 500ms，CPU 时间增量为 0**」，**不是**「墙钟超过 N 倍」—— v1.4 的墙钟算法在 8 并发下会误杀合法解法（§5.4.1 有推演）
  - 兜底墙钟放宽到 `time_limit_ms × 10 + 5000`（默认 35s），对 8 倍放大留 1.46 倍余量
  - **两条 kill 路径都必须 `kill(-pgid, SIGKILL)`**，不能只杀直接子进程
  - ⚠️ 在代码注释里写明「`× 10` 与全局并发闸门 8 绑定，调高闸门必须同步调高」——与 §6.3.6 的 FIFO 契约同类
- [ ] T2.4.1 **verdict 完备判定表** `D-66`：按 §5.6.2 的 11 行实现，**重点是第 5 条「0 条 `@@OJ@@` → `JE`」**
  - 这是 RE 与 JE 的唯一区分依据，**必须有单测覆盖**（AC-39）
- [ ] T2.4.2 **内存口径标注透传** `D-63`：把「限制=AS 口径 / 度量=RSS 口径」两个值分别下发给前端，不得合并成一个字段
- [ ] T2.5 `@@OJ@@` stderr 协议解析 → `submission_case_results` 行
- [ ] T2.5.1 **CE 时 `case_total` 修正** `D-65`：写回时 `case_total = GREATEST(COUNT(明细行), problems.case_count)`，且**读该列的查询须 JOIN `problems`**
  - 断言表达式（§3.4 v1.5 版）与写回**必须用同一表达式**，否则一致性校验全线报错
- [ ] T2.5.2 **生成期类型一致性校验** `D-64`：按 §4.2.1 的 5 条校验；失败则不产出文件、本次判 `JE`（**不判 CE** —— 那是题库问题不是用户代码问题）
- [ ] T2.6 结果比较器：trim、数组空白归一、浮点容差
- [ ] T2.7 `judge/queue.h` + `scheduler.h`：内存 FIFO + 4 worker 条件变量 + 并发闸门
- [ ] T2.8 启动恢复逻辑（§5.7 的两条 SQL）
- [√] T2.9 工作目录生命周期：创建/清理/启动时扫残
  - **已完成 `include/judge/workspace.h` + `src/judge/workspace.cpp`**：`workspace_paths()`（拼路径，纯函数）/ `create_workspace()` / `remove_workspace()` / `sweep_workspace()` 四个入口，文件名常量取自 `judge/compiler.h` 不另抄一份
  - **已接线**：`main()` 把它排在题库同步之后、judge 调度器之前（§5.2 末句"服务启动时先扫描 `/tmp/oj-judge/*` 清空残留"），失败只 `WARN` 不拒绝启动 —— 清不掉垃圾目录不影响判题，而"起不来"比"留个目录"更糟。实测：造 `999`/`1000`/`manual-thing` 三个残留，重启后删掉前两个、`manual-thing` 只点名保留
  - **三条不可协商的删除纪律**（每条都有对应用例）：① **只删 `<root>/<纯数字>`**，形状不符的条目只记名不删 —— §5.2 的"残留"指的是我们自己留下的，对别的形状动手删掉的可能是别人正在用的东西，且不可恢复；② **递归删除用 `nftw(FTW_DEPTH|FTW_PHYS)` + `unlink`，绝不跟随符号链接** —— 否则工作区里一个指向 `/etc` 的链接会把 `/etc` 当子树清空，这是递归删目录最经典的事故形状而平时测不出来；③ **形状校验用字符串比较而非 `realpath`** —— `realpath` 会把链接解析成真路径，于是 `/tmp/oj-judge/42 -> /etc` 反而"通过校验"，恰好是它要防的事
  - **权限用 0700**：工作区里放着用户源码与编译产物，同机其他本地用户连 `ls` 都不该做得到。注意它**不替代** §5.4 的 `setrlimit` —— 那是拦用户代码去读别人的文件，0700 是反过来保护用户自己的代码
  - **`remove_workspace` 幂等**（已不存在即返回 true）：worker 的清理在 `finally` 里调用，而"上一轮已经删过"与"这轮没建过"都不该算失败
  - **扫残没有"新旧"概念**，因此**必须在 judge 启动之前跑**：worker 正在编译时目录被删，表现为随机的 CE/JE 且极难复现。用例 `SweepHasNoNotionOfFreshness` 把这条前置条件固化成文档
  - **单测** `tests/workspace_test.cpp` **22 条全绿**（路径拼装 3 + 根目录取值 1 + 创建 5 + 删除 7 + 扫残 4 + 并发 1 + 收尾 1）。全部真跑文件系统：`mkdtemp` 出 `/tmp/ojws-XXXXXX`，`TearDown` 整棵删 —— 这批用例的价值全在删除纪律上，而那三条一旦写错，mock 掉的正是最危险的那一层（递归遍历 + `unlink`/`rmdir` 的真实语义）
    - **并发用例是必需的而非凑数**：judge worker 是多线程（§2.3 × 2），"两个线程各自建/删各自的工作区"是真实并发形态。8 线程各跑一遍创建 → 写入 → 删除，收尾再扫一次残 —— `removed` 必须是 0，因为"漏删"在生产里不是浪费磁盘，而是下一次启动扫残时会被一起清掉
    - **顺序无关性已实测**：`--gtest_shuffle --gtest_repeat=3` 三轮全绿（用例之间唯一共享的是 `root_`，每轮由 `SetUp` 重建）
    - **不碰 MySQL**，因此这批用例在没起 mysqld 的机器上照跑不误
  - **回归**：`ctest --test-dir build` **316/316 通过**（`TEST_F` 逐条注册成 ctest 用例，本模块占 24 条）；`-DOJ_SANITIZE=address,undefined` 下同样 **316/316 通过**（`ASAN_OPTIONS=detect_leaks=0`，sanitizer 构建按 §0.0 用 `-j1`）
    - 唯一跳过的是 `LoaderTest.DatabaseFailureRollsBackThatProblemAndFailsTheReport` —— MySQL 账号缺 `SUPER` 建不了触发器，**与本模块无关，且是基线既有状态**
  - 尚未接线：`worker` 的"创建 → 编译 → 清理"三步属 **T2.7**；本模块 `user.cpp` 的写入方是生成器（T1.5），它把内容写进 `create_workspace()` 返回的路径即可
- [ ] T2.10 judge 日志：`/var/log/oj/judge.log` 记录生成代码、编译命令、进程退出原因
- [ ] T2.11 写回事务：`UPDATE submissions`(四个汇总列 + status) + `INSERT` 用例行 + `INSERT` 诊断行必须同事务；失败则整条 submission 置 `failed`/`JE`
  - `status` 迁移与明细行必须同事务 —— §5.7 的恢复逻辑依赖此不变量（证明见该节注释）
- [ ] **验收**：特制题目覆盖 6 种 verdict（AC/WA/TLE/MLE/RE/CE），逐个通过

### M3 — API 层（Day 11–13）

- [ ] T3.1 `db/mysql_pool.h` + 互斥量串行化；所有 DAO 用预处理语句（**禁止字符串拼接 SQL**）
  - **已完成 `db/mysql_pool.h` + `src/db/mysql_pool.cpp`**：`tests/mysql_pool_test.cpp` **35 条单测全绿**（连不上 MySQL 时整体 `GTEST_SKIP`），`-DOJ_SANITIZE=address,undefined` 下同样全绿
  - `[D-21]` 的串行化落在**一把进程级 `std::mutex`** 上，`Lease` 是持有它的 RAII 凭证：租约内可跑多条语句，归还时按「回滚未提交事务 → 置空闲并通知 → 释放全局互斥量」的顺序（顺序反了会与等待者互锁）
  - **超时四档**（串行化 ⇒ 一条慢语句拖垮全站，故每条连接都带超时）：connect 5s / read 15s / write 15s / `innodb_lock_wait_timeout` 5s（MySQL 默认 50s，在本模型下等于停站 50s）
  - **断连只重连、不重试**：空闲时可安全重连；语句失败一律如实上抛 —— 重放一次 `INSERT` 就是写两次（T2.11 的写回尤其致命）
  - **两道本地护栏**拦在「发 SQL 之前」：占位符个数与参数个数不符直接失败；`exec()` 拒绝含 `?` 与返回结果集的语句
  - **连接建立后硬校验**三项，任何一项不满足则 `init()` 失败并给出修复命令：sql_mode 含 `STRICT_TRANS_TABLES`（否则 `VARCHAR(255)` 静默截断）/ 含 `NO_ENGINE_SUBSTITUTION`（否则 DDL 写错 ENGINE 静默退化成 MyISAM，硬删账号依赖的 CASCADE 会失效 `[D-57]`）/ 字符集 utf8mb4；会话时区锁死 `+00:00`（`[D-17]` 存 UTC）
  - **不引入新环境变量**：`PoolOptions` 由 `common::config()` 的 `MYSQL_*` 派生，其余为模块内默认值 —— 与 T0.4「新增键会让 §6.7.3 清单与 config 单测失配」的既有约定一致
  - `dao.h` 尚空，「所有 DAO 用预处理语句」要等 **T3.2**，故本条**不勾选**
  - **已接线**：`main()` 在 `load_config()` / `init_logging()` 之后调用 `init_from_config()`，失败即打印原因并 `exit(1)`；退出前 `shutdown()` 归还连接。§2.3 的启动顺序因此推进到「DB 就绪 + 建表完成」（T0.3），第 5~8 步（题库/角色/judge/listen）仍待接线
- [ ] T3.2 DAO：`user` `problem` `problem_case` `problem_param` `tag` `submission` `submission_case_result` `submission_compile_error` 八组
- [√] T3.3 认证：自实现 bcrypt（`libssl-dev` Blowfish，`D-74`）注册/校验 + 64 hex token 会话 + HttpOnly Cookie
  - **已完成自实现 bcrypt `include/web/bcrypt.h` + `src/web/bcrypt.cpp`**（约 300 行，只用 libcrypto 已有的 Blowfish 原语 / `RAND_bytes` / `CRYPTO_memcmp`，**零新增依赖**；唯一"自己抄"的是 Blowfish 初始状态那 1042 个 π 十六进制字面量）：
    - **`D-75` 的理由在本机复核过**：`libbcrypt-dev` 无 Candidate，故自实现；`BF_encrypt` 在 OpenSSL 3.0.13 上标了 deprecated，本文件用 `OPENSSL_SUPPRESS_DEPRECATED` 显式关闭告警（EVP 不暴露 P/S 状态，做不了 EksBlowfishSetup，要的就是这层低层原语）
    - **`R-20` 的三个坑全部踩到并已钉进单测**（症状都是"能跑、但与所有外部工具算出的哈希都不同"，不会崩）：① **每个 encipher 消耗【两个】盐字分别异或到 l 与 r**（不是 l/r 交替一个 —— OpenBSD 早期文献的写法会算出完全不同的哈希）；② **摘要编码 24 字节密文的【前 23 字节】**，不是"编码 24 字节再截掉最后一字符"（两者差在最后一个字符的 2 个比特上，而它就是哈希的最后一位）；③ **密钥流长度取 `strlen + 1`**（结尾 NUL 参与），且口令里的 NUL 会截断口令
    - **AC-49 已达成**：`tests/bcrypt_test.cpp` **11 条**用参考实现（本机 libxcrypt + pyca/bcrypt 3.2.2）逐字节比对了 10 条向量（空口令、单字符、`U*U`、`U*U*`、26 字母、特殊符号、非 ASCII「面试准备2026」、cost 4/5/6/10、72 字节），外加"72/80 字节同哈希""NUL 截断""随机盐不同""坏哈希不抛异常"四条不变量
    - `$2a$`/`$2b$`/`$2y$` 三变体同盐同口令**摘要相同**（前缀不同），故本项目只写 `$2b$`、但能验历史遗留行；`$2x$`（有 bug 的实现的标记）与 `$2$`（1997 老变体）**刻意拒绝**
  - **已完成注册 / 登录 / 登出 / 我是谁 的业务层 `include/web/auth_api.h` + `src/web/auth_api.cpp`**（4 条 SQL，全部预处理占位符，无字符串拼接）：
    - **四道闸门的顺序不能换**：① `OJ_ALLOW_REGISTER=0` → 1010 **且不落库**（§6.3 写的是"先查"，否则关掉注册后仍能从 1001 的长度校验反推出"注册开着"）；② 用户名 → 1001 `field=username`；③ 密码 → 1001 `field=password`；④ 判重 → 1005。**bcrypt 只在四道都过之后才做**（cost=10 约 60~100ms，放在校验前等于让纯格式错误的请求也吃掉 100ms）
    - **注册不做提权**：`role` 在 INSERT 里显式写死 `'user'`，不依赖 DDL 的 DEFAULT、也不接受请求里的 role 字段 —— 管理员身份只由 T3.11 的启动同步决定（§6.3.1）。端点层"顺手支持管理员注册"会把声明式名单变成第二个真源
    - **并发注册的 1062 也映射成 1005**（不是 1008）：判重查询挡不住两个并发请求，唯一保证是 `uk_username`，两种情况必须给用户同一个答案
    - **登录不给"用户不存在"与"密码错误"分别的文案**：两者都是 1001 `field=password` + 同一句理由，否则登录接口就成了用户名枚举器（AC-1 的 1005 只针对注册）。配套动作：**用户不存在时也跑一次 bcrypt**（对着固定假哈希）把耗时拉回同一量级 —— 假哈希用 cost=4 且固定不变（现算的话"用户不存在"反而比真实登录快 60ms，那本身又是一个信号）
    - **本轮定的、SPEC 没单列的一条**：用户名字符集限定 `[A-Za-z0-9_-]`（长度仍是 §5.8 的 3–32）。三条理由都指向 DB 现状 —— ① `users.username` 是 `utf8mb4_general_ci` **大小写不敏感**，不限定就会出现 Bob/bob 别名；② MySQL 的 PAD SPACE 比较忽略尾部空格，会出现"我明明注册了却说已存在"；③ `OJ_ADMIN_USERNAMES` 按精确字符串匹配，名字里混入不可见字符时提权会静默不生效（§6.3.1 约束 3 只 WARN 缺失名单，不 WARN"名字带了不可见字符"）
    - **登录的 bcrypt 发生在持有 DB mutex 期间**（`[D-21]` 串行化），耗时 < 200ms，§6.2.1 要点 3 已判定可接受；将来流量变大时正确做法是把"查用户"与"算 bcrypt"拆成两段租约，而不是加连接（串行化下加连接不提高吞吐）
  - **登出端点已单独收口（V2.6）** —— 业务层与路由在 T3.3/T0.5 两轮里一并落地，本轮把**登出这一条**补成可验收的完整闭环：
    - **路由层的唯一决策抽成纯函数 `web::plan_logout(cookie_header) → LogoutPlan{needs_db, token}`**（`include/web/routes.h` + `src/web/routes.cpp`），与 `error_reply_for` 同一手法。理由：登出端点里真正的判断只有一个 —— **"带没带 sid 决定要不要取租约"** —— 而它恰恰是本文件顶部那条约定（`[D-21]` 先解析 Cookie → 有 sid 才取租约）的落点。匿名登出（curl 直连 / 重复点击 / 页面回退）必然影响 0 行，却要让整个服务的 DB 串行化队列停一下；判据抽出来后这条不变量才有一个能被钉住的用例
    - **判据是「非空」而不是 `is_token_wellformed`** —— 形状非法的 sid 仍要交给 `logout_user`（删不掉就是 0 行，那也是成功）。反过来的症状是"浏览器里的坏 Cookie 永远清不掉"：清除 Cookie 那头照发，看似生效，下次带上同一个坏 sid 还是老样子。少一次无谓的 DELETE 比多留一个清不掉的 Cookie 划算
    - **`Set-Cookie` 无条件下发**：三处（`logout_user` 成功 / `needs_db` 为 false / 未生成）最终都走 `session::make_clear_cookie()` 这一份字符串，路由层不得另造
    - **新增 `tests/logout_test.cpp`（13 条）**，补的是**已有三处覆盖都没穿过的那一段组合**：`session_test` 测 `destroy(token)`（token 由用例自造）、`auth_api_test` 测 `logout_user(token)`（同样自造）、`routes_test` 只测 `error_reply_for` 且全程不起 HTTP —— 而线上真实路径是「**浏览器发来的整条 Cookie 头** → `plan_logout` → `logout_user`」。**AC-1 第三条**（登出后旧 Cookie 返回 1002）在这条路径上才有意义：只断言 `count_sessions == 0` 的话，"删了行但查询路径仍认它"这种回归会漏过去。分两段：纯决策 6 条（不碰库、永远跑）+ 真库组合 4 条（无库整体 `GTEST_SKIP`）+ DB 故障 1 条
    - **两条此前无人覆盖的不变量**：① **§6.3.2「logout 只删自己那一行」** —— 同一用户两个会话 + 一个旁观者会话，删一个后其余 2 行必须还在（掉这条的症状是"在自己电脑上点了登出，手机上也被登出"；而 `[D-40]` 的改密码才是"踢掉其他设备"，两者的差别正是这一行 SQL。已有用例都只造一个会话，故删错了也看不出来）；② **清除 Cookie 的属性串必须是签发头属性串的逐字前缀**（`session_test` 那条只逐项查存在性，查不出顺序被调换或属性被加了一节）
  - **已完成 `include/common/errors.h` + `src/common/errors.cpp`**（T0.4 的一半）：§6.3 的 20 个码 + `http_status()` + `message()` 集中一处。`[D-29]` 的"业务码与 HTTP 状态码同步"落在这里，且 **2xxx 段（判题结论）一律映射 200** —— 它们走 HTTP 200 + `data.verdict`，不是 API 异常。`web/admin.h`（T3.12，早于本文件）自带的 `error_code_of/error_message_of` 暂未改动，接线 T3.5 统一包络时应让它们转调本文件
  - **单测** `tests/bcrypt_test.cpp` **11 条**（纯计算，无 DB，247ms）+ `tests/auth_api_test.cpp` **20 条**（纯逻辑 10 + 真库 10；连不上 MySQL 时真库组整体 `GTEST_SKIP`）**全绿**。**AC-1 四条全部有对应断言**：重复用户名 1005 / 错误密码 1001 / 登出后旧 Cookie 查不到身份（路由层据此回 1002）/ 登录发的 Cookie 能维持会话
  - **1010 那条分支原先写不出用例，处置是【把策略做成递进来的值】而不是改 config**：`common::config()` 是进程级只读快照、`load_config()` 只能整体重载一次，单测改它会污染同进程其它模块的结论。故新增 `RegisterPolicy{allow_register}`，生产入口 `register_user(...)` 内部读 `config()` 填它（.env 仍是唯一真源，`§6.3.1` 的声明式取向不变），单测递 `false`。用例同时钉住了**开关先于参数校验**——挪到校验之后的话，注册关闭状态下"用户名为空"会回 1001，外部就能靠错误码反推出"这里曾经开放注册"
  - **回归**：`ctest --test-dir build` **377/377 通过**（本轮新增 31 条；唯一跳过的 `LoaderTest.DatabaseFailureRollsBack…` 是 MySQL 账号缺 `SUPER` 建不了触发器，早于本轮）
  - **⚠️ 一处【刻意不覆盖】的分支**：并发注册撞 `uk_username` 的 1062 → 1005。`[D-21]` 的全局互斥量让同进程两个线程必然串行，后到者的判重查询必然看到已插入的那行，因此**在本项目的线程模型下这条分支不可达**，要造出来只能靠注入时序的测试钩子。它的兜底价值也低（判重 + 唯一索引已是双重保证），故不写一条永真的用例充数；将来若 `Pool::size > 1` 让 DB 访问真正并行，这条分支会变为可达，届时必须补测
  - **本条已勾选**。**HTTP 侧已在 T0.5 一轮接线完毕**：`common/response.h` 已有统一包络，`Set-Cookie` 头经 `web/routes.cpp` 真正落到响应上（实测 `sid=<64 hex>; HttpOnly; SameSite=Lax; Path=/`，逐字符合 `[D-69]`），四个端点均可 `curl` 走通（AC-1 四条全部达成，见 T0.5 条目）。**唯一仍未覆盖的**是 `OJ_ALLOW_REGISTER=0` 的 1010 端到端用例 —— 需要改 `.env` 重启才能触发，而真库组跑在 `allow_register` 的内置默认值 `true` 下；该分支的业务层已有单测（`RegisterPolicy{false}` 递进），只是没有 HTTP 层的验收

  - **已完成会话/Cookie 层 `include/web/session.h` + `src/web/session.cpp`**（bcrypt 与注册/登录端点已由同一条目的后续条目补齐，见上）：
    - **五个纯函数**（不碰 DB，永远可单测）：`generate_token()`（`RAND_bytes` 32 字节 → 64 小写 hex）、`is_token_wellformed()`、`parse_sid()`、`make_set_cookie()`、`make_clear_cookie()`。`Set-Cookie` 逐字为 `sid=…; HttpOnly; SameSite=Lax; Path=/`（§6.3 / `D-69`），**故意不加 `Secure`**（明文 HTTP 下加了永不下发），欠账与启用 HTTPS 时的三条动作见 §6.3.2.1
    - **`parse_sid` 要求名字【全等】匹配**：`xsid=` / `sid_extra=` / `SID=` 一律不算本站会话；剥一层成对双引号；**第一个 `sid` 说了算**（同名重复只可能来自不同 Path 的遗留，继续往后找会让规则随浏览器排序而变）
    - **token 形状检查先于任何 SQL**：`sessions.token` 是 utf8mb4 `CHAR(64)`，默认排序规则 `utf8mb4_general_ci` **大小写不敏感** —— 不先钉死"64 个小写 hex"，`WHERE token='AB…'` 会命中库里那条 `ab…`。`find_user()` 是唯一的查会话入口，将来新增查会话 SQL 必须同样先过这一关
    - **五个 DB 动作**：`purge_expired()`（§6.3.2）、`issue()`（**同事务内先回收再签发**，避免并发登录互删对方刚落库的行）、`find_user()`（单条 JOIN 取 username/role，过期即不存在，**读路径不写库**）、`destroy()`（幂等）、`destroy_others()`（`AND token <> ?` 保留当前会话，`D-40`/`AC-21`）
    - **`expires_at` 一律由 MySQL 算**（`UTC_TIMESTAMP() + INTERVAL ? DAY`），TTL 唯一来源是 `config().session_ttl_days` —— C++ 侧没有第二处 TTL 常量，也没有第二处时钟
    - **`AuthUser` 收敛到 `web/session.h`**：它由会话层产出、被 `admin.h`/路由/列表页消费，故定义在生产方一侧。`admin.h` 的 `require_admin()` 签名与其 15 条单测**零改动**（改为 include `session.h`）
    - **匿名流量不碰 DB**：`authenticate()` 先解析 Cookie、**没带 sid 连租约都不取**（`[D-43]` 允许匿名读题面，不该在 `[D-21]` 的全局互斥量上排队）
    - **单测** `tests/session_test.cpp` **26 条全绿**（纯逻辑 13 + 真库 13；连不上 MySQL 时真库组整体 `GTEST_SKIP`），全量 `ctest` **345 条执行全通过 + 1 条既有跳过**（`LoaderTest.DatabaseFailureRollsBack…`，MySQL 账号缺 `SUPER` 建不了触发器，与本模块无关且早于本轮）
    - **仍待接线**（本轮已补齐 ①）：② 启动时调一次 `purge_expired()`（T3.9）；③ `ratelimit::forget(user_id)` 与注销联动（T3.18 `D-70`）
- [ ] T3.4 限流器：每用户 60s 滑动窗口 + 同时评测数配额
- [ ] T3.5 路由：§6.3 全部端点 + 统一包络 + 错误码映射
  - **已落地：统一包络 + 认证组四端点**（随 T0.5 一并完成，详见 T0.5 条目 —— 路由层 `web/routes.cpp` + 包络 `common/response.cpp`）
  - **本条【不勾选】**：§6.3 的 20 个端点里目前只接线了 4 个，题目组（T3.6）、提交组（T3.7/T3.8/T3.15/T3.16）、admin（T3.12）、改密（T3.13）、注销（T3.18）都还是"业务层已就绪、路由未接线"
  - **`web/admin.h` 自带的 `error_code_of`/`error_message_of` 暂未改动** —— 它们与 `errors.h` 对同一码给出同样的结果，故不构成不一致；等本条把第 5 个端点接上去时统一转调，避免中途改两处
  - **错误收口的判据已抽成纯函数 `web::error_reply_for(status, body_already_written)` 并被单测钉住**（`ErrorRouting{reply, protocol_level}`）
    - **为什么必须抽**：判据一旦与日志、switch、httplib 类型写在一起，就只能靠 curl 人工比对 —— 而**本轮真的在这个判据上踩过**（见 T0.5 条目：`auth/me` 未登录实测回 `404/1004` 而不是 `401/1002`）。修完是"一句话规则"，但"一句话"不等于"改不掉"
    - **`protocol_level` 随决策一起返回，而不是让调用方重推一次状态码集合**：重推就要在 `routes.cpp` 里再写一遍 `{404,405,413}`，两处白名单各改一处就分叉（§8.1.1 的通则）
    - **单测** `tests/routes_test.cpp` **12 条**（**不起服务、不 listen、不占端口、不碰 MySQL** —— 这批用例在"mysqld 没起"和"8080 被占"的机器上都照跑不误，刻意不同于 mysql_pool_test / auth_api_test 的真库组，那两组没库就跳过，于是"全绿"可能只是全跳过）
      - ⭐ **`BelowABodyWeAlreadyWroteIsNeverRewritten`**：body 非空 ⇒ **任意** status（含 200/418/451 这些"不常见的"）一律放过。这就是那个 bug 的钉子
      - **判别力已实测**：把判据退回 `if (status != 404) return std::nullopt;` 后 **8/12 条立即变红**，且关键那条的失败信息直接点名后果（"未登录 auth/me 会因此从 1002 变成 1004（AC-1 的会话断言会塌）"）
      - 另钉：空 body 恒得包络 / 404·405→1004 / **413→1001 且保留人话 reason**（即上面那个真 bug）/ 协议级 status 恒→1001 且 `protocol_level=true` / 显式认识的三种不标协议级 / **`EveryBranchAgreesWithTheErrorTable` 是关系式断言**（HTTP 状态恒等于包络码的 `http_status`，加新分支也不会漏）/ 静态目录缺失 ⇒ `register_routes` 返回 false、真目录 ⇒ true、同一 Server 注册两次仍 true
- [ ] T3.6 列表查询：多条件筛选 + 分页 + JOIN 进度与收藏
  - **已完成业务层 `include/web/problem_api.h` + `src/web/problem_api.cpp`**：`GET /api/problems`（keyword / difficulty / tags / progress / favorite / sort / page / size）、`GET /api/tags`、`GET /api/problems/{slug}` 三个端点的"算什么、要几条 SQL、失败时怎么退"，camelCase 契约由 `to_json()` 独家序列化。SQL 随业务层走而**不**进 `db/dao.h`，与 T3.12 同一取舍（dao.h 尚空，T3.2 落地时再把聚合语句收敛进 problem 组）
  - **查询串解析是纯函数**（`parse_list_query` / `url_decode` / `validate_list_query`）：枚举值非法一律 1001 并指名 field（静默回退会让用户拿到"看起来筛了其实没筛"的列表）；未知参数名忽略；同名参数最后一个生效；`size` 超上限夹到 100（`kMaxPageSize`，封顶串行化 DB 上的单次大查询，响应回显实际生效值）
  - **两条 SQL：一条 COUNT、一条取页，共用同一个 `append_where()`**。两条各写一套 WHERE 时，"总数 47 但第 3 页是空的"这种错不会崩、只会让分页器发疯
  - **匿名语义只有一个值：`user_id = 0`**（`kAnonymousUserId`）。登录/未登录的 SQL 文本完全相同，只差一个参数 —— 少一半语句模板就没有"两套模板各自演化"的漂移面。⚠️ 依赖"匿名不得携带个人筛选"这条：`progress≠none` / `favorite=1` 在匿名时返回 **1001**（而不是静默忽略或返回空列表）
  - **标签聚合 `JSON_ARRAYAGG` + `CASE WHEN t.id IS NULL THEN NULL ELSE ... END`**：LEFT JOIN 的空行会让 `JSON_ARRAYAGG(JSON_OBJECT('name', NULL,...))` 产出 `{"name":null}` 的假标签，而它不是 NULL 所以 COALESCE 兜不住（MySQL 的 JSON_ARRAYAGG 忽略 NULL 值，故用 CASE 把它整个变 NULL）。聚合组内顺序在 C++ 侧按 `(category, name)` 定死 —— `JSON_ARRAYAGG` 不支持 ORDER BY
  - **`is_favorite` 必须写成 `COALESCE(MAX(up.favorite), 0)`**：`ONLY_FULL_GROUP_BY` 下 MySQL 只能从 `p.id` 推断 `p.*` 的函数依赖，推不出 `up.*`（实测报 1055）
  - 标签筛选用**逐标签一个 EXISTS**（AND 语义 = 同时挂全部所选标签），不用 JOIN+DISTINCT：JOIN 会把题乘以标签数，`LIMIT/OFFSET` 就在乘完的结果上取，第 2 页直接漏题
  - 关键词 LIKE 的 `% _ \` 全部转义并显式写 `ESCAPE`（用户搜 `100%` 时 `%` 必须当字面量）；四个排序方案都以 `p.id ASC` 收尾 —— 少一个稳定 tiebreaker，"翻页不重不漏"会偶发漏题
  - **单测** `tests/problem_api_test.cpp` **63 条全绿**（纯逻辑 34 + 真库 29；连不上 MySQL 时真库组整体 `GTEST_SKIP`），`-DOJ_SANITIZE=address,undefined` 尚未重跑（本机 LSan 受限，见根 `CMakeLists.txt` 注释）
  - **本条仍不勾选**，两处待接线：① 路由接线 —— 统一包络（`common/response.h`）与 Cookie → 登录上下文（T3.3 的 `web::authenticate()`）**均已就绪**（T0.5 已把它们用在认证组四端点上），但 `GET /api/problems` 本身还没挂进 `register_routes()`；② 收藏切换端点（`POST /api/problems/{slug}/favorite` 属 T4.9 的后端，未做）
- [ ] T3.7 提交端点：校验 → 落库 pending → 入队 → 返回 201
- [ ] T3.8 提交详情端点：仅本人可见 + 按 §6.3 的三步组装 `caseResults` / `compileErrors`
  - `submission_case_results` 按 `case_idx ASC`；`submission_compile_errors` 按 `seq ASC`
  - 字段名映射为 API 契约里的 camelCase，**前端零改动**
  - **归属校验不因 admin 放宽**（§6.3.1）
- [ ] T3.9 启动顺序：DB 就绪 → 校验/建表 → 加载题库（同步入库 + 灌缓存）→ **角色同步** → 启动 judge → listen
  - 建表之后、加载题库之前，跑一次过期会话回收（§6.3.2）
  - 角色同步紧接题库同步之后，两者复用同一套「改配置 + 重启」的运维模式
- [ ] T3.10 过期会话回收（§6.3.2）：启动时一次 + 每次登录时一次，`DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP()`
  - **已完成 `session::purge_expired()`**（`src/web/session.cpp`）：必须用 `UTC_TIMESTAMP()` 而非 `NOW()`（容器时区非 UTC 时会误删未过期会话）；复用同一个全局 `DB mutex`（吃调用方递进来的租约）；回收失败仅记日志，不阻断登录；走 `idx_expire` 无需新增索引
  - **「每次登录时一次」已并入 `session::issue()`**（同事务内回收 + 插入，§6.3.2 的约束），所以登录端点只需调它一个入口
  - **边界语义已在单测里钉死**：`expires_at == 当前秒` 的行**不被回收**（语句是 `<`）但**已不能登录**（查询是 `>`）—— 两条语句在同一秒上给出互补答案；改任一侧的比较符都会红
  - **仍待接线**：T3.9 启动顺序里的那一次调用（本模块只提供函数，没有调用点）
    - **✅ 已接线（本轮，随 T0.5 一起）**：`main.cpp` 在「工作区扫残」之后、「HTTP listen」之前调一次 `purge_expired()`，即 §2.3 启动顺序的第 7 步。管的是"服务停了三天再起来"那一段 —— 那些行没人会去登录，也就没人会触发登录时的回收
    - **失败只记 WARN 不拒绝启动**：查会话的 SQL 自带 `expires_at > UTC_TIMESTAMP()`，过期会话本来就查不出来；回收失败不影响任何在线功能，而"起不来"比"多留几行过期会话"严重得多
    - 启动回显里带上本轮回收条数（实测 `已回收过期会话 0 条`），回收彻底不工作时至少有一条可 grep 的证据
  - **`EXPLAIN` 已实测（8.0.46）**：回收语句 `type=range / key=idx_expire`（正是本条要求的形态，无需新增索引）；`find_user()` 的 JOIN 两侧都是 `type=const`（token 走 PRIMARY、user_id 走 PRIMARY 常量折叠）
- [ ] T3.11 角色同步（§6.3.1）`[D-37]`
  - 4 条约束逐条落地：未设置则跳过 / 双向同步 / 缺失用户名逐个 WARN / 同步后回显名单
  - 同步 SQL 走预处理语句占位符（`IN (?,?,...)`），**禁止把名单拼进 SQL 字符串**（§1.4）
  - 幂等：连续执行两次结果必须完全相同
- [ ] T3.12 只读管理端点（§6.3.3）`[D-37]`
  - `require_admin()` 守卫：先判登录（1002）再判角色（1003）
  - `GET /api/admin/overview` 返回 `system` + `users[]`，`solved`/`attempted` 复用 §6.4 口径
  - **打通路由层 → scheduler 的访问路径**，否则 `queueLength`/`activeJudges`/`judgeAlive` 拿不到
  - **锁顺序**：先取 scheduler 快照并释放其锁，再取 `DB mutex` 查 DB；禁止 `DB mutex → scheduler lock` 嵌套
  - **已完成业务层 `include/web/admin.h` + `src/web/admin.cpp`**：`require_admin()` 守卫、`load_system_counts()`/`load_users()` 两条聚合 SQL（全在 SQL 内、无 N+1）、`build_overview()`（**先取快照后取租约**，§6.3.3 实现约束 2 落地）、`to_json()`（camelCase 契约）。三个入口都做成"依赖外部递进来"（`AuthUser` / `StatusProvider` / `db::Session&`），接线时不必改本文件
  - **单测** `tests/admin_test.cpp` **15 条全绿**（守卫顺序、JSON 字段契约、锁顺序、失败路径不变式 9 条纯逻辑 + 真库聚合 6 条；探针数据带 `ojadm` 前缀并在 `TearDown` 级联删净）。`-DOJ_SANITIZE=address,undefined` 下同样全绿
  - **本条仍不勾选**，两处待接线：① 路由接线 —— 统一包络与 Cookie → 登录用户（T3.3 的 `web::authenticate()`，`AuthUser` 由会话层递进来）**均已就绪**（T0.5 已用在认证组四端点上），但 `GET /api/admin/overview` 还没挂进 `register_routes()`；② 运行态三项的真实来源（T2.7 调度器；当前 `unwired_status_provider()` **如实**返回 `judgeAlive=false`，不拿 DB 的 pending 行数冒充 `queueLength`）
  - **两处口径是本轮定的、文档没写死的**（代码里各只需改一行，注释已标注）：`judgingCount` 取 `status IN ('pending','judging')`（只数 `judging` 会让 judge 未接线时排队中的提交在页面上彻底不可见）；`users[].solved` 与 `attempted` **不互斥**（`attempted` = 有过任意提交的题数，与 §6.5 的 `byDifficulty`/`byTag` 写法同源；单题粒度仍走 §6.4 的 `CASE`）
- [ ] T3.13 改密码端点（§6.3.5）`[D-40]`
  - `POST /api/auth/password`：bcrypt `compare` 验旧密码 → 事务内 `UPDATE password_hash` + `DELETE FROM sessions WHERE user_id=? AND token<>?`
  - **`AND token <> ?` 保留当前会话是硬要求**（AC-21 专门验证）
  - 复用同一个全局 `DB mutex`；两个 `user_id` 相同的 SQL 走预处理语句占位符
  - 错误一律 1001 + `data.field`（`oldPassword` / `newPassword`），**不新增错误码**
  - **不加独立限流器** —— 威胁模型是"已登录用户改自己的密码"，见 §6.3.5 的说明
  - **会话侧零件已就绪**：`session::destroy_others(user_id, keep_token, removed, error)` 就是本条的 `DELETE FROM sessions WHERE user_id=? AND token<>?`（T3.3 已实现并单测），缺的是 bcrypt 验旧密码与路由接线
- [ ] T3.14 `GET /api/me/overview`（§6.5）`[D-38] [D-39]`
  - 返回 `{totals, byDifficulty[], byTag[], recent[]}`；`recent` 取最近 3 条提交摘要
  - **`byTag` 必须用 `COUNT(DISTINCT p.id)` + 两个 `SELECT DISTINCT problem_id` 子查询**；直接 JOIN `problem_tags` 后 `COUNT(*)` 会因一题多标签重复计数（AC-22 验证）
  - `byDifficulty` 基表不过标签表，`COUNT(*)` 即可 —— 两者写法不同不是笔误
  - 全部聚合在 SQL 内完成，**不得全表捞 `submissions` 回应用层聚合**（§1.4 的 P95 < 200ms 要求）
  - `user_id` 三处出现（三条 SQL），全部用预处理语句占位符传参
  - **不新增任何表或列**（§3.5）
- [ ] **验收**：`curl` 走通注册→登录→列表→提交→轮询→历史全链路；另用 admin 账号 `curl /api/admin/overview` 返回 200、普通账号返回 1003
  - **前三步已通**（T0.5 一轮实测：注册 201 → 重复 1005 → 登录 200 + `Set-Cookie` → 带 Cookie `auth/me` 200 → 登出后 1002）。**「列表」之后全卡在 judge 与提交端点**（T2.7 / T3.7 未开工）；admin 端点同样待路由接线（T3.12 三处待接线已从三处减为两处）
  - **登录链路的【单测层面】现已全覆盖**：`bcrypt_test` 11 + `auth_api_test` 20 + `session_test` 26 + `response_test` 17 + `routes_test` 12 = **86 条**，全量 `ctest` **406/406 通过**、0 跳过（唯一 Skipped 是 `LoaderTest.DatabaseFailureRollsBack…`，MySQL 账号缺 `SUPER` 建不了触发器，早于本轮）
- [ ] **验收**：改密链路 `curl` 走通 —— 登录拿 cookie A → 登录第二个终端拿 cookie B → 用 A 改密 → B 调 `auth/me` 返回 1002、A 仍返回 200、旧密码登录失败
- [ ] T3.15 diff / 删除 / 重跑三个端点 `[D-42] [D-49] [D-58]`
  - `GET /api/submissions/diff?from=&to=`：归属校验写进 `WHERE id=? AND user_id=?`（不单独查再比）；`from==to` → 1001；返回两段原文 + `hunks`，**diff 算法在前端**（服务端只传原文，见 §6.3.6）
  - `DELETE /api/submissions/{id}`：明细行靠外键 CASCADE 连带删，**不显式 DELETE 子表**
  - `POST /api/submissions/{id}/retry`：`status='failed' AND verdict='JE'` 才允许，否则 **1011**；成功则 `INSERT ... SELECT` 复用原代码新建一条，**不复用原 status/verdict/明细行**
  - `GET /api/submissions/{id}` 响应追加 `canRetry` 布尔
- [ ] T3.16 队列位置 `queueAhead` `[D-54] [D-60]`
  - `POST /api/submissions` 在 INSERT 后跑 `SELECT COUNT(*) FROM submissions WHERE status='pending' AND id < ?`
  - **⚠️ 在代码注释里写明这条 SQL 依赖「FIFO + 按 id 升序出队」**（§6.3.6 的隐含契约，R-13）
  - **不返回全局队列长度**，不返回任何其他用户信息
- [ ] T3.17 「下一题建议」`nextRecommendation` `[D-45] [D-59]`
  - 并入 `GET /api/problems/{slug}` 响应，**不单独开端点**
  - 规则见 §6.6：`score = 1000 / (1 + 薄弱度(主标签))`，同分按 difficulty ASC → sort_no ASC
  - **主标签 = 该题 algorithm 标签中通过率最低者** `D-68`；并列取 `tags.id` 最小（保证可复现）；**无 algorithm 标签的题不进候选集**
  - 候选集 = `progress ≠ 'solved'` 的题；**必须可解释** —— 返回值里带现成的理由文案字符串（与主标签同源，不会出现"说的标签和算分的标签不是同一个"）
  - 全部通过 → 返回 `null`（前端渲染完成度）；**未登录 → 字段整个不下发**
  - **已完成（与 T3.6 同一模块，`include/web/problem_api.h`）**：两条 SQL（标签通过率 + 候选题及其 algorithm 标签）+ 一个**纯函数** `pick_recommendation()` 做决策
  - **为什么拆成"取数 + 纯函数"**：`D-68` 的主标签是"先按题聚合、再在标签间取极值"的两层比较，写成 SQL 要么套窗口函数要么自连接；而 AC-46 要求**连跑 5 次同一输入得同一题**，这种可复现性放在可直接单测的纯函数里最容易保证
  - **打分比较用整数交叉相乘**（`(tA-sA)*tB < (tB-sB)*tA`），不用 double —— 否则"本该并列却不相等"的抖动会让同分 tie-break 变成不可复现。`score` 只留在结构体里供核对，不参与决策
  - 同分 tie-break 在 SPEC 的 `difficulty → sort_no` 之后**补了 `id ASC`**：前两者都可能重复，没有最后一层兜底时结果仍取决于输入顺序（单测 `IsReproducibleForTheSameInput` 每轮打乱候选顺序来钉这一条）
  - 理由文案用**算分时选中的那个标签**的通过率拼装（`「链表」标签你通过 2/5，本题尚未通过`）—— 顺序反过来就会出现"理由说的和算的不是同一个"（AC-34）
  - 候选集为空有**两种**情形（全部通过 / 题库里没有 algorithm 标签），都序列化为 `nextRecommendation: null`；**未登录是第三种**：字段整个不下发（AC-25 / T4.22 整张卡不渲染）。三种形状各自有单测
  - 推荐查询失败**不让整个详情端点失败**（宁可少一张卡，也不要用户在读题时看到 1008），但错误仍进 `app.log`
- [ ] T3.18 注销账号端点 `[D-56] [D-57] [D-70]`
  - `DELETE /api/auth/account {password}`：验密码 → 1001/field=password
  - 成功路径**只有一条 SQL**：`DELETE FROM users WHERE id=?`，靠两级 CASCADE 完成（§3.6）
  - **不删 `problems`/`problem_cases`/`tags`**（题库是全局资源）
  - **成功后调 `ratelimit::forget(user_id)` 清内存滑窗**（"同时评测数"是 DB 派生量，随 CASCADE 自动归零，只有滑窗要显式清）
  - 走同一个全局 `DB mutex`；清当前 Cookie
- [ ] T3.19 题库加载器支持 `hint_md` `[D-47]`
  - 字段映射表补一行（§4.1）；缺省 `NULL` 而非空串（两者语义不同，前者"不渲染 tab"，后者"渲染空 tab"）
  - **写侧早已由 T1.2 的加载器落地**（`loader.cpp` 落库 NULL、`json_schema.cpp` 缺省/空串都映射为 NULL）；**读侧由 T3.6 的详情端点下发**：JSON 键名照抄 §6.3 端点表的拼写 `hint_md`（**snake，与相邻 camelCase 字段不同名** —— 改成 `hintMd` 要同步改 T4.20 与 AC-26），值为 `null` 即"不渲染提示 tab"（T4.20）
- [ ] **验收**：六个新端点 `curl` 全通；`canRetry` 逻辑正确；`queueAhead` 在单任务时恒为 0

### M4 — 前端（Day 14–17）

- [ ] T4.1 骨架：`app.css` 设计系统（色板/间距/按钮/表格/徽标）+ 顶栏 + 通用 `api.js` `common.js`
  - **已完成 `static/css/app.css`**：CSS 变量色板 + 4px 基准间距 + 按钮/表单/卡片/表格/徽标/toast + `pre.ascii-art`（`[D-48]` 的横向滚动不换行）+ 登录页与列表页的页面级区块。**为什么必须用变量而不是字面量**：同一批颜色要在五页出现几十次，字面量分散后"换 accent"要全文替换，漏掉的那一处会静默留下不协调的色块 —— 不崩、不可测、只能靠肉眼发现
    - **主题定为「浅色冷灰 + 靛蓝 `#4F46E5`」**（实测走过一版深色青，出于两条理由被否）：① 语义色里绿=AC、橙=WA/TLE、红=MLE/RE/CE 已占满三原色，主色再取绿或青会与结论徽标撞脸；② 靛蓝在白底上 7:1+ 且不与任何 verdict 色相撞。科技感靠 mono 标签 + 3.5% 网格底纹，不靠霓虹/扫描线
    - **`color-scheme` 随主题切换**：否则会出现"白底 + 深色滚动条/下拉/自动填充"的割裂
    - **输入框的校验失败态用「左边框 + 淡底」而非整块红底**：整块红底会让用户以为服务端拒绝了请求，而登录页的绝大多数红框只是"格式不对，还没发出去"
  - **已完成 `static/js/api.js`**（请求层）
    - **一个请求只有两种结局**：`resolve({code, message, data})`（**包络内的失败一律 resolve**）/ `reject(ApiError)`（没拿到包络：网络断、静态 404 的 HTML、连接被拒）
    - **为什么不「code !== 0 就 throw」**：未登录（1002）不是异常 —— 顶栏要**渲染未登录形态并继续跑**（`[D-52]`），轮询遇到 1002 要跳登录页并把草稿带过去（`[D-44]`）。两者都不是"调用栈里没人能处理"的错误；让它们 throw 就得 everywhere 加 try/catch，漏一处就变成"未登录时白屏"
    - **先判 JSON 再判状态**：静态文件 404 返回的是 HTML，直接 `response.json()` 会抛 SyntaxError，被误报成"服务器内部错误"，用户会去刷新而不是检查地址
    - **`credentials: 'same-origin'` 显式写出**：默认值本来就是它，但写出来是为了让"登录态为什么能生效（HttpOnly Cookie，JS 读不到）"在代码里可见 —— 将来若有人为调跨域 API 改成 `'include'`，这一行就是他会先看到的地方
    - **`currentUser()` 模块级缓存**：顶栏、概览条、admin 入口判定读的是同一份登录态，各自 fetch 会在 Network 面板出现多条一模一样的 `/api/auth/me`，AC-24/AC-35 的验收反而要花时间解释"为什么重复"
  - **已完成 `static/js/common.js`**（`renderTopbar()` + `escapeHtml()` + `toast()` + `redirectToLogin()` + `formatLocalTime()`）
    - **顶栏是同一个组件按登录态条件渲染，不是三套模板**（§7.1 的三张线框看着像三份模板，但只在两处不同）。写成多套模板，AC-23「普通用户两者都不可见，**DOM 中不存在该元素**，不是 CSS 隐藏」就得靠"记得给另一套也删掉"来保证，而那个"记得"迟早会漏；条件渲染里"不渲染"是 default 分支的**默认结果**
    - **`[D-41]`：普通用户不渲染任何角色徽标**（不显示"普通用户"字样）—— 那是一条对每个用户都无信息量的标签
    - **`login.html` 不调用本组件**（§7.1 明确：登录页不渲染顶栏，否则同页出现「登录」和「登出」两个按钮，AC-35 专门验证这一点）—— 故本组件做成"由页面主动调用"而非自动挂载
  - **本条【不勾选】**：题目页的编辑器/左右分栏（T4.4/T4.5）与五页其余四页的顶栏接线属 T4.3~T4.14
- [√] T4.2 `login.html`：登录/注册切换、表单校验、redirect 跳转
  - **已完成 `static/login.html` + `static/js/auth.js`**（ES Module，无构建步骤，§7.1）
  - **本地校验与服务端逐条对齐**：用户名 3–32 且字符集 `[A-Za-z0-9_-]`、注册时密码 6–64（`auth.js` 里逐条注明对应 `auth_api.h` 的哪个常量）。两边各写一份必然漂移，而漂移的症状很隐蔽：前端放行一个 40 字符用户名，服务端回 1001，用户看到的是"表单自己清空了"却不知道为什么
    - **登录时【不】校验密码长度**：改密码功能（T4.17）尚未上线，历史账号的密码可能只有 5 位，前端先拦一道会让正确口令也登不进去，且错误文案（"长度不足"）指向错误方向。注册才查 —— 服务端注册时一定会查（§6.3 四道闸门之三）
  - **本地校验不通过就不发请求**（与 T4.17 改密表单同一条约定）。代价是服务端那条分支在浏览器里不触发，但它本来就该由 §9 的 curl 验收与单测覆盖，前端重复跑一遍只会更慢
  - **`redirect` 参数做了站内校验**（`[D-44]` T4.27 / T4.28）：只接受**站内绝对路径**，且拒绝 `//evil.com`（协议相对）与 `/\evil.com` —— **只查"以 / 开头"是不够的**，这是最常见的一处"看起来校验过了"；另外拒绝跳回 `/login.html` 自身（未登录拦截跳登录、登录后再跳回登录 = 死循环）。校验不过退回 `/list.html` 而不是报错：手敲坏参数不该被拦住
  - **注册成功后自动走一次 login**：单实例下注册者就是本人，再输一遍密码是纯摩擦。但**不能在注册响应里种 Cookie** —— §6.3 的注册端点不签发会话（只有 `201 {userId, username, role}`，无 `Set-Cookie`，实测确认），故这里显式再调 login。代价是多一次 bcrypt（约 100ms，§6.2.1 要点 3）
  - **`[D-46]` 不预填上次用户名**：只给 `<input autocomplete="username">`（让浏览器记住**值**），不写 `value`；且**已登录用户访问本页会被直接送走**，省得在"登录"表单上点一下才被弹回
  - **三个页面级提示各有明确出处，不是"通用错误区"**：`?deleted=1`（注销完成，`[D-57]` T4.29）/ `?expired=1`（会话过期，`[D-44]` T4.27）/ **注册被关闭不在这里判断** —— §7.1 明确"不加探测注册是否开放的端点"，而是等用户真提交后由 1010 触发，那才是准确的信息
  - **1005 与 1010 走整体提示而非字段红字**：这两个码下用户名本身合法，挂到输入框下会让人以为是格式问题，改用户名或等管理员开注册才对；而 1001 才按 `data.field` 挂到对应输入框下（§6.3）
  - **登录前清空密码框再跳转**：不是洁癖 —— 浏览器 bfcache 可能在同页返回时恢复出带密码的表单，那等于把口令留在页面上
  - **前端逻辑验证**（node 24 + 极简 DOM 桩，无框架无依赖，脚本未入库）：9 条全绿 —— 非法用户名本地拦下且**零请求** / 错误文案挂到 `username` 框 / 注册密码过短本地拦下且零请求 / 登录**不拦**短密码并发出请求 / 注册流程 = `register` 然后 `login` 两次 / 合法站内 `redirect` 生效 / `//evil.com` 被拒回 `/list.html` / 绝对外站 URL 被拒 / 跳回 `/login.html` 被拒
  - **对着真服务跑的 11 条集成验证全绿**（node 复用真实的 `static/js/api.js`，Cookie 由桩手工带上 —— 这正是浏览器替我们做的事）：未登录 `auth/me` → 1002 且 **resolve 而非 throw** / 注册 → 0 且拿到 `userId` / 注册拿到 `role='user'`（**不因注册提权**）/ 注册后 `auth/me` 仍 1002（注册端点确实不签发会话）/ 错密码 → 1001 `field=password` / 登录 → 0 且 `Set-Cookie` 的 sid 是 64 位小写 hex / 带 Cookie `auth/me` → 0 且用户名一致 / 登出 → 0 / **登出后 `auth/me` → 1002（AC-1）** / 未知端点与静态 404 都 resolve 成 1004 包络 / 连不上服务 reject `ApiError`
  - **已知欠账（属 T4.3，不是本条缺陷）**：登录成功跳 `/list.html`，而 `list.html` 仍是 0 字节 → 浏览器看到 1004 包络。§9 的 T4.2 只要求"redirect 跳转"，跳转本身已通
- [√] T4.3 `list.html` **（兼首页）**：概览条 + 筛选栏 + 表格 + 分页 + 空态 `[D-38]`
  - **已完成 `static/list.html` + `static/js/list.js`**（原生 ES Module，无构建步骤）：搜索(防抖 300ms) / 难度分段 / 进度 / 排序 / 仅看收藏 / 标签多选芯片 / 表格 / 分页窗口 / 空态「清除筛选」
  - **未登录时 `progress` / `favorite` 两个控件直接置灰并给出说明，而不是发出去再报错** —— 服务端对匿名带这两个参数一律回 1001（API.md §4.2），前端发过去只会得到一条红色报错，却说不出"该登录"
  - **分页跟随服务端回显的 `page`**：删题 / 改筛选后页码可能越界，不跟随会卡在永远返回空列表的页上
  - **关键词防抖 300ms**：每敲一个字打一次 DB 是最容易被忽略的负载来源
  - 顶部概览条见 T4.15
  - 概览条与表格是**两个独立的数据流**：翻页/筛选只重发 `GET /api/problems`，**不重取 `overview`**（避免翻页时概览条闪烁）
  - **顺带把登录/注册的 tab 抽成 `.seg` 分段控件**（T4.2 与本条的难度筛选共用一份样式）：两处各写一份，早晚出现"登录页圆角改了、筛选栏没改"
  - **对真服务冒烟验证**：匿名 `tags`/`problems` 正常；注册→登录→`me/overview` 返回 `byDifficulty`（含 `total=0` 的难度，实测确认 `total=0` 整条不渲染的分支会命中）→ `POST /favorite` 返回 `isFavorite=true` → `favorite=1` 筛出该题 → 匿名带 `progress` 得 1001「按进度筛选需要登录」。测试账号已 `DELETE /api/auth/account` 注销，无残留
- [√] T4.4 `problem.html` 左右分栏：可拖拽、比例持久化
  - **已完成 `static/problem.html` + `static/js/problem.js` + `static/css/problem.css`**：左题面/右编辑器，结果面板占下半屏全宽（§7.2）
  - **比例存 `localStorage["split:problem"]`，夹在 25%~75%**：夹上下限是因为不夹的话一次误拖能把题面或编辑器压成 0，而那种状态下**两边都没有任何东西可恢复**
  - **splitter 是 `role="separator" tabindex="0"`，方向键调 2%**：只做鼠标拖拽等于把键盘用户挡在外面，而 §1.4 只要求桌面浏览器并不代表可以用鼠标
  - 900px 以下折成上下（`.pane-right` 给 `min-height: 70vh`，否则编辑器会被压成一条缝）
- [√] T4.5 编辑器集成：**Ace 1.32.7**（原计划 CodeMirror 5，实施时换库，理由见 §6.2）：模板预置、草稿 debounce 保存、Ctrl/⌘+Enter 提交
  - **已完成 `static/js/editor.js` + `static/vendor/ace/`（4 个文件 + BSD LICENSE）**：`text/x-c++src` 对应的 `c_cpp` 模式、`tomorrow` 浅色主题（与全站浅色底一致）、`tabSize: 4` + `useSoftTabs`（用空格不用 Tab，用户拷到本地能直接编译）、`wrap: false`（代码不折行，折行会让行号与诊断对不上）
  - **`useWorker: false`**：编译诊断的真源是服务端 g++ 输出（§5.6），两套诊断同时开会让编辑器红线与判题结论不一致；且 cdnjs 的 ace 包不含 `worker-*.js`
  - **降级路径保留**：`window.ace` 不存在时挂 textarea，功能（提交/草稿/Ctrl+Enter）全在，只是没有着色 —— [D-73] 消除 CDN 依赖后它已降为防御性冗余
- [√] T4.6 Markdown 渲染 + DOMPurify + 提示折叠
  - **已完成 `static/js/markdown.js`**：`marked.parse()` → `DOMPurify.sanitize()` 白名单（**无 `img`**，`D-48`），链接只放行 http/https 并补 `rel="noopener noreferrer"`
  - **题面与提示走【同一个函数】**（`renderMarkdown`），`D-47` 的"同一条管线"由此保证，而不是两处长得一样的调用
  - 字符画靠 `.markdown pre { overflow-x: auto; white-space: pre }`（`D-48`）
- [√] T4.7 提交 + 轮询 + 状态徽标 + 结果面板
  - **已完成 `static/js/poll.js`**：1.5s 轮询、**终态才停**、组件销毁时清定时器、5 分钟未出结果提示"可稍后在「我的」页查看"
  - **`judging:<slug>` 存 sessionStorage 而非 localStorage**（`D-53`）：评测态是 per-tab 的，跨标签页共享会让用户以为有两条评测在跑
  - **`queueAhead` 每秒递减是【推算】**（`D-54` `D-60`），真值由下一次轮询校正 —— 为一个能推出来的数字单独发请求等于自己压测自己
  - **提交失败的文案【以服务端话术为准】**：1009「判题服务不可用」不改成"judge 未接线"，那是实现状态而非用户能据此行动的信息；内部状态另起一行小字（与列表页对 1001 的处理同一条约定）
- [√] T4.8 逐用例表格 + 编译错误行高亮 + 错误面板
  - **已完成**（`problem.js` 的 `renderResult` / `renderCompilePanel` + `problem.css` 的 `.result__*`）
  - 行高亮**只对 `file_name='user.cpp'` 的诊断生效**；`harness`/`link` 归属显示专属文案（§5.6）
  - **内存列表头写「峰值RSS」**，表下常驻口径脚注 `D-63`（不折叠）
  - **CE / JE 两种状态不渲染逐用例表** `D-65` `D-66`；CE 状态行显示「Y 个用例未执行」（**不是"通过 0/Y"** —— 那会让人以为跑了 Y 个全错）；JE 显示「平台错误」+ 重试按钮
  - **失败用例整行淡红 + 「结论」列文字**：颜色不能是唯一的信息载体
  - **`renderResult(el, data, {highlight})`：`highlight` 只在"刚提交的这条"为 true** —— 在「我的提交」里回看历史时若也高亮，会把旧诊断画到用户的新代码上
- [√] T4.9 收藏切换 + 收藏筛选
  - **切换**：列表页表格内星标 + 题目页右上角星标，均走 `POST /api/problems/{slug}/favorite`（语义是 toggle，响应回切换**之后**的状态）
  - **筛选**：列表页「仅看收藏」；**未登录时该控件置灰并给出说明**，因为服务端对匿名带 `favorite` 一律回 1001（API.md §4.2）
  - **「仅看收藏」开着时取消收藏会重取列表**：否则页面上会留下一条已经不符合筛选条件的行，用户会以为筛选漏了
  - **实测**：`POST /favorite` → `isFavorite=true`；随后的 `GET /api/problems?favorite=1` 只返回该题；匿名带 `favorite` → 1001「按收藏筛选需要登录」
- [√] T4.10 `history.html` **（拆三 tab）** `[D-39]`
  - **已完成 `static/history.html` + `static/js/history.js`**（样式落在 `app.css` 的「我的」页区块 —— §6.1 的 css 目录里没有 `history.css`，而本页样式大多是跨页可复用的）
  - **`location.hash` 决定激活 tab**，点 tab 改 hash 走 `hashchange`；可直接分享 `/history.html#progress`，刷新后停在原 tab
  - **三个 tab 共用一次 `overview`**：`overview.js` 的模块级 Promise 缓存保证**整个页面最多一条 `/api/me/overview`**（AC-24 要的就是「切 tab 与 tab1 分页不产生额外请求」）
  - **掌握分布【首次切到才渲染】**：没人打开的 tab 不该替用户产生这次请求；渲染后即被缓存，之后来回切零请求
  - **tab1 与 v1.2 行为一致**：题目/结论筛选 + 20 条/页分页 + 点行展开详情（代码 + 逐用例表 + 编译输出）+ 删除/重跑（T4.23）；同一时刻最多展开一条
  - **未登录直连 ⇒ `redirectToLogin()` 带 redirect**（整页不可看的场景，与题目页「弹内联登录框」不同[D-43]）
  - **`.tabs` 样式从 `problem.css` 上移到 `app.css`**：题目页一层、我的页三层，各写一份早晚会分叉
  - **实测**（node 最小 DOM 桩 + 真实服务 + admin 会话）：tab1 渲染出提交行（相对时间「刚刚」+ `title` 完整本地时间、结论徽标、`1/2` 用例、7 ms、2.0 MB）、tab2 渲染出难度卡与标签表、tab3 用户名与管理员徽标正确
  - tab1 **提交记录**（`#records`）：全局列表 + 筛选 + 展开详情 —— **与 v1.2 行为完全一致，零改动**
  - tab2 **掌握分布**（`#progress`）：见 T4.16
  - tab3 **账户设置**（`#account`）：见 T4.17
  - `location.hash` 决定激活 tab；点 tab 改 hash 走 `hashchange`，**不重新请求**（`overview` 缓存在模块级变量，页面生命周期内只请求一次）
  - tab 顺序固定：提交记录 → 掌握分布 → 账户设置
- [√] T4.11 题内最近 5 次提交
  - **已完成**（`problem.js` 的 `renderRecent` + 「我的提交」tab）：时间 + 结论徽标 + 用例数/耗时；**点一行即读那条提交的详情并渲染结果面板**，这是复盘的最小闭环
  - 匿名时 `recentSubmissions` 为空数组 ⇒ 渲染「还没有提交记录」而不是空白块
- [√] T4.12 时间本地化 + 相对时间
  - **已完成 `common.js` 的 `relativeTime()` / `parseUtcTime()`**：`刚刚` / `3 分钟前` / `2 小时前` / `昨天 15:24` / `10-14 20:01`，跨年才补年份
  - **列表用相对时间、`title` 放完整本地时间**：扫历史时问的是「我刚才什么时候提交的」（相对），复盘某条时问的是「准确几点」（绝对）—— 后者不该要求用户把鼠标停在每一行上
  - **抽出 `parseUtcTime()` 的原因**：后端下发的是 `"2026-10-14 20:01:02"`（空格分隔、**无时区**），直接交给 `new Date()` 会被当本地时间解析、**整体差 8 小时**；这个坑两个函数都会踩，所以解析只写一处
- [√] T4.13 空态/错误态/401 跳转/CDN 降级路径
  - **4 条 v1.3 空态全部落地**：未登录概览条 → 「登录后查看你的刷题进度」+ 登录按钮且**题库列表照常渲染**（`overview.js`）；零数据 → 进度条渲染 0% 空槽 + 「还没有提交记录，去做第一题 →」；`byTag` 为空 → 「题库暂无标签分类」；改密旧密码错 → 挂到对应输入框下且**新密码框不清空**（`history.js`）
  - **401 统一走 `common.js` 的 `redirectToLogin()`**：题目页 `returnToLogin()` 会额外带 `expired=1`（`D-44`），重新登录回来自动载入草稿并提示
  - **CDN 降级路径**：Ace 加载失败时显示一条**看得见**的降级提示条（提交/草稿/Ctrl+Enter 全部照常），而不是静默退回无高亮的 textarea（`D-73` 消除外网依赖后它只是防御性冗余）
- [√] T4.14 `admin.html` + 顶栏运维入口 `[D-37]`
  - **已完成 `static/admin.html` + `static/js/admin.js` + `static/css/admin.css`**（占位页已被完整实现替换）
  - `common.js` 按 `auth/me` 的 `role` 条件渲染顶栏「运维」链接（T4.1 已实现，本条复用）
  - 页面两块：`system` 状态卡（**7 个计数** + **3 项运行态**：队列长度 / 并发评测 / 判题调度器存活）+ `users[]` 表格（role 徽标、AC 率、solved/attempted、收藏、最近提交/结论）
  - **严格只读**：无表单提交、无写按钮；角色调整入口**不放在此页**（只经 `.env` + 重启）。**唯一交互是"刷新"按钮，重发的是同一个 GET**
    - **页面底部必须留一段说明**：写清"本页不提供任何写操作，角色由 `OJ_ADMIN_USERNAMES` 声明式指定"。否则管理员在这页上找不到改角色的按钮，会当成缺功能而不是刻意的边界
  - **AC 率 0 提交渲染「—」而非 0%**（与 T4.16 通过率同一条约定）：0/0 不是"成功率 0"，是"没数据"，渲染成 0% 会让清单出现假的最低分
  - **`judgeAlive=false` 用 pill「未接线」而不是渲染成 0**：队列 0 / 并发 0 是"空"，不是"没有"。当前 T2.7 未接线，两个数恒为 0 属**预期状态**，与"系统空闲"对用户的行动含义完全不同
  - **`lastVerdict` 为 null 时渲染「—」，不回退到上一条 AC**（API.md §7.1 明确要求）—— 回退会让管理员以为"最新这次就通过了"
  - **普通用户直连 → 1003 → 先渲染无权限说明、1.5s 后自动跳回 `list.html`**（保留「立即返回」链接）。不直接跳的理由：直接跳用户只看到"被弹走"，分不清是"没权限"还是"页面坏了"
  - **未登录 → 1002 → 跳 `/login.html?redirect=/admin.html`**
  - **样式复用 app.css 的 `.page/.card/.table/.stat/.badge/.verdict/.notice`**，admin.css 只写页面专属的区块骨架、运行态行与用户表列宽 —— 重复定义按钮/表格会让"换主色"要改三个文件
  - **实测三条路径**（node 最小 DOM 桩 + 真实服务 + 真实会话）：
    ① admin 会话 → `code=0`，渲染 7 行用户清单、1 个管理员徽标、`sys-note` 给出"判题调度器未接线"说明；
    ② 普通用户会话 → `1003`，不渲染任何数据，1.5s 后 `location.replace('/list.html')`（已观测到）；
    ③ 无效会话 → `1002`，`location.href = /login.html?redirect=%2Fadmin.html`（已观测到）。测试账号已注销，库内无残留
- [√] T4.15 首页概览条 `[D-38]`
  - **已完成 `static/js/overview.js`**（SPEC §6.1 要求它单列一个文件，供 T4.16 的掌握分布 tab 复用同一份数据与缓存）
  - 横向卡片区：`你好，{username}`（角色徽标复用 `common.js` 的 `roleBadgeHtml()`，与顶栏同一段逻辑）+ `已通过 x/N` `尝试中 y` `收藏 z` `提交 s` + 三条难度迷你进度条 + 最近 3 条提交
  - 进度条宽度 = `solved/total*100%`；**`total=0` 的难度整条不渲染**（不显示 `0/0` 的空槽），而 `solved=0 且 total>0` 渲染成 0% 空槽（§7.4「新用户零数据」）
  - `recent` 每条 = 时间 + 题名 + 结论徽标，点击跳 `/problem.html?slug=`；`recent` 为空时渲染「还没有提交记录，去做第一题 →」
  - **未登录时显示「登录后查看你的刷题进度」+ 登录按钮，题库列表照常渲染**（`GET /api/problems` 的 `Auth=否`），概览条不隐藏
  - **模块级 Promise 缓存**：页面生命周期内只请求一次（AC-24 要求切 tab / 翻页不产生额外的 `/api/me/overview` 请求）
  - **顺带把角色徽标抽成 `common.js` 的 `roleBadgeHtml(user)`**（T4.18）：顶栏与概览条两处各写一遍，早晚出现"顶栏改了徽标、概览条没改"的不一致，而这种不一致只有肉眼能发现
- [√] T4.16 tab2 掌握分布 `[D-39]`
  - **已完成 `overview.js` 的 `renderProgressPanel(el)`**：左侧难度卡（`已过/总数` + 进度条 + 通过率），右侧标签表（`标签 / 已过 / 总数 / 通过率`）
  - **与首页概览条同源同数值**（AC-24）：两者读同一个 `loadOverview()` 的同一个 Promise，一致性由「只有一份实现」保证，不是靠两边口径相同的约定
  - **`total=0` 渲染「—」而不是 `NaN%`**；难度 `total=0` **整行不渲染**（与 T4.15 同一约定）
  - **只渲染 `category=algorithm`，`source` 类折叠进「展开更多」**；顺序沿用服务端 `total DESC, name`，前端不重排
  - **多标签题在【每个】标签下各计 1 次**：`byTag` 的 total 之和 ≥ 题目总数，这是多对多的正常展开，不是重复计数（§6.5 头号陷阱）
  - 左侧难度卡：3 行（简单/中等/困难），每行 `已过/总数 + 百分比 + 进度条`
  - 右侧标签表：列 = 标签名 / 已过 / 总数 / 通过率；**按 `total` 倒序**；只渲染 `category=algorithm`，`source` 类折叠进「展开更多」
  - 通过率 = `solved/total`，`total=0` 时渲染 `—` 而非 `NaN%`
  - 标签为 0 → 整区显示「题库暂无标签分类」（§7.4）
  - **验收重点**：一道挂 3 标签且已 AC 的题，在该 3 个标签的「已过/总数」中各出现 1 次（AC-22）
- [√] T4.17 tab3 账户设置 `[D-40]`
  - **已完成**：只读区（用户名不可改 + 理由；角色徽标复用 `common.js` 的 `roleBadgeHtml`）+ 改密码表单 + 登出
  - **本地校验：非空、6–64 位、两次一致**；不一致时本地拦下标红，**不发请求**（与登录页同一条约定）
  - **1001 按 `data.field` 挂到对应输入框下**；**旧密码错时新密码框不清空**（§7.4：便于只改旧密码后重试）
  - **成功后 toast + 清空表单 + 静默重拉 `auth/me` 并重渲染顶栏**（§6.3.5 要点 5：角色真源在 `.env`，可能已被运维改过）
  - **登出走 `api.js` 的 `logout()`**，它会清掉 `currentUser()` 的模块级缓存 —— 否则同页顶栏仍显示已登录
  - **本页【不做注销账号】**：那是 T4.29（输密码 + 逐项列出待删数据量的二次确认[D-57]），与「改密码」不是一个风险等级，不在同一条里顺手加
  - 只读区：用户名（不可改，改用户名会破坏 `OJ_ADMIN_USERNAMES` 绑定，§6.3.5 要点 6）+ 角色徽标
  - 改密码表单：旧密码 / 新密码（`type="password"`）+ 确认修改
  - 前端校验：非空、6–64 位、两次一致；不一致时本地拦下标红，**不发请求**
  - 1001 时按 `data.field` 把错误文案挂到对应输入框下方；**旧密码错时新密码框不清空**
  - 成功后：toast + 清空表单 + **静默重拉 `auth/me`**（§6.3.5 要点 5）+ 更新角色徽标
  - 底部：登出当前账号（复用 v1.2 逻辑）
- [√] T4.18 顶栏角色徽标 `[D-41]`
  - **已完成**：徽标片段抽成 `common.js` 的 `roleBadgeHtml(user)`，被顶栏、首页概览条、`history.html` tab3、`admin.html` 四处复用
  - **普通用户不渲染任何徽标**（不显示「普通用户」字样）；`history.html` tab3 的「普通用户」是**只读字段的取值**，不是徽标，两者刻意区分
  - `admin` → 用户名右侧渲染 `管理员` 徽标（accent 色底 + 12px），并在左侧渲染「运维」链接
  - 普通用户 → **不渲染任何徽标**（不显示"普通用户"字样），也不渲染「运维」链接
  - 与 T4.14 的运维入口**共用同一段 `auth/me` 渲染分支**，不写两遍
  - 首页概览条的"你好，{username} [角色]"**复用这段逻辑**，不另写一份
  - 顶栏「提交历史」文案改为「我的」，指向 `history.html`（默认 `#records`）
- [ ] **验收**：§1.2 成功标准链路端到端手动走通；admin 账号能打开 `admin.html` 看到状态与用户清单，普通账号看不到入口且直连被拒
  - **admin 部分已达成**（T4.14 实测三条路径：admin 渲染状态与清单 / 普通用户 1003 后跳回 / 无效会话 1002 跳登录）
  - **端到端链路卡在 `judge 未接线`（T2.7）**：`POST /api/submissions` 稳定返回 1009「判题服务不可用」且不写库，因此"提交 → 轮询 → 逐用例结果"这一段无法走通。前端已把该分支渲染清楚（红条 + 说明），但**在 T2.7 落地前这条验收不可能达成**，不得因为"前端都做完了"就勾它
- [ ] **验收**：首页概览条与 tab2 的 `solved/total` **完全一致**（同一端点同一次请求，AC-24）；改密后其他设备被踢、当前设备不掉线
  - **一致性由结构保证**：两者读 `overview.js` 里同一个 `loadOverview()` 的同一个 Promise，不是"两边口径相同的约定"（T4.15 / T4.16）
  - **待补的验收动作**：在 DevTools Network 里数 `/api/me/overview` 的请求条数（切 tab + tab1 翻页应恒为 1 条），以及"改密后其他设备被踢"的真实双会话验证（需两个浏览器会话，当前尚未做）
  - **本机无法做性能类验收**：`D-74` 的双档要求 NF-1~NF-4 与 AC-38 必须在 4G 机器上跑
- [√] T4.19 顶栏两套形态 `[D-52]`
  - **已完成**：单组件 `renderTopbar()` 按 `auth/me` 条件渲染；未登录形态下**不渲染**「我的」与「运维」（DOM 中不存在该元素，不是 CSS 隐藏）
  - **`login.html` 不挂顶栏**；题目页用同一组件的 `compact` 变体；`index.html` 落地页复用同一组件（不写第三套）
  - 顶栏的「运维」入口与 `admin.html` 的越权访问都由后端独立校验，前端隐藏只是体验不是安全边界
  - `common.js` 的 `renderTopbar()` 单组件：`auth/me` 返 1002 → 登录/注册形态；200 → 我的/运维/用户名/角色徽标/登出形态
  - `login.html` **不挂顶栏**；题目页用同一组件的 compact 变体（单行）
  - 未登录形态下**不渲染**「我的」与「运维」入口（DOM 中不存在该元素）
- [√] T4.20 题目提示 `[D-47]`
  - **已完成**：`hint_md` 为空 ⇒ **整个提示 tab 不渲染**（不是渲染一个空 tab）；非空才显示，与题面共用 `markdown.js` 的同一条渲染管线，免费无限看、无解锁流程
  - `hint_md` 为空/缺省 → **不渲染提示 tab**（不是渲染空 tab）
  - 非空 → 渲染，可折叠展开，**无次数限制、无解锁流程**
  - 与 `statement_md` **同一条** marked + DOMPurify 管线（`markdown.js` 加一个双参入口）
- [ ] T4.21 提交 diff 模块 `[D-42] [D-50]`
  - 新建 `static/js/diff.js`：LCS 行级 diff，输入两段代码输出 `hunks`（结构见 §6.3.6）
  - **行数 > 2000 时降级为整体替换显示**（R-11 的硬闸门，防 O(n·m) 内存爆炸）
  - 题目页「你的最近提交」每行加 checkbox，最多选 2 条，选满即请求并展开并排双栏
  - 每栏底部「回填到编辑器」：点击弹确认 → 写入 CodeMirror → **立即触发草稿保存**
- [√] T4.22 「下一题建议」卡 `[D-59]`
  - **已完成**（`problem.js` 的 `renderNext`）：三种形态严格区分 —— **字段不下发（未登录）整张卡不渲染** / 对象 ⇒ 渲染理由 / `null` ⇒ 换成"题库已全部通过"完成度文案。`reason` 是服务端算好的文案，前端不编
  - 题目页底部卡片：题名 + 难度徽标 + **理由字符串**（服务端给的，不前端编）+ 跳转按钮
  - 全部通过 → 替换为「🎉 题库 N/N 全部通过」+ 提示编辑 `problems/*.json` 后重启
  - **未登录 → 整张卡不渲染**（推荐依赖个人薄弱项，不做"登录后推荐"的虚假承诺）
- [√] T4.23 历史页删除 / 重跑 `[D-49] [D-58]`
  - **已完成**：展开详情后每条一个「删除这条提交」，`canRetry` 时额外一个「重跑」
  - **删除的确认框必须写清连带删掉了什么**（「含 N 个用例结果」）：用户点的是「删除一条」，实际连带删掉的是 N 行用例结果与 M 行编译诊断（外键 CASCADE）
  - **`canRetry` 由列表已有的 `status` + `verdict` 前端算出**，不再为每一行额外请求详情：服务端 `can_retry = (status=='failed' && verdict=='JE')`（`submission_api.cpp:294`），而这两个字段列表响应里都有。⚠️ 服务端规则若改，前端这一行要跟着改 —— 这条耦合是**故意保留**的，它省掉的是每次翻页的 N 次往返
  - **删除/重跑成功后重取列表**（而不是就地改一行）：总数变了、行序也变了，就地修改更容易让人误以为「它被替换了」，而 [D-58] 的语义恰恰是**多出一条新记录**
  - **实测**：删除一条含 2 行用例结果的提交 → `code=0`，`submission_case_results` 残留 **0 行**（CASCADE 生效）；重跑一条 `failed/JE` → 归属与状态校验通过后回 `1009`（judge 未接线），前端透传话术并恢复按钮
  - 每行「删除」→ 确认弹窗（含"含 N 个用例结果"）→ `DELETE` → 移除该行、`total` 减一
  - `canRetry=true` 时才渲染「重跑」→ 成功后用 `newSubmissionId` 启动轮询
  - **原记录保持 `failed` 不变**；历史里多一条 `pending`（用户要能理解这一点）
- [√] T4.24 队列位置显示 `[D-54] [D-60]`
  - **已完成**：`queueAhead===0` 只显示「评测中…」；`n>0` 显示「前面还有 n 个评测」，每秒递减由 `poll.js` 的独立心跳驱动，**不为此额外发请求**
  - `queueAhead===0` → 只显示「评测中…」；`n>0` → 「前面还有 n 个评测」
  - 轮询时用上一次的 `queueAhead - 1` 递减推算，**不为此额外发请求**
- [√] T4.25 刷新恢复轮询 `[D-53]`
  - **已完成**：`judging:<slug>` 存 sessionStorage；挂载时若存在且对应提交仍 `pending/judging` ⇒ 重建状态区与轮询，终态则清键
  - **判断"是否还在跑"要先请求一次**（`isStillJudging`）：仅凭 key 存在就重建，会让一条早已完成的提交重新转圈
  - 提交成功后 `sessionStorage["judging:"+slug] = submissionId`
  - `problem.html` 挂载时若该键存在且对应提交 `status ∈ {pending,judging}` → 重建轮询与状态区；终态后删键
  - **用 sessionStorage 不用 localStorage** —— 评测态是 per-tab 的，不该在另一标签页也转圈
- [ ] T4.26 草稿冲突检测 `[D-55]`
  - 草稿写入同存 `draftAt:<slug> = Date.now()`
  - 挂载时若 `draftAt` 晚于本页已加载草稿的时间 → `confirm`「此代码已在另一个标签页更新，是否载入？」；**用户选否则保留本页内容**
- [√] T4.27 会话过期恢复草稿 `[D-44]`
  - **已完成**：会话过期（1002）→ `returnToLogin()` 跳 `/login.html?redirect=<题目路径>&expired=1`；重登跳回后 `problem.html` 检测 `expired=1` 且 `localStorage` 有草稿 ⇒ 弹「已恢复你上次未完成的代码」
  - **草稿本来就自动载入**（`loadDraft(slug)` 是编辑器的初始值），所以"恢复"不需要额外写盘逻辑 —— `D-44` 真正防的是"跳转时把代码丢掉"，而它全程没离开 `localStorage`
  - 401 跳 `/login.html?redirect=<题目路径>&expired=1`
  - 重登跳回后 `problem.html` 检测 `expired=1` → 载入 `localStorage` 草稿 + 提示「已恢复你上次未完成的代码」
- [√] T4.28 未登录访客交互 `[D-43]`
  - **已完成**（题目页部分）：提交按钮渲染为「登录后提交」，点击**弹内联登录框而不跳转**（跳转会丢正在写的代码）；登录成功后**不刷新页面**（刷新会连编辑器内容与光标一起扔掉），而是就地更新登录态 → 重拉题目详情补齐 `recentSubmissions`/`nextRecommendation` → **自动把刚才那次提交补上**
  - 收藏星标未登录时点击 ⇒ 跳登录页带 redirect（题目页是单栏，跳转不构成"丢代码"的损失，与提交按钮的处置不同）
  - 「我的」入口不渲染、直连 `history.html` 跳登录带 redirect —— **已随 T4.10 落地**（`history.js` 挂载时 `currentUser()` 返回 null 即 `redirectToLogin()`）
  - 提交按钮 → 弹**内联登录框**（不跳转！跳转会丢正在写的代码）；登录成功后自动跳回并恢复提交意图
  - 收藏星标 → 描边态，点击弹登录框；登录后**不自动补收藏**（不隐式写操作）
  - 「我的」入口不渲染；直连 `history.html` → 跳登录带 redirect
- [ ] T4.29 注销账号 `[D-56] [D-57]`
  - tab3 底部「注销账号」→ 输密码 → **二次确认弹窗逐项列出待删数据量**（提交 N 条 / 收藏 M 题 / 会话 K 个）+ 红字「不可恢复」
  - 确认 → `DELETE /api/auth/account` → **清空 localStorage 中所有 `draft:*` 与 `draftAt:*`** → 跳 `/login.html?deleted=1`
  - 登录页据 `deleted=1` 显示「账号已注销」提示
- [√] T4.30 字符画样式 `[D-48]`
  - **已完成**：`.markdown pre { overflow-x: auto; white-space: pre; }`（`problem.css`）—— 换行会破坏树/图/矩阵的形状，窄屏必须横向滚动而不是折行
  - **DOMPurify 白名单里没有 `img`**（`markdown.js` 的 `ALLOWED_TAGS` 实测确认），题面里的 `<img>` 会被整条剥掉而不是"只是不显示"
  - **同一条规则也保护了 `pre` 本身**：`ALLOWED_ATTR` 只放行 `href/title/colspan/rowspan/target/rel`，题面里的 `<pre onload=…>` 带不走任何行为
- [√] T4.31 **落地页 `index.html`**（v1.10 新增，**对 `[D-38]` 的一次显式偏离**，理由与边界见 §7.1）
  - **已完成 `static/index.html` + `static/js/landing.js`**（样式并入 `app.css` 的落地页区块）：Hero + 能力四卡 + 五步流程 + 三个技术决定 + 页脚
  - **顶栏复用 `common.js` 的同一个组件**（`[D-52]`）：落地页不写第三套顶栏
  - **"实时数字"只用匿名可读的端点**：`GET /api/problems`（取 `total`）与 `GET /api/tags`；`GET /api/stats` 需登录（1002），**在未登录首屏不可用**
  - **取不到就整行隐藏，不显示 0**：0 会被读成「题库里一道题都没有」，与 §7.4 对空态的一贯要求同源
  - **图标一律内联 SVG**（不用 emoji、不引 icon CDN，`[D-73]` 同一条理由）；hero 字号用 `clamp()` 跟着视口走，笔记本与大屏两处都好看
  - **入场动画只动 hero 的五块、一次 0.45s**，并有 `prefers-reduced-motion` 降级（`app.css` 末尾已有关闭全站动画的兜底）
  - **路由影响已实测**：`/` 现在由 httplib 的目录索引自动服务 `index.html`（200）；**`/api/nope` 仍是 404 + `{"code":1004}` JSON 包络**，未知静态路径仍 404 —— `routes.cpp` 当初"不做未知路径 → index.html"担心的那个问题没有发生
  - **v2 改版（居中 + 极光 + Bento，内容从 4 段扩到 8 段）**：
    - **排布**：全页**居中**，`max-width: 1080px`。Hero 与所有分区标题居中，卡片网格居中
    - **"炫"只用三样东西**：① 极光（三个大号径向渐变球缓慢漂移，`position: fixed` 不跟随滚动）；② **Bento 网格**（大小错落的卡片拼贴，取代"四张一模一样的卡"）；③ 渐变文字（`background-clip: text`）+ 玻璃卡片（`backdrop-filter`）+ 悬停发光
    - **三条克制线**（破了就不叫炫酷而叫花）：正文对比度保持 AA 以上；**持续动画只有两处**（极光、标签跑马灯），其余只在 hover 触发；全部动画有 `prefers-reduced-motion` 降级
    - **新增内容**：七种结论徽标墙（AC/WA/TLE/MLE/RE/CE/JE 各一句人话解释）、提交后五步链路图、**结果面板界面示意**（明确标注「示例数据」，并解释"为什么内存列叫峰值 RSS"）、与通用 OJ 的六维对比表、技术规格六项、FAQ 五条、收尾 CTA
    - **标签跑马灯用真实标签**（`/api/tags`），且**内容必须复制两份**：动画从 0 走到 -50%，只有一份时后半程会露出空白边缘。标签少于 6 个时**不做动画**（3 个标签慢速滚动比静止更吵）
    - **`.result__*` / `.case--*` 样式从 `problem.css` 上移到 `app.css`**：这三处画的是同一种表，而 `history.html` 与 `index.html` 都不加载 `problem.css` —— 留在原处等于**历史页展开详情的失败行一直没有高亮**（一个只有上移样式表才会暴露的 bug）
    - **实测**：匿名访客拿到 `题目 1 / 算法标签 3 / 来源标签 1`，跑马灯渲染出真实标签；六页的 class 与 CSS 定义已交叉校验无缺

### M5 — 题库录入与打磨（Day 18–21）

- [ ] T5.1 录入 20 道覆盖高频题型的题（数组/哈希/双指针/链表/树/二分/动态规划/贪心）
- [ ] T5.2 补齐到 50 道（视进度）
- [ ] T5.3 `tests/run_tests.sh` 自动化验收脚本
  - **派生列一致性断言**（§3.4 的维护契约）：对每条 `status='done'` 的提交，用 §3.4 给出的
    `HAVING` 查询校验 `case_passed` / `case_total` / `total_ms` / `max_mem_kb` 四个汇总列，
    返回行数必须为 0。**必须包含至少一条含 TLE 或 MLE 用例的提交** —— 那类用例的
    `time_ms`/`mem_kb` 为 NULL，是 `COALESCE` 约定唯一能被真正验证的路径。
  - **无残留多值断言**：`submissions` 表不存在 `case_results` / `compile_errors` 列
  - **角色同步断言** `[D-37]`，逐条对应 §6.3.1 的 4 条约束：
    1. 名单 `alice,carol` 同步 → 二人 `admin`、`bob` 为 `user`
    2. **连续同步两次** → 结果完全相同（幂等）
    3. 名单改为仅 `bob` → `alice`/`carol` **降权**为 `user`（验证双向，而非单向提权）
    4. 名单含不存在的用户名 → 同步成功、该名字在日志中 `WARN`、**不影响**其他用户提权
    5. 变量**未设置** → `role` 列零变化；变量**显式空串** → 全部降为 `user`
  - **会话回收断言**：插入若干 `expires_at` 分别为过去/未来的会话行，触发一次登录后 `SELECT COUNT(*) FROM sessions WHERE expires_at < UTC_TIMESTAMP()` 为 0，且未过期行数不变
  - **admin 端点断言** `[D-37]`：admin 账号 `GET /api/admin/overview` 返回 200；普通账号返回 1003；未登录返回 1002；`users[].solved`/`attempted` 与列表页（§6.4 口径）取值一致
  - **标签聚合不重复计数断言** `[D-39]`（对应 §6.5 的头号陷阱，AC-22）：
    1. 造一道**同时挂 3 个标签**且已 AC 的题 → 该题在**这 3 个标签**各自的 `total`/`solved` 中**各计 1 次**（不是 3 次）
    2. `byDifficulty` 三个 `total` 之和 = `problems` 表总行数
    3. `byTag` 所有 `total` 之和 ≥ 题目总数（因为多标签题被计多次 —— 这是**正确**行为，与上一条不矛盾）
    4. 对同一用户，`byTag` 的 `solved` 与 `totals.solved` 在"至少一个标签的题"上口径自洽：任取一道已 AC 的题，它在其所有标签的 `solved` 中都应出现
  - **改密端点断言** `[D-40]`（AC-21）：
    1. 旧密码错 → 1001 且 `data.field='oldPassword'`，且 `password_hash` **未变**
    2. 新密码 5 位 → 1001 且 `data.field='newPassword'`
    3. 正常改密后：该用户**除当前 token 外**的会话行全部消失；当前 token 调 `auth/me` 仍 200
    4. 旧密码登录失败；新密码登录成功
    5. `user_id` 走预处理占位符（注入串 `' OR 1=1--` 作为密码时不影响其他用户行）
  - **v1.4 CASCADE 链完整性断言** `[D-57]`（R-12，AC-33）：
    ```sql
    -- 断言 A：五张表的 users 侧外键全为 CASCADE
    SELECT COUNT(*) FROM information_schema.REFERENTIAL_CONSTRAINTS
    WHERE CONSTRAINT_SCHEMA = DATABASE()
      AND DELETE_RULE <> 'CASCADE'
      AND (REFERENCED_TABLE_NAME = 'users'
           OR REFERENCED_TABLE_NAME = 'submissions');   -- 必须返回 0

    -- 断言 B：真正级联删干净（造数据 → 注销 → 残留计数为 0）
    -- users 会话、submissions、submission_case_results、submission_compile_errors、user_problems
    ```
  - **重跑不覆盖原记录断言** `[D-58]`：造一条 `failed/JE` 提交 → 调 retry → 断言原记录 `status`/`verdict` **未变**、新记录 `status='pending'`、两行 `code` 相同
  - **`canRetry` 与 1011 断言**：`failed` → `canRetry=true`；`done` → `canRetry=false` 且直接调 retry 返回 1011
  - **`queueAhead` FIFO 断言** `[D-60]`：连投 3 条提交（用极慢用例占住 worker）→ 断言第 1 条 `queueAhead=0`、第 2 条 `=1`、第 3 条 `=2`
  - **删除自己的提交断言** `[D-49]`：删一条 `done` 提交 → 断言其 `submission_case_results` / `submission_compile_errors` 行也被清空（验证 CASCADE），且**另一用户的提交不受影响**
  - **diff 归属断言** `[D-42]`：用户 A 用自己的 `from` + 用户 B 的 `to` → 1003（**含 A 是 admin 的情况**）
- [ ] T5.4 压力测试：10 并发提交 × 20 轮，验证无崩溃、无永久 pending
- [ ] T5.5 恶意输入测试：死循环 / 内存炸弹 / 超长输出 / fork bomb / 空代码
- [ ] T5.6 README：架构说明、启动方式、题库编写指南、**`OJ_ADMIN_USERNAMES` / `OJ_ALLOW_REGISTER` 两个变量的作用与急救用 shell 命令（§6.3.1）**

---

## 10. 验收标准

### 10.1 功能验收（对应成功标准）

| ID | 验收项 | 通过条件 |
|---|---|---|
| AC-1 | 注册登录 | 重复用户名返回 1005；错误密码返回 1001；正确登录后 Cookie 可维持会话 7 天；登出后旧 Cookie 返回 1002 |
| AC-2 | 题库浏览 | 列表页能按关键词/难度/算法标签/来源标签/进度/收藏组合筛选；分页正确；无结果显示空态 |
| AC-3 | 题面渲染 | Markdown 表格、代码块、列表正确渲染；`<script>` 标签被 DOMPurify 清除（XSS 防护有效） |
| AC-4 | 代码编辑 | CodeMirror 加载成功，有 C++ 高亮与行号；刷新后草稿恢复；切题再切回草稿仍在 |
| AC-5 | 提交判题 | 提交后 1.5s 内页面显示"评测中"；正确答案 verdict=AC 且用例全绿 |
| AC-6 | 六种 verdict | 特制题目分别触发 AC/WA/TLE/MLE/RE/CE，`verdict` 与 `message` 均正确 |
| AC-7 | 逐用例详情 | 失败时表格展示每个用例的输入/你的输出/期望输出/耗时/内存，值与实际一致；DB 中 `submission_case_results` 行数 = `case_total` |
| AC-8 | 编译错误定位 | 故意写语法错误，`compileErrors` 正确给出行号，编辑器对应行高亮，面板显示原始 g++ 输出 |
| AC-9 | 资源限制 | 死循环题判 TLE；`vector` 无限增长的题判 MLE；超大输出被 `RLIMIT_FSIZE` 拦截不撑爆磁盘 |
| AC-10 | 提交历史 | 全局历史页能按题目/verdict 筛选；展开能看到代码全文与逐用例详情；题目页显示最近 5 次 |
| AC-11 | 进度推导 | 通过过的题在列表页标记"已通过"；仅编译错误的题标记"尝试过"；从未提交的标记"未开始" |
| AC-12 | 收藏 | 收藏切换即时生效；列表页"仅看收藏"筛选正确；重启服务后收藏不丢（落在 MySQL） |
| AC-13 | 鉴权边界 | 未登录访问 `/api/submissions` 返回 1002；用户 A 访问用户 B 的提交详情返回 1003 |
| AC-14 | 限流配额 | 单用户同时提交第 3 个时返回 1007；每分钟超 10 次返回 1006 |
| AC-15 | 重启恢复 | 手动 `docker compose restart`，重启后无永久"评测中"的任务，pending 任务被重新评测 |
| AC-16 | 用例落库与范式 | `problem_cases` 行数 = JSON `cases` 长度，`case_count` 与之一致；`problem_params` 与 `params` 长度一致；JSON 删用例后重启不残留幽灵行；`submissions` 表无多值 JSON 列 |
| AC-17 | 过期会话回收 | 构造若干已过期会话行，触发一次登录（或重启服务）后过期行被删除、未过期行不受影响；`NOW()` 与 `UTC_TIMESTAMP()` 混用导致的误删**必须**被测试覆盖（见 T3.10） |
| AC-18 | 角色声明式同步 `[D-37]` | `OJ_ADMIN_USERNAMES=alice,carol` 启动同步后二人为 `admin`、`bob` 为 `user`；连续同步两次结果相同（幂等）；名单改为仅 `bob` 后 alice/carol **被降权**；名单含不存在用户名时同步仍成功且日志 `WARN` 点名；变量**未设置**时 `role` 零变化、显式**空串**时全部降为 `user`；`OJ_ALLOW_REGISTER=0` 时注册返回 1010 且 `users` 行数不变 |
| AC-19 | 只读管理能力 `[D-37]` | admin 账号 `GET /api/admin/overview` 返回 200 且 `system`/`users[]` 字段完整；普通账号返回 1003；未登录返回 1002；`queueLength`/`activeJudges`/`judgeAlive` 取自 scheduler 内存态且未触发 `DB mutex → scheduler lock` 嵌套；`admin.html` 可打开且**无任何写操作**；普通用户顶栏不渲染「运维」入口 |
| AC-20 | 归属校验不因角色放宽 `[D-37]` | AC-13 仍然成立：用户 A 访问用户 B 的提交详情返回 1003，**包括 A 是 admin 的情况**（§6.3.1 的 conscious omission） |
| AC-21 | 改密码 `[D-40]` | 旧密码错返回 1001 且 `data.field='oldPassword'`，`password_hash` **未变**；新密码 5 位返回 1001 且 `field='newPassword'`；正常改密后该用户**除当前 token 外**的会话全部失效（另一设备的 cookie 调 `auth/me` 返回 1002），**当前会话仍返回 200**；旧密码无法登录、新密码可登录；**改密后不发生登出**（这是最容易做错的一条） |
| AC-22 | 标签聚合不重复计数 `[D-39]` | 造一道**同时挂 3 个标签**且已 AC 的题 → 该题在**这 3 个标签**各自的 `byTag[].total` 与 `.solved` 中**各计 1 次**（若实现为直接 JOIN 后 `COUNT(*)`，会各计 3 次，测试必须能区分）；`byDifficulty` 三个 `total` 之和 = `problems` 表总行数；`byTag` 的 `total` 之和**≥** 题目总数（多标签题被重复计入是正确行为） |
| AC-23 | 顶栏角色显示 `[D-41]` | admin 账号在任何页面顶栏可见 `管理员` 徽标 + 「运维」链接；普通账号**两者都不可见**（页面 DOM 中不存在该元素，不是 CSS 隐藏）；直连 `admin.html` 仍被 1003 拒（AC-19 保持成立）；首页概览条与顶栏的徽标文案一致 |
| AC-24 | 概览条与 tab2 同源 `[D-38] [D-39]` | 首页概览条的 `已通过 x/N` 与三个难度进度条数值，与「我的」tab2 的难度卡**逐项相等**（同一端点、同一份响应、同一渲染模块 `overview.js`）；切换 tab 与 tab1 分页**不产生额外的 `/api/me/overview` 请求**（DevTools Network 验证） |
| AC-25 | 未登录可读题 `[D-43]` | 未登录能打开**任意题**并看到完整题面与提示；提交按钮显示「登录后提交」；点收藏弹登录框；「我的」入口不渲染；直连 `history.html` 跳登录带 redirect；**未登录状态下 `GET /api/problems/{slug}` 的响应中不含 `nextRecommendation`** |
| AC-26 | 题目提示 `[D-47]` | 有 `hint_md` 的题渲染提示 tab，点击可展开，**重复点击不消耗任何东西、无次数提示**；无 `hint_md` 的题**不渲染该 tab**（DOM 中不存在）；`hint_md` 走 marked + DOMPurify，`<script>` 被清除 |
| AC-27 | 题面禁图片 `[D-48]` | 题面中的 `![](x.png)` 被 DOMPurify 过滤（渲染结果无 `img` 元素）；字符画题在窄屏（1024px）下 `pre` 横向滚动而**不换行**，图形不错行 |
| AC-28 | 提交 diff `[D-42] [D-50]` | 题目页勾选两条提交 → 展开并排双栏，**仅实际差异行**带 `+`/`-` 标记，相同行不带；「回填到编辑器」把该侧代码写入编辑器且当前草稿被覆盖（弹过确认）；只选 1 条时不发请求；`from==to` 返回 1001 |
| AC-29 | 删除与重跑 `[D-49] [D-58]` | 删除自己的提交：确认弹窗 → 记录消失，其用例结果行也被清空（验证 CASCADE），**题面页最近提交同步移除**；重跑 `failed` 提交：**原记录 `status`/`verdict` 未变**，历史多出一条新记录；重跑 `done` 提交 → 按钮不渲染，直接调接口返回 1011 |
| AC-30 | 队列位置 `[D-54] [D-60]` | 连续投 3 条提交（占住 worker）→ 响应 `queueAhead` 依次为 `0/1/2`；前端显示「前面还有 n 个评测」；**响应中不含任何全局队列长度或其他用户信息** |
| AC-31 | 刷新恢复轮询 `[D-53]` | 提交后在评测完成前**刷新页面** → 状态区自动重建并继续显示「评测中…」直至出结果；评测已完成后再刷新**不**重建（`sessionStorage` 键已清）；**另一个标签页不因本标签的评测而转圈**（sessionStorage 不共享） |
| AC-32 | 草稿异常路径 `[D-44] [D-55]` | ① 写入草稿后手动改 `localStorage.draftAt` 为未来时间再刷新 → 弹「此代码已在另一个标签页更新，是否载入？」，选「否」则**保留本页内容不被覆盖**；② 会话过期跳登录、重登回到该题 → 草稿自动载入并提示「已恢复你上次未完成的代码」 |
| AC-33 | 注销硬删除 `[D-56] [D-57]` | `information_schema` 中 `users`/`submissions` 侧的 `DELETE_RULE` **全为 CASCADE**（R-12）；注销后该用户的 `sessions`/`submissions`/`submission_case_results`/`submission_compile_errors`/`user_problems` 行**全部为 0**；**`problems`/`problem_cases`/`tags` 行数不变**（题库是全局资源）；密码错 → 1001 且不删任何数据；**二次确认弹窗逐项列出待删数据量**；注销后 localStorage 的 `draft:*` 全部清空 |
| AC-34 | 下一题建议 `[D-45] [D-59]` | 卡片显示的推荐题 = 规则算出的最高分题（可用题库小规模人工核对）；**理由字符串非空且与选中题的主标签一致**；把该题 AC 之后推荐**发生变化**；全部题通过后卡片变为完成度文案；未登录不渲染卡片 |
| AC-35 | 顶栏两套形态 `[D-52]` | 未登录：只有「登录/注册」；普通用户：有「我的/登出」无「运维/角色徽标」；admin：全部有且有 `管理员` 徽标；`login.html` 页面**不存在顶栏元素**；三种形态在题目页均能用 compact 变体渲染 |
| AC-36 | diff/删除的归属不放宽 `[D-42] [D-49]` | AC-20 立场延续：用户 A 请求 B 的提交做 diff → 1003；A 删除 B 的提交 → 1003。**包括 A 是 admin 的情况** |
| AC-37 | 历史分页与排序 `[D-61]` | 默认按 `created_at` 倒序、每页 20 条；翻页后顺序稳定不重不漏；筛选（题目/结论）与分页可组合 |
| AC-38 | **并发下不误判 TLE** `[D-62] [D-74]` | 造一个 CPU 耗时约 2.5s 的用例（远低于 3s 限制），在**全局 4 并发**下连续跑 10 轮 → **100% 得到 verdict（AC 或 WA），0 次 TLE**。这是 v1.4 墙钟保险阀必然失败的场景（§5.4.1），也是 R-14 停滞检测误判的检验。⚠️ **必须在 4G 机器上执行**（§1.4 双档），本机 1967MB 跑出的延迟数字不可信 |
| AC-39 | **阻塞能被抓住且不误判** `[D-62]` | ① `sleep(999)` → 1.5s 内判 TLE（停滞检测生效，不是等 35s 兜底）；② `while(true){}` → 由 `RLIMIT_CPU` 在约 4s 判 TLE；③ 正常 3ms 用例**不被停滞检测误杀**（连续 50 轮全 AC） |
| AC-40 | **0 条 `@@OJ@@` → JE** `[D-66]` | 用一份 `__expr` 语法错误的题库（生成出无法编译的 `tester.cpp` 但**不触发**生成期校验的场景，如 `harness_prelude` 里引用的函数未定义）→ 用户提交 → `verdict='JE'`，**不是 RE**；UI 显示「平台错误」+ 重试按钮，**不显示"运行错误"** |
| AC-41 | **CE 的用例数语义** `[D-65]` | 造一份语法错误的代码提交到有 12 个用例的题 → `case_total=12`（不是 0）、`case_passed=0`、`caseResults` 为空数组；UI 状态行显示「❌ 编译错误 · 12 个用例未执行」且**不渲染逐用例表**；`tests/run_tests.sh` 的派生列断言对这条记录**通过** |
| AC-42 | **平台文件报错归属** `[D-64]` | 造一份 `return_type: int` 但 `expected: [1,2]` 的题（生成 `std::vector<int> __exp = {1,2}` 编译不过）→ 提交 → verdict=CE，`file_name='harness'`，UI 显示「⚠️ 这不是你的代码出错…」；**编辑器不高亮任何行**；原始 g++ 输出可见 |
| AC-43 | **链接错误归属** `[D-64]` | 故意把方法名写成 `twoSum2`（签名不匹配）→ verdict=CE，`file_name='link'`，UI 显示四项签名核对清单；`compileErrors` **不含**行号项 |
| AC-44 | **内存口径标注** `[D-63]` | 逐用例表列头字面为「峰值RSS」；表下 `ⓘ` 脚注存在且**未折叠**；MLE 徽标写「内存超限 · 地址空间 128MB」；**RSS 数值与 MLE 徽标不在同一视觉层级**（不给人"两个数字对不上"的错觉） |
| AC-45 | **`failed` 的不变量与断言** `[D-67]` | 断言 SQL（§3.4 v1.5 版）对**所有** `status IN ('done','failed')` 的记录返回 0；用 `information_schema` 确认断言确实 JOIN 了 `problems`；手工构造一条 `failed` 且带 1 条明细行的脏数据 → 断言**必须报 1**（证明它真的在校验） |
| AC-46 | **主标签确定性** `[D-68]` | 造一道挂 3 个 algorithm 标签、通过率分别 8/10、3/10、5/10 的题 → 推荐理由指向**通过率 3/10 的那个标签**；把三者的通过率改成全相等 → 结果**可复现**（连跑 5 次同一输入得同一题）；无 algorithm 标签的题不出现在推荐候选里 |
| AC-47 | **题库加载期校验** `[D-64]` | 放 3 个 JSON：1 个正常、1 个 `expected` 容器深度不符、1 个 `args` 数量与 `params` 不符 → 服务**正常启动**，两个坏题被跳过并 `ERROR` 点名，好题正常可用；列表页**不出现**被跳过的题 |
| AC-48 | **vendored 前端库完整性** `[D-73]` | ① 断网状态下打开题目页：编辑器**有 C++ 语法高亮**、题面 Markdown 正常渲染、`<script>` 被 DOMPurify 清除（三项都不依赖外网）；② `static/vendor` 下 7 个 js 文件全部存在且 > 1KB；③ 3 份 MIT LICENSE 在 `static/vendor/LICENSES/` 内；④ **`Network` 面板中无任何指向 `cdn.jsdelivr.net` 等外部域的请求** |
| AC-49 | **自实现 bcrypt 正确性** `[D-75]` | 用 RFC 6234 / Provos 官方测试向量（含 `$2a$` 与 `$2b$`、不同 cost、空口令、70+ 字节口令、含 NUL 字节）验证：自实现输出与参考实现**逐字节一致**；再验证「注册 → 退出 → 重新登录」闭环通过。**不得**用"注册没报错"当作哈希正确的证据 |

### 10.2 非功能验收

| ID | 验收项 | 通过条件 |
|---|---|---|
| NF-1 | 提交延迟 | 10 用例以内的题，提交到出结果 P95 < 8s（4 核机器） |
| NF-2 | 页面性能 | 列表页首屏 < 1s（本地网络，50 道题） |
| NF-3 | 并发稳定性 | 10 用户 × 20 轮并发提交，进程无崩溃、无内存泄漏（RSS 增长 < 10%）、无永久 pending |
| NF-4 | 资源无泄漏 | 压测后 `/tmp/oj-judge/` 为空，僵尸进程数为 0，磁盘占用平稳 |
| NF-5 | 日志 | `app.log` 与 `judge.log` 均正常写入，按天切割；judge 日志含生成代码与编译命令 |
| NF-6 | 数据持久化 | `docker compose down && up` 后用户、题目、用例、提交历史（含逐用例结果行）、收藏全部保留 |
| NF-7 | SQL 注入 | 对所有字符串入参做注入测试（`' OR 1=1--`），无异常无绕过 |
| NF-8 | 机器稳定 | 连续运行 24 小时，judge 队列无卡死，HTTP 无 5xx 堆积 |

### 10.3 架构可讲述性（技术面试向）

| ID | 验收项 | 通过条件 |
|---|---|---|
| AR-1 | 隔离原理 | 能画出 fork 后的进程树，说明 `setpgid` + 进程组 kill 的必要性 |
| AR-2 | 生成器 | 能讲清"为什么用户代码与 harness 分两阶段编译"以及签名不匹配为何在链接期暴露 |
| AR-3 | 线程模型 | 能说明为什么 DB 用全局互斥量而 judge 用独立 worker 池，以及为什么不加大 worker 数 |
| AR-4 | 度量精度 | 能主动说明 `ru_maxrss` 是进程峰值而非单用例增量的精度限制 |
| AR-5 | 范式取舍 | 能讲清"为什么逐用例结果要拆表（1NF 的一格多值）"，以及"为什么结果表存快照而不外键引用 `problem_cases`"——后者是业务需求（历史可回放）压过范式纯洁性的例子 |
| AR-6 | 需求补全 | 能讲清 v1.4 那 20 项里"哪些是异常路径而非新功能"（D-44/D-53/D-55），以及"为什么 20 项需求只加了 1 列 DDL"（§3.6） |
| AR-7 | CASCADE 链 | 能说明注销为什么能用一条 `DELETE FROM users` 完成，以及为什么这条链一旦被改成 RESTRICT 会静默失效（R-12） |
| AR-8 | 隐含契约 | 能主动指出 `queueAhead` 的 `id < ?` 写法**依赖 FIFO 假设**（§6.3.6），并说明队列改优先级后该改哪里 |
| AR-9 | 交叉一致性审计 | 能讲清 v1.5 那 4 个硬漏洞的**共同成因**："两处各自合理的设计放在一起就矛盾"（§8.1.1），并能当场演示 v1.4 墙钟保险阀在 8 并发下的误杀推演 |
| AR-10 | 口径分裂 | 能说明为什么 `RLIMIT_AS` 与 `ru_maxrss` **不能统一**（Linux 的 `RLIMIT_RSS` 自 2.6.30 起不再强制执行，真按 RSS 限只能上 cgroup v2，与 D-33 双容器冲突），以及为什么"保留严格值 + 界面标注"优于"放宽阈值" |
| AR-11 | 判定完备性 | 能讲清「0 条 `@@OJ@@`」为什么是区分 RE（用户代码崩）与 JE（平台崩）的**唯一充分依据**，以及为什么生成期类型错误要判 JE 而不是 CE |

---

## 11. 关键风险的前置验证（Day 1 必做）

在写任何业务代码前，先用**最小可运行样例**验证三个高风险假设，避免 M1/M2 阶段才发现走不通：

```
V-1  验证 R-2（生成器 + tester 范式）
     手写一个 two-sum 的 problems JSON 与生成器原型，
     确认能生成可编译的 tester.cpp 并跑出 @@OJ@@ 协议行

V-2  验证 §5.4（fork + setrlimit 是否真的能拦住死循环与内存炸弹）
     手写一个死循环程序与一个内存炸弹程序，
     确认分别被判为 TLE 与 MLE，且不残留僵尸进程

V-3  验证 §5.3（stdout/stderr 分流是否可靠）
     确认用户 cout 的内容不会与 @@OJ@@ 协议行混淆
```

任一验证失败，立即启动对应的降级方案（R-2/R-4），不要等到 M2 结束。

---

## 12. 决策回溯

本文档任何一处若需修改，先回到 §0 决策表确认该决策的来源与理由，再同步更新：
架构图（§2）→ 数据模型（§3，含 §3.5 派生列判据）→ 题库 Schema（§4）→ 流水线（§5）→ API 契约（§6.3，含 §6.3.1–6.3.5）→ 聚合口径（§6.5）→ 前端交互（§7）→ 降级方案（§8）→ 验收标准（§10）。

**v1.3 的联动面** `[D-38~D-41]`：本轮**未改 §3.2 的 DDL**（无新表新列，见 §3.5），改动集中在 **§0 决策表 / §1.2–1.4 / §6.3 / §6.5 / §7 / §8 / §9 / §10.1**。

**v1.4 的联动面** `[D-42~D-61]`：20 项需求，**DDL 只新增 `problems.hint_md` 一列**（见 §3.6），改动集中在 **§0 / §1.2–1.4 / §3.2 / §3.6 / §4.1–4.2 / §6.3（+6 端点、+1 错误码 1011、+§6.3.6、+§6.3.7、+§6.6）/ §7.1–7.4 / §8（+R-10~R-13、+§8.1）/ §9 / §10.1（+AC-25~AC-37）/ §10.3（+AR-6~AR-8）**。

**v1.5 的联动面** `[D-62~D-70]`：4 个硬漏洞 + 6 处不一致，**DDL 零变更**（`case_total` 的修正只改注释与写入逻辑，不改列定义），改动集中在 **§0 / §1.4（口径声明）/ §3.2（注释）/ §3.4（断言 SQL 三处修订 + 不变量扩展）/ §4.2.1（新增）/ §4.3（新增校准步）/ §5.4（重写，含 §5.4.1/5.4.2/5.4.3）/ §5.5.1（新增）/ §5.6（归属分流）/ §5.6.1（新增）/ §5.6.2（新增判定表）/ §5.6.3（新增）/ §5.7（不变量扩展）/ §6.1（diff.js）/ §6.3.2.1（新增 CSRF）/ §6.3.7（+第 5 条要点）/ §6.6.1（新增）/ §7.2（结果面板三形态）/ §7.3（+4 条交互）/ §7.4（+3 条空态）/ §8（+R-14~R-19、+§8.1.1）/ §9（+T2.1.1/T2.4.1/T2.4.2/T2.5.1/T2.5.2，改写 T1.2/T2.2/T2.3/T2.4/T3.18/T4.8）/ §10.1（+AC-38~AC-47）/ §10.3（+AR-9~AR-11）**。

> **v1.5 特意没动的三处**：
> 1. **§2 架构与线程模型** —— 停滞检测发生在既有 worker 的等待循环内，**不新增线程**。
> 2. **§5.1 时序图** —— 步骤 5「逐用例 fork + setrlimit」不变，只是步骤 5 内部的等待逻辑换了。
> 3. **§6.3 端点清单** —— 9 个新端点与 20 条错误码**零新增**。所有修复都在既有端点的服务端行为或既有字段的语义上。

> **v1.4 特意没动的三处**，因为它们不该被这批需求带偏：
> 1. **§5 判题流水线逐字未改** —— D-42~D-61 全部是"提交记录怎么读/怎么操作"，判题侧零改动。
> 2. **§2 架构与线程模型未改** —— 没有新组件、没有新线程、没有新的中间件。
> 3. **§3.4 的受控反范式立场未改** —— `queueAhead` 与推荐都刻意**不落派生列**，与 §3.5 同一逻辑。

**v1.6 的联动面** `[D-71~D-75]`：环境落地轮。**DDL 零变更**（11 张表、0 列），**API 契约与 20 条错误码零新增**。改动集中在 **§0（+5 决策）/ §1.4（并发 8→4、内存双档）/ §2.1（拆 A/B 两图）/ §2.3（worker 4→2 + 闸门比例关系式）/ §5.4.2.1（新增兜底墙钟定量关系式）/ §6.1（+third_party、+static/vendor、+.gitignore、+Makefile）/ §6.2（httplib 版本与 CodeMirror 拆 5 文件）/ §6.2.1（新增 bcrypt 替代方案）/ §6.7（新增环境依赖与安装命令）/ §8（+R-20、R-7 标为已消除）/ §9（新增 T0.0、改写 T0.1/T0.2/T0.3/T3.3/新增 T0.6、AC 口径）/ §10.1（AC-38 改 4 并发、+AC-48/AC-49）**。

> **v1.6 特意没动的四处**，因为环境问题**不该回头改架构**：
> 1. **§3.2 DDL 逐字未改** —— 换构建方式、换 bcrypt 实现、换前端加载方式，都不改变持久化形态。`password_hash` 仍 255 宽、仍存 60 字符的 bcrypt 输出。
> 2. **§5 判题流水线未改** —— 判题逻辑与"跑在容器里还是宿主机上"完全无关；§2.1-A/B 两图的内部结构逐项一致。
> 3. **§6.3 API 契约未改** —— 前端改成 vendored 不影响任何端点；`queueAhead` 仍不落库。
> 4. **§2.3 的锁顺序与线程职责未改** —— 路线 A 只是让 mysqld 跑在同一台机器上，不引入新的并发问题。
>
> **唯一一处跨章节的定量约束**（也是本轮最容易被漏掉的）：**§1.4 的闸门 4 → §2.3 的 worker 2 → §5.4.2.1 的兜底墙钟余量 2.9 倍 → §10.1 的 AC-38 四并发压测**。这四个数字是一条链，动一个必须重算另外三个 —— 这正是 §8.1.1 说的"交叉一致性"问题，环境轮把它又演示了一遍。

**v1.7 的联动面**（**不新增决策，仍 75 项**）：纯依赖清单的事实性补正。改动仅 **§0（+v1.7 补正说明）/ §6.7（整章重写为「包清单 + 全部命令」，原 6 个子节与附录 A 合并为一处，+5 必需项、−3 不需要项）/ §9 T0.0（改为指向 §6.7）**。

> **v1.7 刻意没动的四处**：① **决策表** —— 这轮是"清单对不上现实"，不是"需要做新选择"，不占决策号；② **§3.2 DDL** —— 依赖装法与持久化形态无关；③ **§2.1-A/B 两张图** —— 路线划分不变，只是不再假设机器已配好；④ **Docker 相关内容** —— 路线 B 仍为可选，合并进 §6.7.5 后不受影响。

**v1.8 的联动面**（**不新增决策，仍 75 项**）：构建入口轮。`cmake` 由「⛔ 不装」移回「必装」，`CMakeLists.txt` 成为唯一构建规则，`Makefile` 降为薄封装。改动仅 **§0（版本表 v1.8 + 新增构建入口轮说明 + 标注推翻 v1.7 的第 5 行处置）/ §6.1（目录结构 +CMakeLists.txt、tests/CMakeLists.txt、两个测试脚本）/ §6.7.1（包清单 12→13 项，cmake 与 libgtest-dev 入列，「明确不装」删掉 cmake）/ §6.7.2（安装命令 +cmake +libgtest-dev +构建自检两行）/ §6.7.4（验证 6→8 项）/ §9（T0.0 / T0.1 改写 + 新增 T0.1.1、T0.2 备注、T0.4 进度备注）/ §12（本段）+ `dependence.md` §1、§4、§5**。

> **v1.8 刻意没动的六处**：① **决策表** —— `[D-71]` 的"宿主机原生构建"与 `[D-13]` 的 g++-12 都没变，只是构建规则的文件换了；② **§3.2 DDL**；③ **§5 判题流水线** —— judge spawn 的 `g++-12` 与本项目的 CMAKE 编译器互不相干；④ **§6.3 API 契约与错误码**；⑤ **§2.3 线程模型** —— 构建期多一个静态库不改变运行期任何并发关系；⑥ **页面与前端** —— `static/` 完全不参与编译。
>
> **一条需要记住的隐含关系**（与 §5.4.2.1、`§6.3.6` 同类）：`CMakeLists.txt` 里 `OJ_JUDGE_WORKERS`/`OJ_JUDGE_GATE` 的关系式校验依赖 `.env`，而 `.env` 改完**必须重启**才重新生效 —— 这正是 §6.3.1 那套「改配置 + 重启」哲学在构建期的一致体现。

**v1.9 的联动面**（**不新增决策，仍 75 项；修订 `[D-72]` 的版本口径**）：httplib 版本对齐轮。改动仅 **§0（版本表 + `[D-72]` 行 + 新增 v1.9 说明）/ §6.2（库表 cpp-httplib 行 + 尾注重写）/ §6.7.1（包清单 `libcpp-httplib-dev` → 自装单头文件，并移入「明确不装」）/ §6.7.2（安装命令去掉 apt 包、增加 `install` 一行）/ §6.7.4（验证项换成 `grep CPPHTTPLIB_VERSION`）/ §9（T0.0、T0.1.1 的 httplib 路径说明、T0.2 的镜像要求）/ §12（本段）+ `dependence.md` §1、§1.1、§5**。

> **v1.9 刻意没动的四处**：① **`#include <httplib.h>` 的写法** —— 换版本不改写法，这是选 header-only 单文件库的最大好处；② **`CPPHTTPLIB_OPENSSL_SUPPORT` 关闭**（§6.2，单机 HTTP 不引 OpenSSL 依赖）；③ **§5 判题流水线与 §3.2 DDL** —— httplib 只出现在 HTTP 层；④ **T0.4 的勾选状态** —— log/response/errors 未完成，`config` 部分做完也不勾选。

---