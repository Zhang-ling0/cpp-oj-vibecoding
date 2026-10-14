// =============================================================================
// CppOJ 提交组业务层：新建 / 列表 / 详情 / diff / 删除 / 重跑
// 规格来源：API.md §5.1–§5.6（对应 SPEC §6.3「提交」组 + §6.3.6）
//
// 【范围】与 user_api.h / problem_api.h 同一取舍：只回答"算什么、怎么算、要几条 SQL"，
//   不含 HTTP 路由与统一包络（common/response.h 独家负责），不含 Cookie → 登录身份的解析。
//
// 【归属校验写进 WHERE，不单独查再比】§5.2：SELECT … WHERE id=? AND user_id=? 无行时
//   本该回 1003，但"id 不存在"同样是这一行查询的结果 —— 两条错误码要分开报，
//   故失败路径上【再查一次是否存在于任何人名下】：存在→1003（他人的），不存在→1004。
//   成功路径只有一条查询，归属判定不依赖应用层比较。
//
// 【admin 不放宽】§5.2/§5.4/§5.5：他人提交对 admin 同样回 1003。
//
// 【judge 未接线时的口径】§5.1 的失败表里有 1009「判题服务不可用」，触发条件是
//   「judge 调度器未启动 / 队列满」。本模块把这一判定收敛到 judge_available() 一个函数
//   （目前恒 false，T2.7 接线后改为读调度器存活状态），路由层在建行【之前】问它一次。
//   ⚠️ 顺序很重要：先答 1009 再建行，否则判题器没起来时库里会留下一批永远 pending 的
//   死提交（前端会一直轮询下去）。
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "web/problem_api.h"   // QueryParseError / kMaxPageSize（分页护栏只有一份）

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// ---- judge 闸门（§5.1 的 1009）----------------------------------------------------
// 唯一真源。judge 调度器（T2.7）接线前恒 false —— 此时 POST /api/submissions 与
// POST /api/submissions/{id}/retry 都返回 1009，且【不落任何行】。
bool judge_available();

// ---- 列表（§5.3）------------------------------------------------------------------
struct SubmissionQuery {
  std::string problem_slug;             // 空 = 不按题过滤
  std::string verdict;                  // 空 = 不按结论过滤；取值 AC/WA/TLE/MLE/RE/CE/JE
  int page = 1;                         // 1-based，纯数字
  int size = 20;
};
// 枚举值非法 / 非纯数字 → false 并写 error（1001 + data.field）；未知参数名忽略、
// 同名参数最后一个生效（§1.5 的通用规则）。
bool parse_submission_query(const std::string& raw_query, SubmissionQuery* out,
                            QueryParseError* error);

struct SubmissionListItem {
  unsigned long long id = 0;
  std::string problem_slug;
  std::string problem_title;
  std::string status;        // pending / judging / done / failed
  std::string verdict;       // 空 = NULL（还没出结论）
  std::string message;       // 空 = NULL
  bool has_case_passed = false;  long long case_passed = 0;
  bool has_case_total = false;   long long case_total = 0;
  bool has_total_ms = false;     long long total_ms = 0;
  bool has_max_mem_kb = false;   long long max_mem_kb = 0;
  std::string created_at;
};

struct SubmissionList {
  std::vector<SubmissionListItem> list;   // 按 id 倒序（默认时间倒序，§5.3）
  long long total = 0;                    // 过滤后的总条数（不含分页）
  int page = 1;
  int size = 20;
};
bool load_submissions(db::Session& session, unsigned long long user_id,
                      const SubmissionQuery& query, SubmissionList* out, std::string* error);

// ---- 详情（§5.2）-------------------------------------------------------------------
struct CaseResult {
  int case_idx = 0;
  std::string verdict;        // AC / WA / TLE / MLE / RE
  bool passed = false;
  std::string args_text;
  std::string output_text;
  std::string expected_text;
  bool has_time_ms = false;   long long time_ms = 0;
  bool has_mem_kb = false;    long long mem_kb = 0;
  std::string error_msg;
};

struct CompileDiagnostic {
  std::string file;     // user.cpp / harness / link（[D-64] 的归属分流，库里的原值）
  int line = 0;
  int col = 0;
  std::string severity; // error / warning / note
  std::string message;
};

struct SubmissionDetail {
  unsigned long long id = 0;
  std::string problem_slug;
  std::string problem_title;
  std::string status;
  std::string verdict;    // 空 = NULL
  std::string message;    // 空 = NULL
  bool has_case_passed = false;  long long case_passed = 0;
  bool has_case_total = false;   long long case_total = 0;
  bool has_total_ms = false;     long long total_ms = 0;
  bool has_max_mem_kb = false;   long long max_mem_kb = 0;
  std::string code;
  std::string compiler_output;   // 空 = NULL
  std::vector<CompileDiagnostic> compile_errors;   // 按 seq 升序
  std::vector<CaseResult> case_results;            // 按 case_idx 升序
  std::string created_at;
  bool can_retry = false;         // status='failed' 且 verdict='JE'（[D-49]）
};

// 三种结局：本人 → 0；他人的 → 1003；id 不存在 → 1004。DB 异常走 error（→ 1008）。
enum class LookupStatus { kFound, kNotOwned, kNotFound };
bool load_submission_detail(db::Session& session, unsigned long long user_id,
                            unsigned long long id, SubmissionDetail* out, LookupStatus* status,
                            std::string* error);

// ---- diff（§5.4）--------------------------------------------------------------------
struct DiffSide {
  unsigned long long id = 0;
  std::string created_at;
  std::string code;
};
// from == to 在解析阶段就被 1001 挡掉（见 §5.4 失败表），故本函数不再判重。
bool load_submission_diff(db::Session& session, unsigned long long user_id, unsigned long long from_id,
                          unsigned long long to_id, DiffSide* out_from, DiffSide* out_to,
                          LookupStatus* status, std::string* error);

// ---- 删除（§5.5）--------------------------------------------------------------------
// 只 DELETE 本人的那一条；明细与诊断由外键 CASCADE 连带删除，无需显式 DELETE。
bool delete_submission(db::Session& session, unsigned long long user_id, unsigned long long id,
                       LookupStatus* status, std::string* error);

// ---- 新建与重跑（§5.1 / §5.6）----------------------------------------------------------
struct CreatedSubmission {
  unsigned long long submission_id = 0;
  long long queue_ahead = 0;   // 你前面还有几个 pending（不落库，§5.1）
};

// POST /api/submissions：slug 不存在 → 1004。code 长度由调用方（路由层）先按
// OJ_MAX_CODE_BYTES 校验后递进来（1001 + field=code），故本函数不再重复判长度。
//
// create=false 时【只校验 slug 不建行】—— judge 未接线时路由层传这个值，于是 1004
// 今天仍可验收，而不会在判题器没起来时留下一批永远 pending 的死提交。
bool create_submission(db::Session& session, unsigned long long user_id, const std::string& slug,
                       const std::string& language, const std::string& code, bool create,
                       CreatedSubmission* out, LookupStatus* status, std::string* error);

// POST /api/submissions/{id}/retry：status='failed' 且 verdict='JE' 才允许，**新增一条**
//   记录而不是覆盖原记录（§5.6；覆盖会破坏"提交历史不可变"）。不满足 → *retryable=false，
//   路由层据此回 1011。
//
// create=false 时【只做前置校验不建行】—— judge 未接线（judge_available()==false）时路由层
// 传这个值，于是 1003 / 1004 / 1011 三种失败今天仍然可验收，而不会在判题器没起来时
// 留下一批永远 pending 的死提交（前端会一直轮询下去）。只读不改库，故可以放在租约内。
bool retry_submission(db::Session& session, unsigned long long user_id, unsigned long long id,
                      bool create, unsigned long long* out_new_id, LookupStatus* status,
                      bool* retryable, std::string* error);

// ---- 序列化（§5.1–§5.4 的 data 块，camelCase）-----------------------------------------
std::string to_json(const SubmissionList& list);
std::string to_json(const SubmissionDetail& detail);
std::string to_json(const DiffSide& from, const DiffSide& to);
std::string to_json(const CreatedSubmission& created);

}  // namespace web
}  // namespace oj