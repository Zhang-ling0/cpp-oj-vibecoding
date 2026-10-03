# SPEC — CppOJ：面向求职面试的在线判题系统

| 项 | 值 |
|---|---|
| 版本 | v1.1（补齐前端页面 + 注册与权限能力） |
| 日期 | 2026-10-02 |
| 状态 | 规格已确认，进入实施 |
| 交付周期 | 3 周 |

---

## 0. 决策溯源

本文档由 36 项访谈决策收敛而成。每条标注 `[D-xx]` 便于回溯与验收对照。

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
| D-11 | 资源限制：贴近 LeetCode 严格值 | D-29 | 错误码：数字业务码 + 同步 HTTP 状态码 |
| D-12 | 度量：CPU 时间 + ru_maxrss | D-30 | 题库文件为源，启动时同步入库 |
| D-13 | 工具链：g++ 12 / C++17 / -O2 -Wall | D-31 | 提交历史：代码全文 + 逐用例结果全存 |
| D-14 | 题面：Markdown + 前端渲染 | D-32 | 前端：多页应用 |
| D-15 | 分类：难度 + 算法标签 + 来源标签 | D-33 | 容器：mysql + oj 双容器 |
| D-16 | 进度：由提交历史自动推导 | D-34 | 日志：应用日志 + judge 日志双文件 |
| D-17 | 时间：存 UTC + 前端本地化 | D-35 | 历史页：全局页 + 题内最近 5 次 |
| D-18 | 存储：MySQL | D-36 | 页面：login / list / problem / history 四页 |

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

**非目标（首版明确不做）**：多语言执行、竞赛/排行榜、实时推送、题解社区、支付、管理后台、移动端原生适配、真沙箱安全隔离。

### 1.3 核心用户流程

| 流程 | 描述 |
|---|---|
| P1 注册登录 | 用户名 + 密码注册，登录后获得 HttpOnly Cookie，会话有效期 7 天 |
| P2 找题 | 首页列表：按关键词、难度、算法标签、来源标签、我的进度（未开始/尝试过/已通过）筛选与排序 |
| P3 刷题 | 打开题目页，左侧 Markdown 题面（含示例、约束、提示），右侧编辑器（预置模板）+ 提交按钮 |
| P4 提交判题 | Ctrl/Cmd+Enter 提交 → 立即返回 submissionId → 前端每 1.5s 轮询 → 进度条/状态实时刷新 |
| P5 看结果 | 状态徽标（通过/失败/超时/内存超限/运行错误/编译错误）+ 可折叠的逐用例表格（编号/输入/你的输出/期望/耗时/内存） |
| P6 复盘 | 提交历史页全局检索；题目页底部显示该题最近 5 次提交，可展开看代码全文 |
| P7 收藏 | 题目页右上角星标切换收藏，列表页可按"仅看收藏"筛选 |
                     
### 1.4 非功能需求

| 维度 | 要求 |
|---|---|
| 性能 | 页面加载 < 1s；提交到拿到结果 P95 < 8s（10 用例以内）；列表查询 P95 < 200ms |
| 并发 | 支持 10 个用户同时提交；全局同时评测数硬上限 8；单用户同时评测数上限 2 |
| 可靠性 | 服务重启后不出现永久"评测中"的任务；judge 进程崩溃不拖垮主进程 |
| 安全 | 用户代码不可读文件系统（除自身工作目录）、不可无限 fork、不可超限占用内存/CPU/磁盘；所有 SQL 走预处理语句 |
| 可观测 | 应用日志与 judge 日志分文件、按天切割、保留 7 天 |
| 成本 | 单机 2 核 4G 可稳定运行，无额外中间件（无 Redis / MQ） |
| 兼容性 | 桌面 Chrome/Edge/Safari 最近两个大版本；1280px 以上宽度 |

---

## 2. 架构

### 2.1 系统总览

```
┌──────────────────────────────────────────────────────────────────────┐
│  Docker Compose (单机)                                                 │
│                                                                        │
│  ┌──────────────────────── oj 容器 ────────────────────────────────┐  │
│  │                                                                  │  │
│  │  ┌──────────────┐        ┌───────────────────────────────────┐  │  │
│  │  │ HTTP 线程池   │        │  Judge 调度器 (1 个调度线程)       │  │  │
│  │  │ cpp-httplib  │        │   - 内存 FIFO 队列                  │  │  │
│  │  │  (N=8)       │───────▶│   - 启动时恢复 pending 任务         │  │  │
│  │  │  - 静态文件   │ 入队   │   - 全局并发闸门(8) / 用户闸门(2)  │  │  │
│  │  │  - REST API  │        └────────────┬──────────────────────┘  │  │
│  │  │  - Session   │                     │ 分发                      │  │
│  │  └──────┬───────┘                     ▼                          │  │
│  │         │                  ┌──────────────────────────────────┐  │  │
│  │    ┌────▼─────┐            │  Judge Worker × K (K=4)         │  │  │
│  │    │   DAO    │            │   1. 取题 + 读用例（内存缓存）   │  │
│  │    │ 互斥量    │            │   2. 生成 solution_decl.h       │  │  │
│  │    └────┬─────┘            │      + tester_util.h            │  │  │
│  │         │                  │      + tester.cpp                │  │  │
│  │         │                  │   3. g++ -c user.cpp -o user.o  │  │  │
│  │         │                  │      g++ user.o tester.cpp -o run│ │
│  │         │                  │   4. 解析编译器输出 → CE?        │  │  │
│  │         │                  │   5. 逐用例 fork+setrlimit 执行   │  │  │
│  │         │                  │   6. 写回 DB(逐用例行) + 汇总列    │  │  │
│  └─────────┼──────────────────┴──────────────┬───────────────────┘  │  │
│            │                                 │                      │
│    ┌───────▼────────┐              ┌─────────▼──────────────────┐  │
│    │ 题库加载器      │              │ 工作区 /tmp/oj-judge/<id>/ │  │  │
│    │ problems/*.json│              │  user.cpp / user.o          │  │  │
│    │ 启动时 upsert  │              │  *.h / tester.cpp / run     │  │  │
│    │ + 灌内存缓存   │              └────────────────────────────┘  │  │
│    └───────┬────────┘                      完成后清理              │
│  ──────────┼─────────────────────────────────────────────────────   │
│            │  mysql://oj:oj@mysql:3306/oj                             │
│    ┌───────▼────────────────────────────────────────────────────┐    │
│    │  mysql 容器  (8.0)   volume: oj-mysql-data                  │    │
│    │  /docker-entrypoint-initdb.d/01-schema.sql   首次自动建表   │    │
│    └────────────────────────────────────────────────────────────┘    │
│  volume: oj-logs → /var/log/oj/{app,judge}.log                      │
└──────────────────────────────────────────────────────────────────────┘
```

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
Judge Worker × 4: 阻塞在条件变量上，被唤醒后独占执行一个提交的全流程
```

**锁顺序**：`DB mutex` 是唯一全局锁，仅在 DAO 调用期间持有，**绝不在持有 DB 锁时等待 judge**。判题流程不碰 DB 锁（除最终写回），避免长事务阻塞 HTTP 请求。

**为什么不加大 worker 数**：judge 会 fork 子进程，CPU 密集；4 个 worker × 最多 8 个孙进程在 2 核机器上已接近饱和。全局闸门 8 保证机器不会被自己的评测打满导致误判 TLE。

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
  case_passed     INT UNSIGNED NULL,               -- 受控反范式：= COUNT(passed=1) 于 submission_case_results
  case_total      INT UNSIGNED NULL,               -- 受控反范式：= COUNT(*) 于 submission_case_results
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
| `submissions.case_total` | `COUNT(*)` | 同上 | 同上 |
| `submissions.total_ms` | `COALESCE(SUM(time_ms),0)` | 避免列表页 N+1 聚合 | 同上 |
| `submissions.max_mem_kb` | `COALESCE(MAX(mem_kb),0)` | 同上 | 同上 |
| `submission_case_results` 内的文本快照 | `problem_cases.args_json` | **不可改为外键引用** —— 题库可改，历史必须可回放（D-31） | 只写不改，随 submission 生命周期级联删除 |

> **聚合必须用 `COALESCE(... , 0)`，不能直接用 `SUM`/`MAX`**。TLE/MLE 用例的子进程被 `SIGKILL`，harness 来不及输出该用例的 `@@OJ@@` 行，因此它的 `time_ms` / `mem_kb` 是 NULL。`SUM` 遇 NULL 会跳过，`MAX` 也会跳过 —— 这正是期望行为（被杀的用例不该贡献耗时）。但若一次提交**所有**用例都被杀，裸 `SUM` 返回 NULL 而非 0，与列的 `NOT NULL` 语义冲突。故约定统一以 `COALESCE(...,0)` 收口，写回方与校验方用同一表达式。

**落库后的一致性校验**：`tests/run_tests.sh` 增加一条断言 —— 对每条 `status='done'` 的提交，

```sql
SELECT COUNT(*) AS violations FROM (
  SELECT s.id
  FROM submissions s
  LEFT JOIN submission_case_results r ON r.submission_id = s.id
  WHERE s.status = 'done'
  GROUP BY s.id, s.case_passed, s.case_total, s.total_ms, s.max_mem_kb
  HAVING s.case_passed <> SUM(CASE WHEN r.passed = 1 THEN 1 ELSE 0 END)
      OR s.case_total  <> COUNT(r.id)
      OR s.total_ms    <> COALESCE(SUM(r.time_ms), 0)
      OR s.max_mem_kb  <> COALESCE(MAX(r.mem_kb), 0)
) t;   -- 必须返回 0
```

> 四个汇总列必须一起写进 `GROUP BY`（依赖 `s.id` 的函数依赖），否则 MySQL 8 的
> `ONLY_FULL_GROUP_BY` 报 `Unknown column 's.case_passed' in 'having clause'`。
> 用 `LEFT JOIN` 而非 `JOIN`：`status='done'` 但明细行全丢（写回事务异常）时，
> `COUNT(r.id)` 得 0、`SUM` 得 NULL，才能被这条断言抓出来。

上表任何一条派生规则被改坏、或写回漏了同事务，都会被这条断言抓住。

**仍保留为原子文本、不拆表**：

- `submissions.compiler_output` — 编译器输出的原始全文，是一份不可分的产物，非结构化多值，拆行无收益（`submission_compile_errors` 只是它的结构化旁路）。
- `problem_cases.args_json` / `expected_json` — 单条用例的**固有复合参数**（一次调用传多个实参），不是"重复组"。这一列的原子性边界止于"一条用例"。

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
| `slug` `title` `difficulty` `statement_md` `signature` `method_name` `return_type` `code_template` `time_limit_ms` `mem_limit_kb` `harness_prelude` `is_featured` `sort_no` | `problems.*` 同名列 | 一题一行 |
| `tags.algorithm[]` / `tags.source[]` | `tags` + `problem_tags` | 按 `category` 分别落 tag 行 |
| `params[i].name` / `.cpp_type` | `problem_params` | 每形参一行，`param_idx = i` |
| `cases[i].args` / `.expected` / `.brief` | `problem_cases.args_json` / `.expected_json` / `.brief` | 每用例一行，`case_idx = i` |
| — | `problems.case_count` | 同步完成后回填为该题用例行数 |

> `harness_prelude` 是**可选**字段，缺省为空串。用到 `__expr` 逃生舱时填所需的前置声明/头文件，如 `"#include \"../common/treedef.h\"\nusing namespace oj;"`（见 §4.2）。
> 注意：上面的 `statement_md` 为节省篇幅做了缩写（值里有 `...`），因此**整块并非可直接粘贴的合法 JSON**。录入时把 `statement_md` 换成完整的单行转义字符串（换写作 `\n`），其余字段可直接照抄。`cases` / `params` 的结构则是字面准确的。

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

### 4.3 题目录入工作流 `[D-30]`

```
1. 新建 problems/<slug>.json，按 §4.1 填写
2. 本地起服务 → 启动时自动同步到 MySQL（problem_cases / problem_params 按 §4.1 映射表展开）
3. 打开该题页面提交一次，用官方/自写解法验证 12 个用例全绿
4. 全绿则录入完成；否则按 judge 日志 /var/log/oj/judge.log 修正 JSON
5. 若改动了用例内容/数量，重启服务让 DB 与内存缓存刷新
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

| 限制 | 值 | 超限表现 |
|---|---|---|
| `RLIMIT_CPU` | `time_limit_ms / 1000 + 1` 秒 | `SIGXCPU` → 进程终止 → 判为 **TLE** |
| `RLIMIT_AS` | `mem_limit_kb * 1024` 字节 | `bad_alloc` / 启动失败 → **MLE** |
| `RLIMIT_FSIZE` | 1 MB | 写盘超限 → **MLE** |
| `RLIMIT_NOFILE` | 64 | 打开文件超限 → **RE** |
| `RLIMIT_CORE` | 0 | 禁止 core dump |
| 墙钟保险阀 | `time_limit_ms * 2 + 1000` ms | 平台侧 `SIGKILL` 整个进程组 → **TLE** |

**为什么需要墙钟保险阀**：`RLIMIT_CPU` 只统计 CPU 时间，用户代码若阻塞在 `sleep(999)` 或无返回的 `read()` 不会被 CPU 限制触发。保险阀兜住这类情况。

**进程组隔离**：`fork()` 后在子进程中 `setpgid(0, 0)`，超时时 `kill(-pgid, SIGKILL)` 杀整组，防止用户代码 fork 出孙进程后逃逸超时判定。

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

**文件路径归一化**：把绝对路径前缀 `/tmp/oj-judge/<id>/` 剥掉，只保留 `user.cpp`，避免向用户暴露服务器目录结构。归一化后的值直接落 `file_name` 列。

**解析失败降级**：正则未命中的行原样保留在 `compiler_output`，`submission_compile_errors` 只装成功解析的部分，不影响功能。

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

> **为什么无需清理明细行、也不会撞唯一键**：`status` 的迁移与明细行插入在**同一事务**内（T2.11），InnoDB 保证全提交或全回滚。因此可以证明：`status ∈ {pending, judging}` 的 submission **恒有 0 条明细行**。恢复逻辑只需 UPDATE `submissions` 本身，而重新评测时 `uk_sub_case` 上不可能存在冲突行。
> 这条推论是 T2.11 单事务写回的直接副产品 —— **若哪天把写回改成分批提交，必须同时在本节补上明细行清理逻辑**，否则会出现"判题中"的提交带着上一次重放的残留结果。

### 5.8 配额与限流 `[D-22]`

| 规则 | 阈值 | 超限响应 |
|---|---|---|
| 单用户同时评测中（pending+judging） | 2 | 拒绝提交，`code=1007 提交过于频繁，请等待当前评测完成` |
| 全局同时评测中 | 8 | 任务**入队但不启动**（排队等待，不拒绝） |
| 单用户每分钟提交数 | 10 | 拒绝提交，`code=1006` |
| 单次提交代码长度 | 64 KB | 前端拦截 + 后端二次校验 |
| 用户名长度 / 密码长度 | 3–32 / 6–64 | 参数校验失败 |

限流计数用**内存滑动窗口**（每用户 deque 存最近 60s 的提交时间戳），不落库。

---

## 6. 后端模块设计

### 6.1 目录结构

```
/home/ZL/project/cpp-oj-vibcoding/
├── SPEC.md
├── docker-compose.yml
├── Dockerfile
├── db/
│   └── init/01-schema.sql            # 挂载到 /docker-entrypoint-initdb.d
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
│   │                  runner.h       # fork + setrlimit 执行
│   │                  compiler.h     # g++ 调用 + 输出解析
│   └── web/            routes.h  session.h  ratelimit.h
├── src/                （对应 .cpp）
├── static/
│   ├── login.html  list.html  problem.html  history.html
│   ├── css/  app.css  problem.css
│   └── js/   api.js  auth.js  list.js  problem.js  history.js
│           editor.js  markdown.js  poll.js  common.js
├── logs/               # volume 挂载点
└── tests/
    ├── run_tests.sh                 # 端到端验收脚本
    └── problems/                    # 验收用特制题目
```

### 6.2 第三方依赖（全部需在 Dockerfile 中安装）

| 库 | 用途 | 说明 |
|---|---|---|
| cpp-httplib (header-only) | HTTP 服务 | `CPPHTTPLIB_OPENSSL_SUPPORT` 关闭，单机 HTTP |
| MySQL C Connector | 数据库 | 官方 `libmysqlclient-dev` |
| nlohmann/json | JSON 解析 | header-only |
| bcrypt (`libbcrypt`) | 密码哈希 | 或用 OpenSSL `EVP` 自行实现 Blowfish |
| cpp-httplib OpenSSL | HTTPS | **首版关闭**，nginx 反代时再启用 |
| marked.js (CDN) | Markdown 渲染 | 前端 |
| DOMPurify (CDN) | XSS 过滤 | 前端，配合 marked |
| CodeMirror 5 (CDN) | 代码编辑器 | 前端 |

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
| 1003 | 403 | 无权限 | 访问他人提交详情 |
| 1004 | 404 | 资源不存在 | 题目/提交 ID 不存在 |
| 1005 | 409 | 冲突 | 用户名已注册 |
| 1006 | 429 | 请求过于频繁 | 每分钟提交数超限 |
| 1007 | 429 | 配额超限 | 单用户同时评测数超限 |
| 1008 | 500 | 服务器内部错误 | DB 异常、未捕获异常 |
| 1009 | 503 | 判题服务不可用 | judge 调度器未启动/队列满 |
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
| POST | `/api/auth/register` | 否 | `{username, password}` → `201 {userId, username}` |
| POST | `/api/auth/login` | 否 | `{username, password}` → `Set-Cookie: sid=...; HttpOnly; SameSite=Lax; Path=/` + `{userId, username}` |
| POST | `/api/auth/logout` | 否 | 删除会话行，清 Cookie |
| GET | `/api/auth/me` | 是 | `{userId, username}`；未登录返回 401 |

**题目**

| Method | Path | Auth | 说明 |
|---|---|---|---|
| GET | `/api/tags` | 否 | `{algorithm:[...], source:[...]}` |
| GET | `/api/problems` | 否 | query: `keyword` `difficulty` `tags`(逗号分隔) `progress`(none/attempted/solved) `favorite` `sort`(default/featured/easiest/hardest) `page` `size` → `{list:[...], total, page, size}` |
| GET | `/api/problems/{slug}` | 否 | `{...题目详情, tags, isFavorite, myProgress, recentSubmissions:[最近5条]}` |
| POST | `/api/problems/{slug}/favorite` | 是 | 切换收藏 → `{isFavorite: bool}` |

**提交**

| Method | Path | Auth | 说明 |
|---|---|---|---|
| POST | `/api/submissions` | 是 | `{problemSlug, language, code}` → `201 {submissionId, status:"pending"}` |
| GET | `/api/submissions/{id}` | 是（仅本人） | `{id, status, verdict, message, caseResults, compileErrors, compilerOutput, casePassed, caseTotal, totalMs, maxMemKb, code, createdAt}` |
| GET | `/api/submissions` | 是（仅本人） | query: `problemSlug` `verdict` `page` `size` → `{list:[...], total, ...}` |

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
| GET | `/api/stats` | 是 | `{total, solved, attempted, favorites}` |

**静态资源**：`GET /*` → `static/` 目录。`/api/*` 之外的路径若文件不存在 → 返回 `404.html` 或重定向到 `list.html`。

**CORS**：前后端**同源部署**（同一 nginx/httplib 服务），无需 CORS 配置。开发期若前端单独起 http-server，需在 httplib 侧开启 `set_cors` 允许 `http://localhost:5500`。

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

---

## 7. 前端设计

### 7.1 页面结构 `[D-32, D-36]`

| 页面 | 路径 | 内容 |
|---|---|---|
| 登录/注册 | `/login.html` | 切换式表单，登录成功跳 `list.html` |
| 题库列表 | `/list.html` | 筛选栏（关键词/难度/标签/进度/收藏）+ 题目表格 + 分页 |
| 题目详情 | `/problem.html?slug=xxx` | 左右分栏工作台 |
| 提交历史 | `/history.html` | 全局历史 + 筛选 + 展开详情 |

页面间用普通 `<a>` 链接跳转。每个页面独立引入所需 JS 模块，无构建步骤、无框架。

### 7.2 题目详情页布局 `[D-26]`

```
┌─────────────────────────────────────────────────────────────────┐
│ ← 返回   两数之和  [中等]  ⭐收藏          提交记录 | 题面 | 提示  │  ← 顶栏
├──────────────────────────┬──────────────────────────────────────┤
│                          │  [ C++ ▾ ]  [重置]  [提交 Ctrl+↵]    │  ← 工具栏
│  Markdown 题面            ├──────────────────────────────────────┤
│  (marked + DOMPurify      │                                      │
│   渲染)                   │        CodeMirror 5                  │
│                          │        C++ 语法高亮                   │
│  ### 题目描述             │        行号 / Tab 缩进                │
│  ### 示例 1               │        (localStorage 自动保存)        │
│      输入: ...            │                                      │
│      输出: ...            │                                      │
│  ### 约束                 │                                      │
│  ### 提示 (点击展开)      │                                      │
│                          │                                      │
│  ── 你的最近提交 ──       ├──────────────────────────────────────┤
│  10:24  通过  3ms        │  ⣾ 评测中…   (每 1.5s 轮询)          │
│  10:02  答案错误 [展开]   │                                      │
├──────────────────────────┴──────────────────────────────────────┤
│ 结果面板（可折叠，占详情页下半屏）                                  │
│  状态: ❌ 答案错误   通过 2/3   总耗时 7ms   峰值内存 2.0MB          │
│  ┌──┬────────┬──────────┬──────────┬─────┬──────┐                │
│  │# │ 输入    │ 你的输出  │ 期望输出  │耗时 │内存  │                │
│  ├──┼────────┼──────────┼──────────┼─────┼──────┤                │
│  │0 │[2,7..] │[0,1]     │[0,1]     │ 3ms │2.0MB │                │
│  │1 │[3,2..] │[1,0]     │[1,2]     │ 2ms │2.0MB │ ❌            │
│  │2 │[3,3..] │ 超出时间限制 (3000ms)              │ ❌            │
│  └──┴────────┴──────────┴──────────┴─────┴──────┘                │
└─────────────────────────────────────────────────────────────────┘
```

左右分栏比例可拖拽调节（`mousedown` 改 `flex-basis`，比例存 localStorage）。

### 7.3 关键交互

| 交互 | 实现 |
|---|---|
| 草稿保存 `[D-27]` | CodeMirror `change` 事件 debounce 800ms → `localStorage["draft:"+slug]`。切题/刷新/关闭浏览器均保留；登录不涉及 |
| 提交 `[D-28]` | `Ctrl/Cmd+Enter` 或点按钮；提交前校验：trim 后非空、≤64KB，否则前端提示不发请求 |
| 轮询 `[D-10]` | 提交后每 1.5s `GET /api/submissions/{id}`；`status` 变 `done`/`failed` 时停止并渲染结果。**组件卸载时清除定时器**；页面停留超过 5 分钟无结果则提示"评测耗时较长，可稍后在历史页查看" |
| CodeMirror 设置 | `mode: text/x-c++src`, `lineNumbers: true`, `indentUnit: 4`, `tabSize: 4`, `indentWithTabs: false`, `matchBrackets: true`, `viewportMargin: 30` |
| 编译错误高亮 | `compileErrors` 中 line 命中编辑器行号 → `cm.addLineClass(line, "background", "cm-error-line")`；悬停显示 message |
| Markdown 渲染 | `marked.parse()` 后经 `DOMPurify.sanitize()`，白名单允许 `pre/code/h1~h6/table/ul/ol/li/strong/em/img/a` |
| 时间显示 `[D-17]` | 接口返回 UTC ISO8601；前端 `new Date()` 转本地，先显示相对时间（"刚刚"/"3 分钟前"/"昨天 15:24"），`title` 属性放完整本地时间 |
| 401 处理 | 任意接口返回 1002 → 全局跳转 `/login.html?redirect=<当前路径>` |

### 7.4 空态与错误态

| 场景 | 表现 |
|---|---|
| 题库列表无结果 | "没有匹配的题目，试试放宽筛选条件" + 清除筛选按钮 |
| 无提交历史 | "还没有提交记录" + 去刷题按钮 |
| 未登录访问需鉴权页 | 跳转登录页并带 redirect |
| 提交后 1.5s 轮询遇 503 | 弹 toast"判题服务繁忙，评测可能延迟"，继续轮询 |
| 网络错误 | toast + 3 次指数退避重试 |

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
| **R-7** | **CDN 依赖不可达**（CodeMirror/marked 加载失败） | 编辑器不可用 | 中 | 启动时检测；`editor.js` 内置 `<textarea>` 降级路径，保证核心链路（写码→提交→看结果）不被阻断 |
| **R-8** | **MySQL 首次初始化失败**（initdb 脚本仅首次执行） | 容器起来但无表 | 中 | 服务启动时检测表是否存在，不存在则**幂等执行** schema（`CREATE TABLE IF NOT EXISTS`）作为兜底 |
| **R-9** | **严格资源限制误杀**（LeetCode 同级限制在本机偏紧） | 用户频繁 TLE | 中 | 题目 JSON 逐题可配 `time_limit_ms`/`mem_limit_kb`，默认取 LeetCode 同级值，录入时按需放宽 |

### 降级触发规则

```
若 M2 结束时 R-2 未达标（生成器仅支持白名单类型）
  → 冻结功能范围，题库录入只用白名单类型 + __expr，不再扩展生成器
若 M3 结束时题库 < 20 道
  → 砍掉 M4 的历史页筛选功能，优先保证题目录入完成
若整体进度落后 > 3 天
  → 砍：收藏功能、题内最近提交、来源标签筛选（保留核心闭环）
```

---

## 9. TODO 清单

### M0 — 地基（Day 1–2）

- [ ] T0.1 目录骨架 + `docker-compose.yml`（mysql + oj 双容器，volumes，healthcheck）
- [ ] T0.2 `Dockerfile`：基于 `ubuntu:22.04`，装 `g++-12`、MySQL Connector、cmake、bash
- [ ] T0.3 `db/init/01-schema.sql`（§3.2 全量 DDL，11 张表，注意外键依赖顺序：`problems`/`users`/`tags` 须先建）
- [ ] T0.4 `common/`：`config.h`（环境变量读取）、`log.h`（双文件按天切割 logger）、`response.h`（响应包络）、`errors.h`（错误码枚举）
- [ ] T0.5 httplib 起服务，静态文件映射跑通 `list.html`
- [ ] **验收**：`docker compose up` 后浏览器访问 `localhost:8080` 看到页面；`/var/log/oj/app.log` 正常输出

### M1 — 题库与生成器（Day 3–6）★ 最高风险

- [ ] T1.1 `nlohmann/json` 引入 + `problems/two-sum.json` 首个样例题
- [ ] T1.2 `problem/loader.h`：读目录 → 校验必填字段 → 同步 MySQL（幂等，按 slug）
  - `cases[]` / `params[]` 数组按 §4.1 映射表**展开为多行**写入 `problem_cases` / `problem_params`
  - 同步用 delete-then-insert 单事务语义，保证 JSON 删用例能传播（不留幽灵行）
  - 同步完成后同事务回填 `problems.case_count`
  - 全部入库后灌入进程内只读缓存（结构体含 statement/signature/params/cases），供 judge worker 使用
- [ ] T1.3 `judge/generator.h` 核心：JSON 值 → C++ 字面量转换器（含字符串转义）
- [ ] T1.4 类型推导：从 `problem_params.cpp_type` 推导数组嵌套层数
- [ ] T1.5 生成 `solution_decl.h` / `tester_util.h` / `tester.cpp`
- [ ] T1.6 `__expr` 逃生舱 + `problems.harness_prelude` 支持
- [ ] T1.7 单元测试：`tests/gen_test.cpp` 覆盖 int/string/vector/vector2d/嵌套/转义/空数组
- [ ] **验收**：给定 two-sum.json，生成的 tester.cpp 手写答案后能编译通过并输出 `@@OJ@@` 协议行
- [ ] **验收**：改 JSON 删掉第 3 个用例后重启，`problem_cases` 只剩 2 行且 `case_count=2`（验证同步语义）

### M2 — 判题引擎（Day 7–10）

- [ ] T2.1 `judge/compiler.h`：`alarm` 超时 + 两阶段编译 + 输出捕获
- [ ] T2.2 g++ 输出正则解析 → `submission_compile_errors` 行（`seq` 保序），路径归一化
- [ ] T2.3 `judge/runner.h`：fork + `setpgid` + setrlimit 五项 + stdin 重定向
- [ ] T2.4 `wait4` 回收 + 墙钟保险阀（`SIGKILL` 进程组）+ 退出码→verdict 映射
- [ ] T2.5 `@@OJ@@` stderr 协议解析 → `submission_case_results` 行
- [ ] T2.6 结果比较器：trim、数组空白归一、浮点容差
- [ ] T2.7 `judge/queue.h` + `scheduler.h`：内存 FIFO + 4 worker 条件变量 + 并发闸门
- [ ] T2.8 启动恢复逻辑（§5.7 的两条 SQL）
- [ ] T2.9 工作目录生命周期：创建/清理/启动时扫残
- [ ] T2.10 judge 日志：`/var/log/oj/judge.log` 记录生成代码、编译命令、进程退出原因
- [ ] T2.11 写回事务：`UPDATE submissions`(四个汇总列 + status) + `INSERT` 用例行 + `INSERT` 诊断行必须同事务；失败则整条 submission 置 `failed`/`JE`
  - `status` 迁移与明细行必须同事务 —— §5.7 的恢复逻辑依赖此不变量（证明见该节注释）
- [ ] **验收**：特制题目覆盖 6 种 verdict（AC/WA/TLE/MLE/RE/CE），逐个通过

### M3 — API 层（Day 11–13）

- [ ] T3.1 `db/mysql_pool.h` + 互斥量串行化；所有 DAO 用预处理语句（**禁止字符串拼接 SQL**）
- [ ] T3.2 DAO：`user` `problem` `problem_case` `problem_param` `tag` `submission` `submission_case_result` `submission_compile_error` 八组
- [ ] T3.3 认证：bcrypt 注册/校验 + 64 hex token 会话 + HttpOnly Cookie
- [ ] T3.4 限流器：每用户 60s 滑动窗口 + 同时评测数配额
- [ ] T3.5 路由：§6.3 全部端点 + 统一包络 + 错误码映射
- [ ] T3.6 列表查询：多条件筛选 + 分页 + JOIN 进度与收藏
- [ ] T3.7 提交端点：校验 → 落库 pending → 入队 → 返回 201
- [ ] T3.8 提交详情端点：仅本人可见 + 按 §6.3 的三步组装 `caseResults` / `compileErrors`
  - `submission_case_results` 按 `case_idx ASC`；`submission_compile_errors` 按 `seq ASC`
  - 字段名映射为 API 契约里的 camelCase，**前端零改动**
- [ ] T3.9 启动顺序：DB 就绪 → 校验/建表 → 加载题库（同步入库 + 灌缓存）→ 启动 judge → listen
- [ ] **验收**：`curl` 走通注册→登录→列表→提交→轮询→历史全链路

### M4 — 前端（Day 14–17）

- [ ] T4.1 骨架：`app.css` 设计系统（色板/间距/按钮/表格/徽标）+ 顶栏 + 通用 `api.js` `common.js`
- [ ] T4.2 `login.html`：登录/注册切换、表单校验、redirect 跳转
- [ ] T4.3 `list.html`：筛选栏 + 表格 + 分页 + 空态
- [ ] T4.4 `problem.html` 左右分栏：可拖拽、比例持久化
- [ ] T4.5 CodeMirror 集成：模板预置、草稿 debounce 保存、Ctrl+Enter 提交
- [ ] T4.6 Markdown 渲染 + DOMPurify + 提示折叠
- [ ] T4.7 提交 + 轮询 + 状态徽标 + 结果面板
- [ ] T4.8 逐用例表格 + 编译错误行高亮 + 错误面板
- [ ] T4.9 收藏切换 + 收藏筛选
- [ ] T4.10 `history.html`：全局列表 + 筛选 + 展开详情
- [ ] T4.11 题内最近 5 次提交
- [ ] T4.12 时间本地化 + 相对时间
- [ ] T4.13 空态/错误态/401 跳转/CDN 降级路径
- [ ] **验收**：§1.2 成功标准链路端到端手动走通

### M5 — 题库录入与打磨（Day 18–21）

- [ ] T5.1 录入 20 道覆盖高频题型的题（数组/哈希/双指针/链表/树/二分/动态规划/贪心）
- [ ] T5.2 补齐到 50 道（视进度）
- [ ] T5.3 `tests/run_tests.sh` 自动化验收脚本
  - **派生列一致性断言**（§3.4 的维护契约）：对每条 `status='done'` 的提交，用 §3.4 给出的
    `HAVING` 查询校验 `case_passed` / `case_total` / `total_ms` / `max_mem_kb` 四个汇总列，
    返回行数必须为 0。**必须包含至少一条含 TLE 或 MLE 用例的提交** —— 那类用例的
    `time_ms`/`mem_kb` 为 NULL，是 `COALESCE` 约定唯一能被真正验证的路径。
  - **无残留多值断言**：`submissions` 表不存在 `case_results` / `compile_errors` 列
- [ ] T5.4 压力测试：10 并发提交 × 20 轮，验证无崩溃、无永久 pending
- [ ] T5.5 恶意输入测试：死循环 / 内存炸弹 / 超长输出 / fork bomb / 空代码
- [ ] T5.6 README：架构说明、启动方式、题库编写指南

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
架构图（§2）→ 数据模型（§3）→ 题库 Schema（§4）→ 流水线（§5）→ API 契约（§6.3）→ 前端交互（§7）→ 验收标准（§10）。
