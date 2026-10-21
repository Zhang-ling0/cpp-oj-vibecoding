-- =============================================================================
-- CppOJ 数据库初始化脚本
-- 唯一真源：SPEC.md §3.2 DDL（11 张表 / 19 个索引 / 11 条外键）
-- 对应任务：SPEC.md §9 M0 T0.3
--
-- 【不写死库名】本脚本不含 CREATE DATABASE / USE —— 两条部署路线各自保证：
--   路线 A（宿主机原生，D-71）：应用启动时幂等执行本文件，应用已连上 oj 库
--   路线 B（Docker）：        db/init/ 挂载到 /docker-entrypoint-initdb.d，
--                            MySQL 镜像用 MYSQL_DATABASE 建库后在该库中执行本文件
--
-- 【幂等】全部使用 CREATE TABLE IF NOT EXISTS，可重复执行。
--         需要清表重建时用配套的 db/reset.sql（注意：它【不能】放在 init/ 下，
--         否则 MySQL 镜像会按字母序在 01 之后把它执行掉，等于首启建表又删表）。
--
-- 【建表顺序不可重排】存在外键依赖，必须按下述拓扑创建：
--   users ──┬─→ sessions
--           ├─→ user_problems ──┐
--           └─→ submissions ←──┴── problems ──┬─→ problem_cases
--                                                 ├─→ problem_params
--   tags ──────→ problem_tags ──────────────────┘
--   problems ───→ submissions ──┬─→ submission_case_results
--                                └─→ submission_compile_errors
--
-- 【CASCADE 链】11 条外键全部 ON DELETE CASCADE，构成两级链：
--   users → submissions → {submission_case_results, submission_compile_errors}
--   users → sessions / user_problems
--   problems → problem_cases / problem_params / problem_tags / user_problems / submissions
--   tags → problem_tags
--   账号注销（D-57）依赖这条链用【一条】DELETE FROM users 完成，因此任何一张
--   子表若被改成 RESTRICT，注销就会直接失败 —— 见 SPEC R-12 / AC-33。
-- =============================================================================

SET NAMES utf8mb4;

-- -----------------------------------------------------------------------------
-- 1/11  users —— 用户与角色
--        role 的真源是 .env 的 OJ_ADMIN_USERNAMES（D-37），服务启动时同步；
--        本列只是同步结果，不是配置来源。
--        username 的唯一性只由下方 UNIQUE KEY uk_username 声明 —— 不要在列上再写
--        内联 UNIQUE，否则 MySQL 会建出两个等价的唯一索引（实测踩过）。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  username      VARCHAR(32)  NOT NULL,   -- 唯一性只由下方 UNIQUE KEY uk_username 声明；列上再写内联 UNIQUE 会建出两个等价唯一索引
  password_hash VARCHAR(255) NOT NULL,           -- bcrypt, cost=10
  role          ENUM('user','admin') NOT NULL DEFAULT 'user',  -- D-37 真源为 OJ_ADMIN_USERNAMES，启动时同步，见 §6.3.1
  disabled      TINYINT(1)   NOT NULL DEFAULT 0, -- 阶段 2：管理员停用（1 = 禁止登录，登录时拒绝并已清会话）
  created_at    DATETIME     NOT NULL,
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 2/11  sessions —— 会话
--        idx_expire 专为 §6.3.2 的惰性回收语句设计：
--          DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP();
--        必须是 UTC_TIMESTAMP() 而非 NOW()（容器时区非 UTC 时会误删未过期会话）。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS sessions (
  token      CHAR(64)     NOT NULL PRIMARY KEY,  -- 32 字节随机数的 hex
  user_id    BIGINT UNSIGNED NOT NULL,
  expires_at DATETIME     NOT NULL,
  created_at DATETIME     NOT NULL,
  KEY idx_user (user_id),
  KEY idx_expire (expires_at),
  CONSTRAINT fk_session_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 3/11  tags —— 标签（算法主题 / 来源与面试频率）
--        判据：只有当「按角色维度查询」成为真实需求时才拆 roles 关联表（SPEC §3.4）。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS tags (
  id       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name     VARCHAR(32) NOT NULL,
  category ENUM('algorithm','source') NOT NULL,  -- 算法主题 / 来源与面试频率
  UNIQUE KEY uk_name_cat (name, category)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 4/11  problems —— 题目主表（题库 JSON 的落库结果）
--        case_count 是受控反范式：= COUNT(problem_cases)，由题库加载器在同一事务内刷新。
--        CE（编译错误）提交也要显示「N 个用例未执行」，故 submissions.case_total
--        用 GREATEST(COUNT(明细行), 本列) —— 见 §5.6.3。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS problems (
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
  archived      TINYINT(1)   NOT NULL DEFAULT 0, -- 阶段 2：下线归档（1 = 列表/详情/提交入口不可见；不删行，保住用户历史）
  is_featured   TINYINT(1)   NOT NULL DEFAULT 0, -- 精选置顶
  sort_no       INT          NOT NULL DEFAULT 0,
  created_at    DATETIME     NOT NULL,
  updated_at    DATETIME     NOT NULL,
  KEY idx_diff (difficulty), KEY idx_featured (is_featured, sort_no)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 5/11  problem_cases —— 题目侧用例
--        题库 JSON 的 cases 数组按题内出现顺序展开为多行，不以 JSON 文本落库（1NF）。
--        单条用例内部的 args 仍可为数组 —— 那是该用例的固有复合参数，不是重复组。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS problem_cases (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  problem_id    INT UNSIGNED NOT NULL,
  case_idx      INT UNSIGNED NOT NULL,           -- 题内 0-based 序号，与 @@OJ@@ 的 idx 对齐
  brief         VARCHAR(255) NULL,               -- 如"基础用例"，展示在结果表
  args_json     MEDIUMTEXT   NOT NULL,           -- [[2,7,11,15], 9] 或 [{"__expr":"buildTree(...)"}]
  expected_json MEDIUMTEXT   NOT NULL,
  UNIQUE KEY uk_problem_case (problem_id, case_idx),
  CONSTRAINT fk_pc_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 6/11  problem_params —— 题目侧形参
--        元素类型推导规则（§4.2）依赖 cpp_type 列，故必须结构化才能被查询/校验。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS problem_params (
  id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  problem_id INT UNSIGNED NOT NULL,
  param_idx  INT UNSIGNED NOT NULL,              -- 形参序号，决定调用顺序
  name       VARCHAR(64)  NOT NULL,              -- 如 nums
  cpp_type   VARCHAR(128) NOT NULL,              -- 如 std::vector<int>&
  UNIQUE KEY uk_problem_param (problem_id, param_idx),
  CONSTRAINT fk_pp_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 7/11  problem_tags —— 题目标签关联
--        双向都要有索引：列表页按标签筛题（走 idx_tag），
--        §6.5 的 byTag 聚合也要从标签侧出发。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS problem_tags (
  problem_id INT UNSIGNED NOT NULL,
  tag_id     INT UNSIGNED NOT NULL,
  PRIMARY KEY (problem_id, tag_id),
  KEY idx_tag (tag_id),
  CONSTRAINT fk_pt_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,
  CONSTRAINT fk_pt_tag     FOREIGN KEY (tag_id)     REFERENCES tags(id)     ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 8/11  user_problems —— 收藏关系
--        【没有 progress 列】：进度由提交历史实时 JOIN 推导（§6.4），
--        存在派生列会引入刷新义务且可能与实时聚合不一致（SPEC §3.5）。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS user_problems (
  user_id    BIGINT UNSIGNED NOT NULL,
  problem_id INT UNSIGNED    NOT NULL,
  favorite   TINYINT(1) NOT NULL DEFAULT 0,
  updated_at DATETIME    NOT NULL,
  PRIMARY KEY (user_id, problem_id),
  KEY idx_fav (user_id, favorite),
  CONSTRAINT fk_up_user    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
  CONSTRAINT fk_up_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- -----------------------------------------------------------------------------
-- 9/11  submissions —— 提交主表
--        四个受控反范式汇总列与明细行必须【同一事务】写入（§3.4）：
--          case_passed = COUNT(passed=1)
--          case_total  = GREATEST(COUNT(明细行), problems.case_count)   ← §5.6.3 修正
--          total_ms    = COALESCE(SUM(time_ms), 0)   TLE/MLE 用例度量为 NULL，不计
--          max_mem_kb  = COALESCE(MAX(mem_kb), 0)
--        聚合一律用 COALESCE(...,0) 收口：若某次提交所有用例都被杀，裸 SUM 返回
--        NULL 而非 0，与列语义冲突。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS submissions (
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

-- -----------------------------------------------------------------------------
-- 10/11 submission_case_results —— 提交侧逐用例结果
--        【不外键引用 problem_cases】：题库是活的（改 JSON 即改题），而提交历史要求
--        可回放（D-31）。若用外键，改题删用例会让历史详情凭空少行。
--        因此 case_idx 与三个文本列一律【快照】，有意承担数据重复换历史完整性。
--        前端拿到的 caseResults 由本表按 case_idx 升序组装。
--        time_ms / mem_kb 在 TLE/MLE 时为 NULL：子进程被 SIGKILL，harness
--        来不及输出该用例的 @@OJ@@ 行（不是 0）。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS submission_case_results (
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

-- -----------------------------------------------------------------------------
-- 11/11 submission_compile_errors —— 提交侧编译诊断
--        未被正则命中的行不进本表，原样留在 submissions.compiler_output。
--        file_name 归一化后只有三种取值（D-64）：
--          user.cpp  → 用户代码，编辑器行号高亮
--          harness   → 平台生成的 tester.cpp / *.h，归属题库，不提供行高亮
--          link      → 链接器 undefined reference，归属签名不匹配，不提供行高亮
--        seq 保证前端展示顺序与编译器输出一致。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS submission_compile_errors (
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

-- =============================================================================
-- 派生列一致性断言（T5.3 / §3.4 v1.5 版）
-- 建表后应返回 0 行 violations：
--
--   SELECT COUNT(*) AS violations FROM (
--     SELECT s.id
--     FROM submissions s
--     JOIN problems p ON p.id = s.problem_id
--     LEFT JOIN submission_case_results r ON r.submission_id = s.id
--     WHERE s.status IN ('done','failed')
--     GROUP BY s.id, s.case_passed, s.case_total, s.total_ms, s.max_mem_kb
--     HAVING s.case_passed <> COALESCE(SUM(CASE WHEN r.passed = 1 THEN 1 ELSE 0 END), 0)
--         OR s.case_total  <> GREATEST(COUNT(r.id), p.case_count)
--         OR s.total_ms    <> COALESCE(SUM(r.time_ms), 0)
--         OR s.max_mem_kb  <> COALESCE(MAX(r.mem_kb), 0)
--   ) t;
--
-- 另：不变量「status ∈ {pending, judging, failed} 恒有 0 条明细行」（§5.7 D-67），
-- 可用下面这条随时自检：
--
--   SELECT COUNT(*) AS violations FROM submissions s
--     JOIN submission_case_results r ON r.submission_id = s.id
--    WHERE s.status IN ('pending','judging','failed');   -- 必须为 0
-- =============================================================================
