-- =============================================================================
-- CppOJ 清库重建辅助脚本
-- 配套：db/init/01-schema.sql
-- 对应：改了 §3.2 DDL 之后、或需要彻底清空数据时手工执行
--
-- 【为什么放在 db/ 而不是 db/init/】
--   路线 B 会把 db/init/ 整个目录挂到 /docker-entrypoint-initdb.d，而 MySQL 镜像
--   会【按文件名字母序执行该目录下所有 .sql】。若本文件放在 init/ 下，名字 99 排在
--   01 之后，首次启动就会「建完 11 张表再全部删掉」，得到一个空库。
--   因此重置脚本必须留在 init/ 之外。
--
-- 【用法】
--   mysql -h 127.0.0.1 -uoj -poj oj < db/reset.sql
--   mysql -h 127.0.0.1 -uoj -poj oj < db/init/01-schema.sql
--
-- 【不写死库名】同 01-schema.sql：由调用方用 -D / 连接串指定目标库。
--
-- 【为什么「反序删除」+「关外键检查」双保险】
--   反序保证单看这条 SQL 就合法；FOREIGN_KEY_CHECKS=0 保证即便有人调整了顺序，
--   也不会在中间某张表被引用时中途失败、留下删了一半的残局。
--
-- 【注意】这是不可逆操作。执行前确认 oj 库里没有需要保留的数据
--         （提交历史、收藏、账号全部会消失）。
-- =============================================================================

SET NAMES utf8mb4;

SET FOREIGN_KEY_CHECKS = 0;

-- 反依赖序：先删子表，再删被引用的父表
DROP TABLE IF EXISTS submission_compile_errors;   -- → submissions
DROP TABLE IF EXISTS submission_case_results;     -- → submissions
DROP TABLE IF EXISTS submissions;                 -- → users, problems
DROP TABLE IF EXISTS user_problems;               -- → users, problems
DROP TABLE IF EXISTS problem_tags;                -- → problems, tags
DROP TABLE IF EXISTS problem_params;              -- → problems
DROP TABLE IF EXISTS problem_cases;               -- → problems
DROP TABLE IF EXISTS problems;                    -- 被 problem_*/user_problems/submissions 引用
DROP TABLE IF EXISTS tags;                        -- → problem_tags
DROP TABLE IF EXISTS sessions;                    -- → users
DROP TABLE IF EXISTS users;                       -- 被 sessions/user_problems/submissions 引用

SET FOREIGN_KEY_CHECKS = 1;

-- 注：problem_cases / problem_params / problem_tags 会被删，但它们的
--     [JSON 记忆] 在 problems/<slug>.json 里 —— 题库的唯一真源是文件（D-30），
--     重建后由题库加载器（problem/loader.h）重新 upsert 回来，见 SPEC §4.1。
--     也就是说：清库重建设题库不需要重新录题，只需重启服务。
