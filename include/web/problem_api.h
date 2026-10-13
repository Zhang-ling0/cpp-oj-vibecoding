// =============================================================================
// CppOJ 题目列表 / 详情 API 的业务层
// 对应任务：SPEC.md §9 M3 T3.6「列表查询：多条件筛选 + 分页 + JOIN 进度与收藏」
//           + T3.17「下一题建议 nextRecommendation」[D-45] [D-59] [D-68]
//           + T3.19 的【读侧】（hint_md 由题库加载器落库，本模块只负责下发）
// 规格来源：§6.3 端点清单（题目组三个端点）/ §6.3 错误码 1001、1004
//           / §6.4 进度自动推导 / §6.5 聚合写法与口径一致性契约
//           / §6.6 + §6.6.1 推荐规则与主标签选取 / §1.4「所有 SQL 走预处理语句」
//           / [D-43] 未登录可读全部题面 / [D-47] hint_md 空即不渲染提示 tab
//           / [D-61] 排序与分页 / §6.4 的收藏 LEFT JOIN 一次取回（避免 N+1）
//
// 【范围：与 T3.12 的 admin.h 同一取舍 —— 这一层只负责"算什么、怎么算、要几条 SQL"】
//   不含 HTTP 路由与统一包络（T3.5 + common/response.h，均未开工）、
//   不含 Cookie → 登录用户的解析（T3.3）。因此三个端点的入口都接受
//   "外部递进来的登录上下文"（ViewerContext*，nullptr = 未登录），
//   单测因此能在不碰 Cookie、不起 HTTP 的前提下覆盖全部 SQL 与口径。
//
// 【为什么不把这些 SQL 放进 db/dao.h】
//   dao.h 尚空（T3.2 未开工）。T3.12 选择了"SQL 随业务层走"而不是"先造一个空 DAO 骨架"，
//   本模块沿用同一取舍：写出来的语句此刻就能被单测跑到真库上验证，
//   而放进一个还没开工的 dao.h 只会得到一个无人调用的壳。T3.2 落地时应把这里的
//   聚合语句收敛进 dao 的 problem 组，届时 dao 才是进程里唯一发 SQL 的地方。
//
// 【未登录怎么处理：user_id = 0 这一个值承担全部"匿名"语义】
//   §6.3 的三个端点 Auth 全是「否」（[D-43]：未登录可读全部题面）。做法不是写两套 SQL，
//   而是让匿名用户以 `user_id = 0` 参与查询：
//     · users.id 是 AUTO_INCREMENT，0 不可能是任何真实用户的 id
//       （MySQL 对自增列的 0 有特殊语义，插不进 0），故所有 `user_id = 0` 的
//       EXISTS 子查询恒为假 → my_progress 恒为 'none'
//     · user_problems 的 LEFT JOIN 恒不命中 → isFavorite 恒为 false
//   收益是"登录/未登录"两个分支**在 SQL 文本层面完全相同**，只剩一个参数不同：
//   少一半的语句模板，就没有"两套模板各自演化"的漂移面。
//   ⚠️ 反过来，依赖"匿名"的【过滤条件】（progress≠none、favorite=1）在匿名时
//     【不做静默忽略、也不返回空列表，而是 1001】：见 parse_list_query 的说明 ——
//     静默返回一个空列表，会让用户以为"题库里没有这类题"，那是假信息。
//
// 【进度口径只有一处定义：§6.4 的 CASE 表达式】
//   §6.4、§6.5、§6.3.3（admin 的 solved/attempted）说的是同一件事的三种粒度。
//   本模块按 §6.4 原文实现，并在 src 里由一个函数拼出该表达式供列表/详情/推荐共用 ——
//   同一份 CASE 写三遍，AC-11 与 AC-22 迟早会有一处对不上。
//
// 【推荐的"可解释"是内生的，不是拼出来的】
//   §6.6.1 `[D-68]`：主标签 = 该题 algorithm 标签中【用户通过率最低】者，并列取 tags.id 最小。
//   §6.6 `[D-59]`：理由文案必须与主标签同源。于是本模块的做法是
//   「先选出主标签，再用【这个标签的通过率】拼理由」—— 顺序反过来就会出现
//   "理由里说的标签和算分的标签不是同一个"这种自相矛盾（AC-34 专门核对这一点）。
//
// 【打分比较用整数而不是浮点】
//   §6.6 的 score = 1000 / (1 + 薄弱度)，薄弱度 = (total - solved) / total。
//   直接比 double 在 total/solved 相同或成比例时会引入"本该并列却不相等"的抖动，
//   而 AC-46 要求【连跑 5 次同一输入得同一题】。故 pick_recommendation() 内部
//   用交叉相乘的整数比较（(totalA-solvedA)*totalB < (totalB-solvedB)*totalA），
//   score 只作为调试信息留在结构体里，不参与决策。
// =============================================================================
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/problem.h"

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// ---- 登录上下文 ----------------------------------------------------------------
// 本模块需要的最小登录信息：只有 user_id 会进入 SQL 绑定（D-21 的参数化通道）。
// 与 admin.h 里的 AuthUser 不是同一个东西：那边问的是"你是什么角色"（放行决策），
// 这边问的是"这些提交/收藏是谁的"（数据归属），字段不同是因为它们服务的判定不同。
// T3.3 的会话层落地后，两者都由同一次 session 解析结果构造出来。
struct ViewerContext {
  unsigned long long user_id = 0;   // 0 = 匿名（见文件头【user_id = 0 这一个值】）
  std::string username;
};

// 匿名请求在 SQL 里用的占位 user_id。取 0 的理由见文件头；改成别的值要连带改那里。
inline constexpr unsigned long long kAnonymousUserId = 0;

// ---- 进度与排序（§6.4 / §6.3 的 progress 与 sort 参数）--------------------------

// §6.4 推导出的三态进度。同时也是 `/api/problems` 的 progress 过滤取值
// （§6.3 端点表写的就是 none/attempted/solved 三个值），故只定义一个枚举。
enum class Progress { kNone, kAttempted, kSolved };
const char* to_string(Progress value);
// 大小写不敏感；未命中返回 false 且不写出参（与 model::parse_difficulty 同一约定）
bool parse_progress(const std::string& text, Progress* out);

enum class ProblemSort { kDefault, kFeatured, kEasiest, kHardest };
const char* to_string(ProblemSort value);
bool parse_problem_sort(const std::string& text, ProblemSort* out);

// ---- 标签 -----------------------------------------------------------------------

struct TagRef {
  std::string name;
  std::string category;   // 'algorithm' / 'source'（tags.category 的两个枚举值）
};

// `/api/tags` 的响应体（§6.3：{algorithm:[...], source:[...]}）
struct TagGroups {
  std::vector<std::string> algorithm;
  std::vector<std::string> source;
};

// ---- 列表 -----------------------------------------------------------------------

struct ListQuery {
  std::string keyword;                          // 匹配 title 或 slug（子串，大小写不敏感）
  bool has_difficulty = false;
  model::Difficulty difficulty = model::Difficulty::kEasy;
  std::vector<std::string> tags;                // 【全部命中】（AND），空 = 不按标签筛
  Progress progress = Progress::kNone;           // 过滤的是 §6.4 推导出的枚举值本身
  bool favorite_only = false;
  ProblemSort sort = ProblemSort::kDefault;
  int page = 1;                                 // 1-based
  int size = 20;
};

// 分页护栏。size 上限的作用是给串行化的 DB（[D-21]）封顶：一次 size=100000 的
// 查询在单条连接上会跑很久，把同期的所有请求一起拖住（§1.4 的 P95 < 200ms）。
inline constexpr int kMaxPageSize = 100;

struct ProblemListItem {
  int id = 0;
  std::string slug;
  std::string title;
  std::string difficulty;                 // 'easy' / 'medium' / 'hard'
  std::vector<TagRef> tags;               // 按 (category, name) 升序（确定性）
  Progress my_progress = Progress::kNone;
  bool is_favorite = false;
  unsigned int time_limit_ms = 3000;
  unsigned int mem_limit_kb = 131072;     // AS 口径，非 RSS（[D-63]，前端须标注）
  unsigned int case_count = 0;            // problems.case_count 受控反范式列（§3.4）
  bool is_featured = false;
  int sort_no = 0;
};

struct ProblemList {
  std::vector<ProblemListItem> list;
  long long total = 0;    // 过滤后的总条数（不含分页），供前端算页数
  int page = 1;           // 回显【实际生效】的页码/页大小（size 被护栏夹过时以回显为准）
  int size = 20;
};

// ---- 详情 -----------------------------------------------------------------------

// 题目页「你的最近提交」的条数（§6.3 端点表的 recentSubmissions:[最近5条] / T4.11）。
inline constexpr std::size_t kRecentSubmissionLimit = 5;

// 题目页「你的最近提交」的一行（T4.11 / T4.21：勾选两条做 diff 时只需要 id 与结论）
struct RecentSubmission {
  unsigned long long id = 0;
  std::string status;      // pending / judging / done / failed（§3.2）
  std::string verdict;     // 空 = NULL（还没出结论）
  std::string message;     // 空 = NULL
  bool has_case_passed = false;   unsigned int case_passed = 0;
  bool has_case_total = false;    unsigned int case_total = 0;
  bool has_total_ms = false;      unsigned int total_ms = 0;
  bool has_max_mem_kb = false;    unsigned int max_mem_kb = 0;
  std::string created_at;  // UTC 文本，本地化是前端的活（[D-17]）
};

// 题目页形参（供用户在编辑器里看清该实现哪个签名；D-28 模板 + 提交前核对）
struct ParamRef {
  std::string name;
  std::string cpp_type;
};

// `nextRecommendation`（§6.6）。**未登录时整个字段不下发**（AC-25），故用 optional。
struct NextRecommendation {
  int id = 0;
  std::string slug;
  std::string title;
  std::string difficulty;
  // 主标签（[D-68] 选出来的那个），理由文案与它同源（[D-59]）
  std::string tag;
  long long tag_total = 0;   // 该标签下的题目总数（与用户无关，§6.5 的 total 口径）
  long long tag_solved = 0;  // 该用户在该标签下已 AC 的题数
  std::string reason;        // 服务端算好的可解释文案；前端【不得】自己编
  double score = 0.0;        // 1000/(1+薄弱度)。仅供核对与调试，不参与决策（见文件头）
};

struct ProblemDetail {
  int id = 0;
  std::string slug;
  std::string title;
  std::string difficulty;
  std::string statement_md;
  std::string hint_md;             // 空 = 库里是 NULL ⇒ 序列化为 null，前端不渲染提示 tab
  std::string signature;
  std::string method_name;
  std::string return_type;
  std::string harness_prelude;     // 空 = NULL（无 __expr 逃生舱）
  std::string code_template;       // 编辑器初始内容（[D-28]）
  std::vector<ParamRef> params;
  std::vector<TagRef> tags;
  unsigned int time_limit_ms = 3000;
  unsigned int mem_limit_kb = 131072;   // AS 口径（[D-63]）
  unsigned int case_count = 0;
  bool is_featured = false;
  int sort_no = 0;
  Progress my_progress = Progress::kNone;
  bool is_favorite = false;
  std::vector<RecentSubmission> recent_submissions;   // 最近 5 条；未登录为空数组
  // nullopt 有两种含义，前端都渲染"完成度"卡（§7.4）：全部通过 / 无 algorithm 标签候选。
  // ⚠️ 与"未登录"不同：未登录时这个 optional 所在的字段整个不下发。
  std::optional<NextRecommendation> next_recommendation;
  // 是否已登录。to_json 据此决定要不要下发 nextRecommendation（[D-43]/AC-25）。
  // 判定口径是 viewer != nullptr && viewer->user_id != kAnonymousUserId —— 即"有会话，
  // 且该会话的 user_id 不是匿名标记"。T3.3 不该造出 user_id=0 的会话（users.id 是
  // AUTO_INCREMENT，0 进不去），这条判定只是不让"手滑递进来一个空会话"变成
  // "给匿名用户算了一份推荐"。
  bool authenticated = false;
};

enum class DetailStatus {
  kFound,       // → 0    200
  kNotFound,    // → 1004 404（slug 拼错与题库被删在这里是同一件事）
  kError        // → 1008 500（DB 异常；见 db/mysql_pool.h「语句失败一律如实上抛」）
};
int error_code_of(DetailStatus status);
const char* error_message_of(DetailStatus status);

// ---- 查询串解析（纯逻辑，无 DB）--------------------------------------------------

// §6.3 的失败包络是 `{code:1001, data:{field, reason}}`，故错误里要能指名是哪个参数。
struct QueryParseError {
  std::string field;    // 例："page" / "tags"；解析失败一定有值
  std::string reason;   // 人可读原因（直接进 data.reason）
};

// URL 解码（'+' → 空格，%XX）。解析查询串与将来 T3.5 的其余端点都要用，故单列。
// 遇到不合法的转义（如 "%ZZ" 或被截断的 "%4"）返回 false —— 不做"尽力而为"的
// 半解码，那会让含字面量 '%' 的查询串被悄悄改写。
bool url_decode(const std::string& in, std::string* out);

// 解析 `GET /api/problems` 的查询串（'a=1&b=2'，顺序无关，可有可无）。
// 失败（kind 已知但取值非法）返回 false 并写 error；**不改动 *out**。
//
// 取值规则（与 §6.3 端点表一一对应）：
//   keyword   子串；trim 后为空 = 不筛；长度上限 200（防止把一个超长 LIKE 塞给串行化的 DB）
//   difficulty easy/medium/hard；tags  逗号分隔，【全部命中】（AND 语义，见下）
//   progress  none/attempted/solved；favorite 1/true/0/false（空串当 0）
//   sort      default/featured/easiest/hardest；page  1-based；size  1..kMaxPageSize
//
// 三条刻意选择：
//   ① **枚举值非法 → 1001，而不是回退默认值**：静默回退会让用户拿到一个"看起来
//      按我选的筛了、其实没筛"的结果，这种错没有任何报错信号。
//   ② **未知参数名 → 忽略**：前端加一个新筛选不该让旧后端整体 400。
//   ③ **同名参数出现多次 → 最后一个生效**：httplib 的 params 是 multimap，
//      路由层按顺序摊平成 vector 后天然就是这个语义，不必另外发明规则。
//
// AND 语义的取舍：`tags=a,b` 意为"同时挂 a 和 b 的题"。换成"任一命中"只需把
// 逐个 EXISTS 换成一个 IN 子查询 —— 之所以默认 AND，是因为筛选栏里多个标签被
// 同时勾选时，用户表达的是"同时符合"（与 LeetCode 的跨维度 AND 一致）。
bool parse_list_query(const std::string& raw_query, ListQuery* out, QueryParseError* error);

// 校验"这个查询能不能对这个人执行"。**只有一条规则**：匿名请求不得携带依赖个人
// 数据的过滤条件（progress≠none / favorite=1），返回 1001 并指名 field。
//
// 为什么不给它"降级处理"（未登录就忽略这两个筛选）：忽略掉的结果是一个【看起来
// 合法但不是用户要的东西】的列表，而空列表更糟 —— 用户会读成"题库里没有这类题"。
// 明确报错至少能让前端弹一句"登录后可按进度筛选"。
//
// query_problems() 内部也会再查一次（纵深防御：路由层将来若忘了调本函数，
// 业务层仍不会返回一份"看起来正常"的错列表）。
bool validate_list_query(const ViewerContext* viewer, const ListQuery& query, QueryParseError* error);

// ---- 查询（业务层入口）-----------------------------------------------------------

// `/api/tags`：全部标签按 category 分组，组内按 name 升序。
bool load_tag_groups(db::Session& session, TagGroups* out, std::string* error);
bool load_tag_groups(TagGroups* out, std::string* error);   // 自己取 [D-21] 租约

// `/api/problems`：多条件筛选 + 分页 + 进度/收藏 JOIN。
// 失败时 *out 一个字节都不改（调用方拿着旧值继续用，静默半更新比报错难查得多）。
bool query_problems(db::Session& session, const ViewerContext* viewer, const ListQuery& query,
                    ProblemList* out, std::string* error);
bool query_problems(const ViewerContext* viewer, const ListQuery& query, ProblemList* out,
                    std::string* error);

// `/api/problems/{slug}`。
DetailStatus load_problem_detail(db::Session& session, const std::string& slug,
                                 const ViewerContext* viewer, ProblemDetail* out,
                                 std::string* error);
DetailStatus load_problem_detail(const std::string& slug, const ViewerContext* viewer,
                                 ProblemDetail* out, std::string* error);

// ---- 下一题建议（T3.17，§6.6）---------------------------------------------------
//
// 拆成"取数 + 纯函数决策"两半：SQL 只负责把决策需要的两张小表取回来
// （标签通过率 / 候选题及其 algorithm 标签），真正的选主标签与比大小在一个
// 纯函数里完成。理由见文件头【打分比较用整数而不是浮点】—— 那条确定性要求
// 最好落在一个"能直接单测的纯函数"上，而不是散在一段拼出来的 SQL 里。

// 每个 algorithm 标签对该用户的通过率。total = 该标签下题目总数（与用户无关，§6.5）
struct TagStat {
  long long tag_id = 0;
  std::string name;
  long long total = 0;
  long long solved = 0;
};

// 一个候选题（progress ≠ 'solved' 且至少挂一个 algorithm 标签）
struct RecommendationCandidate {
  int id = 0;
  std::string slug;
  std::string title;
  std::string difficulty;
  int sort_no = 0;
  std::vector<std::pair<long long, std::string>> algorithm_tags;   // (tags.id, name)
};

// 纯函数决策：[D-68] 主标签 → [D-59] 薄弱度 → 同分按 difficulty ASC → sort_no ASC。
// 候选集为空（全部通过 / 无 algorithm 标签）返回 nullopt。
std::optional<NextRecommendation> pick_recommendation(const std::vector<TagStat>& tag_stats,
                                                      const std::vector<RecommendationCandidate>& candidates);

// 取数 + 决策。未登录时返回 false 并写 error —— 调用方应当压根不调用它
// （[D-43]/AC-25：未登录时字段整个不下发），而不是拿它的结果去判断"有没有"。
bool compute_next_recommendation(db::Session& session, const ViewerContext& viewer,
                                 std::optional<NextRecommendation>* out, std::string* error);
bool compute_next_recommendation(const ViewerContext& viewer,
                                 std::optional<NextRecommendation>* out, std::string* error);

// ---- 序列化（§6.3 的 data 块，camelCase）----------------------------------------
// 与 admin.cpp 的 to_json 同一条纪律：只序列化 data，{code,message,data} 包络
// 由 common/response.h（T0.4）独家负责 —— 两处各写一遍就是 T3.5 明令禁止的"两套包络"。
std::string to_json(const TagGroups& tags);
std::string to_json(const ProblemList& list);
std::string to_json(const ProblemDetail& detail);

}  // namespace web
}  // namespace oj
