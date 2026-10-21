# CppOJ —— 面向求职面试的 C++ 在线判题系统

| 项 | 值 |
|---|---|
| 定位 | 个人**单机**的算法面试备战工具（SPEC `[D-01]`），**不是**通用 OJ |
| 形态 | 单进程 C++17 服务（cpp-httplib）+ 宿主机 MySQL 8；无 Redis / MQ / 反向代理 |
| 前端 | 原生 HTML/CSS/JS 多页应用，无框架、无构建步骤，前端库全部 vendored（断网可用） |
| 规模 | 后端 28 个翻译单元（`ojcore` 27 + 入口）· 11 张表 · 26 道题 · **417 条 gtest** · **147 条端到端断言** |
| 环境 | Ubuntu 24.04 · g++-12 · 2 核 / 2GB 起（功能验证；性能验收需 4GB，见 SPEC §1.4） |
| 状态 | 功能闭环可用：判题引擎、管理台写侧、限流配额均已接线（`question.md` 阶段 0–2 已验收） |

> 规格真源 [SPEC.md](SPEC.md)（v1.9，75 项决策）· 接口契约 [API.md](API.md) · **部署 [DEPLOY.md](DEPLOY.md)**
> 依赖细节 [dependence.md](dependence.md) · 审计与待办 [question.md](question.md)

---

## 1. 它解决什么问题

算法面试备战时，通用 OJ 要么太重（部署与学习成本高），要么反馈太粗（只告诉你"错了"，
不告诉你**错在第几个用例、输入是什么、期望与实际差在哪**）。CppOJ 只做一件事：
把「定位错误边界」这件事做得比别处省力。

三条核心价值（SPEC §1.1）：

1. **逐用例全量反馈** —— 输入 / 你的输出 / 期望 / 耗时 / 内存一张表看全
2. **复盘支撑** —— 提交历史存代码全文 + 逐用例快照（改题也能回放）、localStorage 草稿、
   两次提交可对比（后端端点已就绪，见 §9 待办）、按薄弱标签推荐下一题
3. **架构可讲述** —— fork 隔离原理、题库 → harness 代码生成器、线程模型，
   都是可以写进面试的素材

刻意**不做**（SPEC §1.2 非目标）：多语言执行、竞赛/排行榜、题解社区、支付、
找回密码、题面图片（树/图用字符画）、账号软删除（注销即硬删）。

---

## 2. 快速开始

前置：依赖装齐（一次性，见 [DEPLOY.md](DEPLOY.md) §2–§5）。

```bash
make build        # 编译 ./build/oj 与全部测试（内部固定 -j2）
make run          # 未起 mysqld 则先拉起，再从仓库根启动服务
# 浏览器打开 http://127.0.0.1:8080/
```

- **停止**：前台 `Ctrl-C`（SIGINT/SIGTERM 是唯一的干净退出路径）；后台 `pkill -x oj`
- **必须从仓库根启动**：`./problems`、`./static`、`./logs`、`db/init/01-schema.sql`
  全是相对当前工作目录的路径（`make run` 已保证）
- 首次启动会**幂等建表**（11 张）并把 `problems/*.json` 同步入库 —— 空库也能直接跑

---

## 3. 功能一览

### 3.1 刷题主链路

| 环节 | 能力 |
|---|---|
| 找题 | 关键词 / 难度 / 算法标签 / 来源标签 / 进度（未开始·尝试过·已通过）/ 仅看收藏 筛选排序分页；首页顶部个人概览条 |
| 读题 | **未登录可读全部题面**；Markdown 题面 + 示例 + 约束 + **方向提示 tab**（免费无限看，26 题全部带提示） |
| 写码 | 左右分栏题目页；Ace 编辑器（C++ 高亮）+ 预置模板；localStorage 草稿自动保存，多标签页冲突先问不覆盖；`Ctrl/Cmd+Enter` 提交 |
| 判题 | 异步提交立即返回 `submissionId` + 队列位置（「你前面还有 N 个」）→ 前端 1.5s 轮询，刷新中可从 sessionStorage 恢复 |
| 看结果 | 7 种结论（AC/WA/TLE/MLE/RE/CE/JE）+ 逐用例表格；编译错误带 `user.cpp` 行高亮与 g++ 原文，harness 侧错误给「题库配置有误」专属文案 |
| 复盘 | 题目页底部「你的最近 5 次」；「我的」页三 tab：提交记录（筛选/展开/删除/重跑失败项）· 掌握分布（按难度/标签）· 账户设置（改密码踢其他会话、注销账号） |
| 续刷 | 题目页底部「下一题建议」：按最薄弱标签给推荐 + 可解释理由 |

### 3.2 账号与权限

| 能力 | 说明 |
|---|---|
| 注册 / 登录 | 用户名 3–32、密码 6–64；HttpOnly Cookie 会话 7 天；`OJ_ALLOW_REGISTER=0` 可一键关注册（1010） |
| 改密码 | 验旧密码 → 改密并**踢掉其他设备会话**，当前会话保留 `[D-40]` |
| 注销账号 | 输密码 + 二次确认（逐项列出将删数据）→ 硬删除，级联清会话/提交/收藏 `[D-56][D-57]` |
| 角色 | `admin` / `user` 两值；**真源是 `.env` 的 `OJ_ADMIN_USERNAMES`**，启动时双向同步（页面改角色会被下次启动覆盖）`[D-37][D-41]` |
| 限流配额 | 单用户并发评测 2（1007）、每分钟 10 次（1006）、单次代码 64KB；全局并发闸门 4，超了**入队不拒绝** |

### 3.3 管理台（`admin.html`，仅 admin）

四 tab 可写管理台（阶段 2 起，SPEC §6.3.3）：

| tab | 能力 |
|---|---|
| 概览 | 系统计数（用户/题目/用例/提交/队列/判题存活）+ 用户清单 |
| 题目 | 新建 / 编辑（写回 `problems/<slug>.json` + **单题同步入库，无需重启**）/ **下线归档**（不物理删题） |
| 提交 | 列表 / 删除 / **重判** |
| 用户 | 改角色 / 停用（不能改自己、不能停用自己 → 1011） |

---

## 4. 一次提交发生了什么

```
01 入队      INSERT status=pending → 201 {submissionId, queueAhead}，不阻塞请求
02 编译      g++-12 -c user.cpp → user.o；再链接题库生成的 tester.cpp（题库代码永不改动）
03 执行      每个用例一次 fork + setrlimit（CPU / 地址空间 / 文件大小），独立进程组
04 解析      harness 走 stderr 回报 `@@OJ@@<JSON>` 协议行；一条都没有 → JE（平台错误，不冤枉用户）
05 写回      单事务写入逐用例明细 + 汇总列；前端下一次轮询即可渲染表格
```

**资源限制**（每题可在题库 JSON 覆盖）：

| 限制 | 默认 | 超限结论 |
|---|---|---|
| CPU 时间 | 3000ms（`RLIMIT_CPU`） | TLE |
| 地址空间 | 128MB（`RLIMIT_AS`，**比 LeetCode 的 RSS 口径更严**，界面已标注） | MLE |
| 文件大小 / 打开文件数 | 1MB / 64 | MLE / RE |
| CPU 停滞检测（增量为 0 达 1.5s） | 平台侧 `SIGKILL` 进程组 | TLE（拦 `sleep`/阻塞读） |
| 兜底墙钟 | `时限 × 10 + 5000` ms，与并发闸门绑定（§5.4.2.1 有定量关系） | TLE |

> 内存**限制**是地址空间口径、界面**度量**列是峰值 RSS —— 两者不是一个量，UI 显式区分，
> 避免「报 MLE 但只有 30MB」被当成 bug（SPEC §5.5.1）。

---

## 5. 技术架构

### 5.1 选型

| 层 | 选型 | 备注 |
|---|---|---|
| 语言 / 工具链 | C++17 · g++-12（锁定） | 判题编译命令 `-std=c++17 -O2 -Wall` |
| HTTP | cpp-httplib **0.60.1**（header-only，`/usr/local/include/httplib.h`） | 明文 HTTP；线程池由 `OJ_HTTP_THREADS` 决定（默认 8） |
| JSON | nlohmann-json 3.11.3 | 题库解析 + 响应包络 |
| 存储 | MySQL 8 / libmysqlclient | 11 张表；应用启动时幂等 `CREATE TABLE IF NOT EXISTS` |
| 密码哈希 | OpenSSL Blowfish（自实现） `[D-75]` | 24.04 官方源无 `libbcrypt-dev` |
| 构建 | CMake（唯一构建规则）+ Makefile 薄封装 | `make build / run / test / clean` |
| 测试 | GTest 1.14（ctest）+ curl/Python 端到端 | 417 + 147 |
| 前端 | Ace 1.32.7 · marked 12.0.2 · DOMPurify 3.1.6，**全部 vendored** `[D-73]` | 运行时零外网请求 |
| 判题隔离 | fork + setrlimit + 进程组 + CPU 停滞检测 `[D-04][D-62]` | 单机可信用户；**不防逃逸**（见 §9） |

### 5.2 进程与线程模型

```
主线程          : 配置 → 日志 → 连接池 → 建表 → 题库同步 → 扫残 → 会话回收
                  → 角色同步 → judge 调度器 → httplib listen（阻塞）
HTTP 线程 × 8   : REST + 静态资源 + Session；DB 访问经全局 mutex 串行化
调度线程 × 1    : 启动恢复（judging→failed/JE、pending 重新入队）→ 内存 FIFO 队列
                  → 全局闸门 4 / 单用户闸门 2
Judge Worker × 2: 取题（进程内缓存）→ 生成 3 个 harness 文件 → 两阶段编译
                  → 逐用例 fork → 单事务写回 DB
```

**锁顺序**：`DB mutex` 是唯一全局锁，判题流程不持有它等待 —— HTTP 请求不会被一次评测卡住。

### 5.3 两条部署路线

| | 路线 A（宿主机原生） | 路线 B（Docker Compose） |
|---|---|---|
| 地位 | **当前形态**，本仓库已跑通 | 目标形态，`Dockerfile` / `docker-compose.yml` 目前是空占位（T0.2 未开工） |
| 差别 | 仅「谁负责拉起 mysqld 与 g++」 | 判题流水线 / API / 数据模型两者**完全相同** |

---

## 6. 页面与目录

### 6.1 页面（六页）

| 文件 | 作用 | 访问 |
|---|---|---|
| `index.html` | 落地页 / 门面（`/` 直接可达） | 公开 |
| `login.html` | 注册 / 登录（也承载注销后的提示） | 公开 |
| `list.html` | 题库列表 + 顶部概览条（兼首页）`[D-38]` | 公开可读 |
| `problem.html` | 题目页：题面 / 提示 / 编辑器 / 结果 / 最近提交 / 下一题 | 公开可读，提交需登录 |
| `history.html` | 「我的」：提交记录 / 掌握分布 / 账户设置 三 tab `[D-39]` | 登录 |
| `admin.html` | 管理台四 tab | `role=admin` |

### 6.2 目录结构（关键部分）

```
├── SPEC.md / API.md / DEPLOY.md / dependence.md / question.md   ← 文档
├── CMakeLists.txt          # 唯一构建规则（v1.8）
├── Makefile                # 薄封装：make build/run/test/clean（JOBS 默认 2）
├── .env                    # 运行期配置（角色名单 / MySQL / 并发闸门）
├── db/
│   ├── init/01-schema.sql   # 11 张表 DDL：应用启动幂等执行（路线 A 主方案）
│   └── reset.sql            # 手工清库（⚠️ 不可逆，故意不放进 init/）
├── problems/*.json         # 题库唯一真源（26 题），启动时同步入库
├── include/ + src/         # common/ db/ model/ problem/ judge/ web/
├── tests/                  # gtest 单测 + run_tests.sh(curl) + python/ + 同步验收脚本
├── static/                 # 六个 html + css/ + js/ + vendor/（Ace/marked/DOMPurify）
├── tools/reset_db.cpp      # oj-reset：清库 + 造测试数据（不含伪造判题结果）
└── logs/                   # app.log / judge.log（按天切割，保留 7 天，内容不入库）
```

---

## 7. 配置速览

完整说明与全部键见 [DEPLOY.md](DEPLOY.md) §6；`.env` 放在仓库根，**进程环境变量 > `.env` > 内置默认值**。

| 变量 | 默认 | 一句话 |
|---|---|---|
| `OJ_LISTEN` | `0.0.0.0:8080` | 监听地址 |
| `MYSQL_HOST/PORT/USER/PASSWORD/DB` | `127.0.0.1:3306` / `oj` / `oj` / `oj` | 数据库连接 |
| `OJ_JUDGE_WORKERS` / `OJ_JUDGE_GATE` | `2` / `4` | **闸门必须 = 2 × workers**，否则拒绝启动 |
| `OJ_JUDGE_USER_GATE` | `2` | 单用户同时评测数上限（超限 1007） |
| `OJ_FENCE_MULTIPLIER` | `10` | 兜底墙钟系数；**改闸门前必须重算**（§5.4.2.1） |
| `OJ_ALLOW_REGISTER` | `1` | `0` = 关闭注册（1010） |
| `OJ_ADMIN_USERNAMES` | 未设置 | 管理员名单，**角色唯一真源**；未设置 = 不动 role，显式留空 = 全员降权 |
| `OJ_HTTP_THREADS` | `8` | HTTP 线程池（运行期生效） |
| `OJ_MAX_CODE_BYTES` / `OJ_RATE_*` | `65536` / `60s·10次` | 代码长度与提交频率配额 |

**启用管理员的固定顺序**：先注册账号 → 把用户名写进 `OJ_ADMIN_USERNAMES` → 重启服务
（名单里的名字必须已存在，不会自动创建）。

---

## 8. 测试与验收

> 所有命令**从仓库根执行**；ctest 已固定工作目录为仓库根。

| 套件 | 命令 | 前提 | 规模 |
|---|---|---|---|
| 单元测试 | `make test` 或 `tests/run_unit_tests.sh [名字过滤]` | 已构建 | **417 条**（本机实测 100% 通过，1 条按环境跳过，52s） |
| 端到端 HTTP | `tests/run_tests.sh` | 服务在跑 + `jq` + `mysql` 客户端 | **147 条断言**（阶段 2 验收全绿） |
| 端到端 Python | `python3 tests/python/run_api_tests.py` | 服务在跑 + `pip3 install -r tests/python/requirements.txt` | 同口径、断言更严 |
| 题库同步 | `tests/run_problemsync_test.sh` | 会自拉 18080 独立实例、临时改 `problems/` | 增 / 改 / 写坏 / 删 四条路径 |
| 浏览器级 | [web自动化测试文档.md](web自动化测试文档.md) · [web自动化测试文档-paywright.md](web自动化测试文档-paywright.md) | 手工 / Playwright | 页面级回归手册 |
| 接口文档示例 | [基于curl的接口自动化测试文档.md](基于curl的接口自动化测试文档.md) | — | 逐端点 curl 断言 |

**关于并发**（SPEC §0.0 硬约束，与机器绑定）：日常 `make build`（内部 `-j2`）、
sanitizer 构建 `-j1` + `ASAN_OPTIONS=detect_leaks=0`、**ctest 不加 `-j`**。
卡住的不是 CPU 是内存 —— 详见 [DEPLOY.md](DEPLOY.md) §1 的症状对照表。

---

## 9. 已知边界与当前待办

**诚实声明的边界**

| 边界 | 说明 |
|---|---|
| 不防恶意逃逸 `[D-04]` | fork + setrlimit 防资源失控，**不防**同机用户读文件。定位个人单机；开放给不可信用户需换 Docker/nsjail（SPEC 风险 R-1） |
| 内存口径分裂 `[D-63]` | 限制 = `RLIMIT_AS`（虚拟地址空间），度量 = `ru_maxrss`（RSS）；界面分别标注，不并列成一个数字 |
| 改题需重启 | 手工编辑 `problems/*.json` 后需重启（启动时同步）；管理台写侧**单题同步、免重启** |
| 只有 HTTP | 首版不开 TLS；启用 HTTPS 须同步做三件事（SPEC §6.3.2.1） |
| 单机性能口径 | 2G 机器只保证**功能**正确；NF-1~NF-4、AC-38 等性能验收必须在 4G 机器跑 |

**当前未完成**（详见 [question.md](question.md) §6）

- **阶段 3 收尾**：`MALLOC_ARENA_MAX` 与 HTTP 线程默认值下调、受保护页缓存头、
  死文件清理（`static/js/diff.js` 0 字节、`static/vendor/cm/` 已被 Ace 取代）、全量浏览器回归
- **前端并排 diff 未实现**：后端 `GET /api/submissions/diff` 已就绪，`diff.js` 仍为空文件
  （`[D-42][D-50]` 的 UI 欠账，落地页文案目前超前于实现）
- **路线 B（Docker）**：`Dockerfile` / `docker-compose.yml` 为空占位，T0.2 未开工
- **`static/vendor/LICENSES/`** 未落盘（三份 MIT LICENSE）

---

## 10. 文档地图

| 文档 | 回答什么 |
|---|---|
| [README.md](README.md)（本文） | 这是什么、有什么功能、怎么跑、怎么测 |
| [DEPLOY.md](DEPLOY.md) | **从空白 Ubuntu 到可用服务**的逐条命令、配置、排障、运维 |
| [SPEC.md](SPEC.md) | 完整规格：75 项决策溯源、架构、DDL、判题流水线、API、验收标准 |
| [API.md](API.md) | 接口契约 + 实现状态矩阵（含错误码表与 curl 示例） |
| [dependence.md](dependence.md) | 依赖清单与安装（DEPLOY §2 的同源简版） |
| [question.md](question.md) | 对抗性审计：问题清单、四阶段修复方案与进度 |
| web自动化测试文档*.md / 基于curl的接口自动化测试文档.md | 浏览器级 / 接口级回归手册与踩坑记录 |
