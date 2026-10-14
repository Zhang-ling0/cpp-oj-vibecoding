// =============================================================================
// CppOJ「账号与个人数据」业务层：改密 / 注销账号 / 收藏 / 统计 / 概览
// 规格来源：API.md §3.5 §3.6 §4.4 §6.1 §6.2（对应 SPEC §6.3.5 / §6.3.7 / §6.3 题目组
//           / §6.5 聚合写法与口径一致性契约）
//
// 【范围与既有模块一致】只回答"算什么、怎么算、要几条 SQL"，不含 HTTP 路由与统一包络
//   （common/response.h 独家负责），也不含 Cookie → 登录身份的解析（web/session.h 的
//   authenticate()）。与 admin.cpp 的分工：admin 那份是【全站】只读统计，这份是
//   【单个用户】的读写，两者的 solved/attempted 口径必须一致（§6.5 口径一致性契约），
//   故都写成"存在至少一条 AC"而不是"最后一次"。
//
// 【口径只有一处定义：§6.4 的 CASE 表达式的等价 EXISTS 写法】
//   solved = EXISTS(verdict='AC')；attempted = EXISTS(任意提交)。
//   列表/详情在 problem_api.cpp，本文件是第二处落地 —— 两处都必须遵守"至少一条"，
//   而不是"最后一次"（某次蒙对、后来改坏，不会把已通过的题降级）。
//
// 【聚合一律在 SQL 内完成】
//   [D-21] 串行化 DB 下每条语句是一次停顿，所以本文件没有"把 submissions 捞回应用层
//   再聚合"的写法；byTag 先用 SELECT DISTINCT problem_id 把集合收敛再 JOIN
//   （§6.5 的头号陷阱：problem_tags 是多对多，直接 JOIN + COUNT(*) 会把一道题数三次）。
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "web/auth_api.h"   // AuthFailure / validate_password（1001 的 field 口径唯一真源）

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// ---- POST /api/auth/password（§6.3.5）--------------------------------------------
//
// 【同事务的两条语句是硬要求】
//   UPDATE users SET password_hash=? 与 DELETE FROM sessions WHERE user_id=? AND token<>?
//   分开提交会出现"密码已改但旧会话还在"（可接受）或"会话已踢但密码没改"（不可接受）。
//   `token <> ?` 保住当前会话 —— 改密后仍应是登录态（D-40），删掉自己那行等于登出失败。
//
// keep_token 由路由层从 Cookie 解析后递进来（与 web/session.h::destroy_others 同一个口径）。
// 失败时 out_failure 给出 1001 + field=oldPassword / newPassword。
bool change_password(db::Session& session, unsigned long long user_id,
                     const std::string& keep_token, const std::string& old_password,
                     const std::string& new_password, AuthFailure* out_failure,
                     std::string* error);

// ---- DELETE /api/auth/account（§6.3.7，D-57 硬删除）------------------------------
//
// 【一条 DELETE 完成整条 CASCADE 链】：users → sessions / submissions / user_problems，
// submissions → submission_case_results / submission_compile_errors（全部 ON DELETE CASCADE）。
// 不动 problems / problem_cases / tags：题库是全局资源（§6.3.7 约束 2）。
bool delete_account(db::Session& session, unsigned long long user_id,
                    const std::string& password, AuthFailure* out_failure, std::string* error);

// ---- POST /api/problems/{slug}/favorite（§6.3 题目组）----------------------------
//
// 【语义是 toggle 而非置位】：调用一次翻转一次，响应回翻转【之后】的状态。
// 三种行状态（无行 / favorite=0 / favorite=1）用一次 SELECT + 一次写覆盖，读-改-写在
// 同一事务内 —— 两次并发 toggle 同一条只会翻转两次，这正是 toggle 的定义，不需要加锁。
enum class FavoriteStatus {
  kOk,         // → 0    200 {isFavorite}
  kNotFound,   // → 1004 404（slug 不存在）
  kError       // → 1008 500
};
bool toggle_favorite(db::Session& session, unsigned long long user_id, const std::string& slug,
                     bool* out_is_favorite, FavoriteStatus* out_status, std::string* error);

// ---- GET /api/stats（§6.1）--------------------------------------------------------
// 四个标量，一条 SQL 的标量子查询（串行化 DB 下四次往返就是四次停顿）。
struct UserStats {
  long long total = 0;       // 我的提交总数
  long long solved = 0;      // 已通过的【题数】（存在至少一条 AC）
  long long attempted = 0;   // 尝试过的题数（含只有 CE 的题）
  long long favorites = 0;   // 收藏的题数
};
bool load_user_stats(db::Session& session, unsigned long long user_id, UserStats* out,
                     std::string* error);

// ---- GET /api/me/overview（§6.2）-------------------------------------------------
//
// 【头号陷阱：按标签聚合会重复计数】—— problem_tags 是多对多，一道挂 3 个标签的题若先
// JOIN 标签表再 COUNT(*) 会被计三次。正确写法是先 SELECT DISTINCT problem_id 收敛成
// 集合再 JOIN（这里用 IN 子查询，等价且少一次往返）。byDifficulty 基表不 JOIN 标签表，
// 故 COUNT(*) 即可 —— 两者写法不同不是笔误。
struct DifficultyStat {
  std::string difficulty;   // easy / medium / hard（固定三条，缺的补 0）
  long long total = 0;
  long long solved = 0;
  long long attempted = 0;
};

struct TagStatRow {
  std::string name;
  std::string category;     // algorithm / source
  long long total = 0;
  long long solved = 0;
  long long attempted = 0;
};

struct RecentSubmissionRow {
  unsigned long long id = 0;
  std::string problem_slug;
  std::string problem_title;
  std::string verdict;      // 空 = NULL（还在排队）
  std::string created_at;
};

inline constexpr std::size_t kOverviewRecentLimit = 3;

struct UserOverview {
  long long solved = 0;
  long long total = 0;          // 题库题目总数（与用户无关，§6.5 的 total 口径）
  long long attempted = 0;
  long long submissions = 0;
  long long favorites = 0;
  std::vector<DifficultyStat> by_difficulty;   // 恒为 easy/medium/hard 三条，按此顺序
  std::vector<TagStatRow> by_tag;              // 按 total DESC, name 升序
  std::vector<RecentSubmissionRow> recent;     // 最近 3 条，按 id 倒序
};
bool load_user_overview(db::Session& session, unsigned long long user_id, UserOverview* out,
                        std::string* error);

// ---- 序列化（§6.3 的 data 块，camelCase；包络由 common/response.h 独家负责）--------
std::string to_json(const UserStats& stats);
std::string to_json(const UserOverview& overview);

}  // namespace web
}  // namespace oj