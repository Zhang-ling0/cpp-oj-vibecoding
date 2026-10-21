// =============================================================================
// 题目列表 / 详情 API 的实现（include/web/problem_api.h 的配套）
// 对应任务：SPEC.md §9 M3 T3.6（列表查询）+ T3.17（下一题建议）；契约见 §6.3 题目组
//
// 【整个列表查询只有两条 SQL：一条 COUNT、一条取页】
//   [D-21] 把 DB 访问串行化在【一把进程内互斥量】上，于是"最坏延迟"从最慢的那条查询
//   放大成"所有查询之和"。§1.4 又要求列表查询 P95 < 200ms。因此筛选条件必须让
//   COUNT 与取页走【同一个 WHERE 拼装函数】—— 写成两套，一套少写一个条件时
//   表现为"总数 47 但翻到第 3 页是空的"，这种错不会崩、只会让分页器发疯。
//
// 【标签聚合用 JSON_ARRAYAGG，且必须处理 LEFT JOIN 的 NULL 行】
//   problems LEFT JOIN problem_tags LEFT JOIN tags：一道没有标签的题在 GROUP BY 组里
//   依然有一行，只是 t.* 全是 NULL。此时 JSON_ARRAYAGG(JSON_OBJECT('name', NULL,...))
//   会产出 [{"name":null,"category":null}] —— 不是 NULL，所以 COALESCE 兜不住，
//   前端会拿到一个"名字叫 null 的标签"。MySQL 的 JSON_ARRAYAGG 忽略 NULL 值，
//   故把整项包成 CASE WHEN t.id IS NULL THEN NULL ELSE JSON_OBJECT(...) END。
//   JSON_ARRAYAGG 不支持 ORDER BY，组内顺序不定 → 取回后在 C++ 里按 (category, name)
//     排序。**顺序必须确定**：否则同一次查询两次返回的 JSON 不同，diff 测试无从下手。
//
// 【进度表达式只有一个定义处：progress_expression()】
//   §6.4 的 CASE 被列表、详情、推荐三处使用。写三遍的话，AC-11（列表页进度）
//   与 AC-22（掌握分布）迟早有一处对不上，而这种错只表现为"某个用户的进度多了一格"。
//
// 【user_id=0 即匿名：SQL 文本对登录与否完全一致】
//   见头文件【user_id = 0 这一个值承担全部"匿名"语义】。因此本文件里没有任何
//   "if (viewer) 用 A 版 SQL else 用 B 版 SQL" 的分支 —— 分支只有一处：
//   匿名时【不做】最近提交与推荐这两个查询（它们本来就无意义）。
//
// 【推荐为什么在应用层算分】
//   见头文件【打分比较用整数而不是浮点】：§6.6.1 的主标签选取是"该题的 algorithm 标签
//   里用户通过率最低者"，这是"先按题聚合、再在标签间取极值"的两层比较。
//   写成 SQL 要么套窗口函数、要么自连接，而 AC-46 要求连跑 5 次得同一题 ——
//   那种可复现性放在纯函数里最容易保证，也最容易单测。
// =============================================================================
#include "web/problem_api.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "db/mysql_pool.h"

namespace oj {
namespace web {
namespace {

// ---- 失败处理（与 admin.cpp 同一约定）-------------------------------------------
bool fail(std::string* error, const std::string& what, const db::DbError& db_error) {
  const std::string detail = what + "：" + db_error.format();
  common::log_error(common::LogChannel::kApp, "题目列表/详情 API " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

bool fail_text(std::string* error, const std::string& what) {
  common::log_error(common::LogChannel::kApp, "题目列表/详情 API " + what);
  if (error != nullptr) *error = what;
  return false;
}

// ---- 登录上下文的判定（唯一的"匿名"定义处）------------------------------------
unsigned long long viewer_id_of(const ViewerContext* viewer) {
  return (viewer != nullptr) ? viewer->user_id : kAnonymousUserId;
}
bool is_authenticated(const ViewerContext* viewer) {
  return viewer != nullptr && viewer->user_id != kAnonymousUserId;
}

// ---- §6.4 的进度表达式（三处共用的唯一定义）------------------------------------
const char* kProgressExpression =
    "CASE WHEN EXISTS(SELECT 1 FROM submissions s"
    "               WHERE s.problem_id = p.id AND s.user_id = ? AND s.verdict = 'AC')"
    "     THEN 'solved'"
    "     WHEN EXISTS(SELECT 1 FROM submissions s"
    "               WHERE s.problem_id = p.id AND s.user_id = ?)"
    "     THEN 'attempted'"
    "     ELSE 'none' END";

// 标签集合（一道题的标签，用在列表页与详情页的拼接里）
const char* kTagsAggregate =
    "COALESCE(JSON_ARRAYAGG(CASE WHEN t.id IS NULL THEN NULL"
    "          ELSE JSON_OBJECT('name', t.name, 'category', t.category) END),"
    "         JSON_ARRAY())";

// ---- 排序（D-61：顺序必须确定，否则翻页会漏题或重复）--------------------------
// 每个方案都以 p.id ASC 收尾作为最终 tiebreaker：sort_no 与 id 都可能重复，
// 少一个稳定的收尾项，MySQL 就可能对并列行给出任意顺序 —— 那会让"翻页不重不漏"
// 变成"偶尔漏一道"。难度用 ENUM 的声明序（easy < medium < hard，§3.2），
// 因此 ORDER BY p.difficulty ASC 就是"先易后难"，且能走 idx_diff。
std::string order_by_clause(ProblemSort sort) {
  switch (sort) {
    case ProblemSort::kDefault:
      return "p.sort_no ASC, p.id ASC";
    case ProblemSort::kFeatured:
      return "p.is_featured DESC, p.sort_no ASC, p.id ASC";
    case ProblemSort::kEasiest:
      return "p.difficulty ASC, p.sort_no ASC, p.id ASC";
    case ProblemSort::kHardest:
      return "p.difficulty DESC, p.sort_no ASC, p.id ASC";
  }
  return "p.sort_no ASC, p.id ASC";   // 不可达；枚举被改过时退回最保守的一档
}

// 难度排序权重：easy < medium < hard（与 §3.2 的 ENUM 声明序一致，模型枚举的顺序同）。
// 写成本函数而不是直接比 model::Difficulty，是为了让"难度序"在本模块里只有一个定义 ——
// §6.6 的 tie-break（先易后难）与 T4.3 的难度筛选共用它。
int difficulty_rank(const std::string& difficulty) {
  if (difficulty == model::to_string(model::Difficulty::kEasy)) return 0;
  if (difficulty == model::to_string(model::Difficulty::kMedium)) return 1;
  if (difficulty == model::to_string(model::Difficulty::kHard)) return 2;
  return 3;   // 未知值排最后：宁可把排序交给 id 兜底，也不让它冒充"简单"
}

// ---- WHERE 拼装（COUNT 与取页共用，见文件头）-----------------------------------
std::string escape_like(const std::string& raw) {
  std::string escaped;
  escaped.reserve(raw.size() * 2);
  for (const char c : raw) {
    if (c == '%' || c == '_' || c == '\\') escaped.push_back('\\');   // 转义 LIKE 的三个元字符
    escaped.push_back(c);
  }
  return escaped;
}

std::string trim(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

std::string to_lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// 枚举参数统一的规范化：先 trim 再转小写。头文件承诺的"大小写不敏感"就落在这里 ——
// 与 model::parse_difficulty 同一约定。前端拼错大小写时，筛选整页失效且无从查起。
std::string normalize_enum_text(const std::string& text) { return to_lower(trim(text)); }

// 逐个标签一个 EXISTS ⇒【AND 语义】（同时挂全部所选标签）。
// 不用 JOIN + DISTINCT 的理由：JOIN 会把题乘以标签数，分页的 LIMIT/OFFSET
// 就会在"乘完再数"的结果上取 —— 第 2 页直接漏题。EXISTS 不改变行数。
void append_where(unsigned long long viewer_id, const ListQuery& query, std::string* sql,
                  std::vector<db::Value>* params) {
  // archived=0（阶段 2 下线归档）放在基句而不是各个筛选分支里：COUNT 与取页共用本函数，
  // 归档题因此在两个口径里同时消失 —— 分散写两处迟早会漏掉一处，症状是"总数和页数对不上"。
  std::string clause = " WHERE p.archived = 0";

  if (!query.keyword.empty()) {
    const std::string pattern = "%" + escape_like(query.keyword) + "%";
    clause +=
        " AND (p.title LIKE ? ESCAPE '\\\\' OR p.slug LIKE ? ESCAPE '\\\\')";
    params->emplace_back(db::Value::from_text(pattern));
    params->emplace_back(db::Value::from_text(pattern));
  }
  if (query.has_difficulty) {
    clause += " AND p.difficulty = ?";
    params->emplace_back(db::Value::from_text(model::to_string(query.difficulty)));
  }
  for (const std::string& tag : query.tags) {
    clause +=
        " AND EXISTS (SELECT 1 FROM problem_tags pt JOIN tags t ON t.id = pt.tag_id"
        "              WHERE pt.problem_id = p.id AND t.name = ?)";
    params->emplace_back(db::Value::from_text(tag));
  }
  if (query.progress == Progress::kSolved) {
    clause +=
        " AND EXISTS (SELECT 1 FROM submissions s"
        "              WHERE s.problem_id = p.id AND s.user_id = ? AND s.verdict = 'AC')";
    params->emplace_back(db::Value::from_u64(viewer_id));
  } else if (query.progress == Progress::kAttempted) {
    // 过滤的是 §6.4 推导出的枚举值本身，故 attempted 与 solved 互斥：
    // 做过并通过的题只出现在 solved 里。写成"有过任意提交"会让两个筛选重叠，
    // 而筛选栏里的三个选项必须互斥（§7.1 的筛选栏）。
    clause +=
        " AND EXISTS (SELECT 1 FROM submissions s"
        "              WHERE s.problem_id = p.id AND s.user_id = ?)"
        " AND NOT EXISTS (SELECT 1 FROM submissions s"
        "                 WHERE s.problem_id = p.id AND s.user_id = ? AND s.verdict = 'AC')";
    params->emplace_back(db::Value::from_u64(viewer_id));
    params->emplace_back(db::Value::from_u64(viewer_id));
  }
  if (query.favorite_only) {
    clause += " AND up.favorite = 1";
  }

  *sql = std::move(clause);
}

// ---- 行 → 结构体 ----------------------------------------------------------------
std::vector<TagRef> parse_tag_array(const std::string& text) {
  std::vector<TagRef> tags;
  const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  if (!parsed.is_array()) return tags;
  for (const nlohmann::json& item : parsed) {
    if (!item.is_object()) continue;
    TagRef ref;
    ref.name = item.value("name", std::string());
    ref.category = item.value("category", std::string());
    if (ref.name.empty()) continue;   // 库里的标签名非空（tags.name NOT NULL）
    tags.push_back(std::move(ref));
  }
  // JSON_ARRAYAGG 不保证顺序（见文件头），按 (category, name) 定死。
  std::sort(tags.begin(), tags.end(), [](const TagRef& a, const TagRef& b) {
    if (a.category != b.category) return a.category < b.category;
    return a.name < b.name;
  });
  return tags;
}

nlohmann::json tags_to_json(const std::vector<TagRef>& tags) {
  nlohmann::json array = nlohmann::json::array();
  for (const TagRef& tag : tags) {
    array.push_back({{"name", tag.name}, {"category", tag.category}});
  }
  return array;
}

// 空串 = 库里是 NULL ⇒ 序列化成 null 而不是 ""（admin.cpp 同一约定）。
// 两者的前端含义不同：NULL = 库里没有这个东西，空串 = 有但是空的。
nlohmann::json nullable_text(const std::string& value) {
  if (value.empty()) return nlohmann::json(nullptr);
  return nlohmann::json(value);
}

Progress progress_from_text(const std::string& text) {
  Progress progress = Progress::kNone;
  if (!parse_progress(text, &progress)) return Progress::kNone;
  return progress;
}

// ---- 查询串解析 -----------------------------------------------------------------
int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 只接受【纯数字】：page=1abc / page=-1 / page= 1.5 一律 1001。
// strtoll 会把 "12abc" 解析成 12 而不报错 —— 那就是静默地把用户的输入改了。
bool parse_digits(const std::string& text, long long* out) {
  if (text.empty()) return false;
  long long value = 0;
  for (const char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    value = value * 10 + (c - '0');
    if (value > 1000000000LL) return false;   // 溢出/离谱值直接判非法，不静默夹取
  }
  *out = value;
  return true;
}

// LIMIT 5 是写在 SQL 文本里的字面量（不给占位符），故用 static_assert 把它与
// §6.3 的 kRecentSubmissionLimit 焊在一起：将来改常量时这里会响，而不是让人
// 去数 SQL 里那个 5。
static_assert(kRecentSubmissionLimit == 5, "load_recent_submissions 的 LIMIT 与常量不一致");

// 1001 的统一出口：写 field + reason，且**不改 *out**。
bool reject(QueryParseError* error, const std::string& field, const std::string& reason) {
  if (error != nullptr) {
    error->field = field;
    error->reason = reason;
  }
  return false;
}

}  // namespace

// ---- 枚举 -----------------------------------------------------------------------

const char* to_string(Progress value) {
  switch (value) {
    case Progress::kNone:
      return "none";
    case Progress::kAttempted:
      return "attempted";
    case Progress::kSolved:
      return "solved";
  }
  return "none";
}

bool parse_progress(const std::string& text, Progress* out) {
  const std::string key = normalize_enum_text(text);
  Progress parsed = Progress::kNone;
  if (key == "none") {
    parsed = Progress::kNone;
  } else if (key == "attempted") {
    parsed = Progress::kAttempted;
  } else if (key == "solved") {
    parsed = Progress::kSolved;
  } else {
    return false;
  }
  if (out != nullptr) *out = parsed;
  return true;
}

const char* to_string(ProblemSort value) {
  switch (value) {
    case ProblemSort::kDefault:
      return "default";
    case ProblemSort::kFeatured:
      return "featured";
    case ProblemSort::kEasiest:
      return "easiest";
    case ProblemSort::kHardest:
      return "hardest";
  }
  return "default";
}

bool parse_problem_sort(const std::string& text, ProblemSort* out) {
  const std::string key = normalize_enum_text(text);
  ProblemSort parsed = ProblemSort::kDefault;
  if (key == "default") {
    parsed = ProblemSort::kDefault;
  } else if (key == "featured") {
    parsed = ProblemSort::kFeatured;
  } else if (key == "easiest") {
    parsed = ProblemSort::kEasiest;
  } else if (key == "hardest") {
    parsed = ProblemSort::kHardest;
  } else {
    return false;
  }
  if (out != nullptr) *out = parsed;
  return true;
}

int error_code_of(DetailStatus status) {
  switch (status) {
    case DetailStatus::kFound:
      return 0;
    case DetailStatus::kNotFound:
      return 1004;
    case DetailStatus::kError:
      return 1008;
  }
  return 1008;
}

const char* error_message_of(DetailStatus status) {
  switch (status) {
    case DetailStatus::kFound:
      return "ok";
    case DetailStatus::kNotFound:
      return "题目不存在";
    case DetailStatus::kError:
      return "服务器内部错误";
  }
  return "服务器内部错误";
}

// ---- 查询串解析 -----------------------------------------------------------------

bool url_decode(const std::string& in, std::string* out) {
  std::string result;
  result.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '+') {
      result.push_back(' ');
      continue;
    }
    if (c != '%') {
      result.push_back(c);
      continue;
    }
    if (i + 2 >= in.size()) return false;   // 被截断的转义（末尾一个 '%' 或 "%4"）
    const int hi = hex_digit(in[i + 1]);
    const int lo = hex_digit(in[i + 2]);
    if (hi < 0 || lo < 0) return false;
    result.push_back(static_cast<char>(hi * 16 + lo));
    i += 2;
  }
  if (out != nullptr) *out = std::move(result);
  return true;
}

bool parse_list_query(const std::string& raw_query, ListQuery* out, QueryParseError* error) {
  ListQuery parsed;

  std::size_t cursor = 0;
  while (cursor <= raw_query.size()) {
    const std::size_t amp = raw_query.find('&', cursor);
    const std::string segment =
        raw_query.substr(cursor, (amp == std::string::npos ? raw_query.size() : amp) - cursor);
    cursor = (amp == std::string::npos) ? raw_query.size() + 1 : amp + 1;

    if (segment.empty()) continue;
    const std::size_t eq = segment.find('=');
    std::string raw_key = segment.substr(0, eq);
    std::string raw_value = (eq == std::string::npos) ? std::string() : segment.substr(eq + 1);
    std::string key;
    std::string value;
    if (!url_decode(raw_key, &key) || !url_decode(raw_value, &value)) {
      return reject(error, "", "查询串里有不合法的 URL 转义");
    }

    // 未知参数名忽略（见头文件说明 ②）；同名参数最后一个生效（说明 ③）——
    // 下面全部是"覆盖式赋值"，不需要额外记"出现过几次"。
    if (key == "keyword") {
      parsed.keyword = trim(value);
      if (parsed.keyword.size() > 200) {
        return reject(error, "keyword", "关键词过长（上限 200 字符）");
      }
    } else if (key == "difficulty") {
      if (value.empty()) continue;   // 留空 = 不按难度筛（前端清空筛选时会发空值）
      if (!model::parse_difficulty(value, &parsed.difficulty)) {
        return reject(error, "difficulty", "difficulty 只能是 easy / medium / hard");
      }
      parsed.has_difficulty = true;
    } else if (key == "tags") {
      parsed.tags.clear();
      if (value.empty()) continue;
      std::size_t start = 0;
      while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string name = trim(
            value.substr(start, (comma == std::string::npos ? value.size() : comma) - start));
        start = (comma == std::string::npos) ? value.size() + 1 : comma + 1;
        if (name.empty()) continue;   // "a,,b" / 末尾多一个逗号都不该报错
        if (name.size() > 64) {
          return reject(error, "tags", "标签名过长（上限 64 字符）");
        }
        if (std::find(parsed.tags.begin(), parsed.tags.end(), name) == parsed.tags.end()) {
          parsed.tags.push_back(name);   // 去重：重复标签会让 append_where 拼出两个 EXISTS
        }
        if (parsed.tags.size() > 20) {
          return reject(error, "tags", "标签数量过多（上限 20 个）");
        }
      }
    } else if (key == "progress") {
      if (value.empty()) continue;
      if (!parse_progress(value, &parsed.progress)) {
        return reject(error, "progress", "progress 只能是 none / attempted / solved");
      }
    } else if (key == "favorite") {
      const std::string flag = trim(value);
      if (flag.empty()) {
        parsed.favorite_only = false;
      } else if (flag == "1" || flag == "true" || flag == "yes" || flag == "on") {
        parsed.favorite_only = true;
      } else if (flag == "0" || flag == "false" || flag == "no" || flag == "off") {
        parsed.favorite_only = false;
      } else {
        return reject(error, "favorite", "favorite 只能是 1/0 或 true/false");
      }
    } else if (key == "sort") {
      if (value.empty()) continue;
      if (!parse_problem_sort(value, &parsed.sort)) {
        return reject(error, "sort", "sort 只能是 default / featured / easiest / hardest");
      }
    } else if (key == "page") {
      if (value.empty()) continue;
      long long page = 0;
      if (!parse_digits(value, &page) || page < 1) {
        return reject(error, "page", "page 必须是 ≥ 1 的整数");
      }
      parsed.page = static_cast<int>(page);
    } else if (key == "size") {
      if (value.empty()) continue;
      long long size = 0;
      if (!parse_digits(value, &size) || size < 1) {
        return reject(error, "size", "size 必须是 ≥ 1 的整数");
      }
      // 护栏：> kMaxPageSize 夹到上限而不是 1001（见 ListQuery 里的理由）。
      // 响应的 size 字段回显【实际生效】的值，前端据此画分页器。
      parsed.size = (size > kMaxPageSize) ? kMaxPageSize : static_cast<int>(size);
    }
  }

  if (out != nullptr) *out = std::move(parsed);
  return true;
}

bool validate_list_query(const ViewerContext* viewer, const ListQuery& query,
                         QueryParseError* error) {
  if (is_authenticated(viewer)) return true;
  if (query.progress != Progress::kNone) {
    return reject(error, "progress", "按进度筛选需要登录");
  }
  if (query.favorite_only) {
    return reject(error, "favorite", "按收藏筛选需要登录");
  }
  return true;
}

// ---- 标签 ------------------------------------------------------------------------

bool load_tag_groups(db::Session& session, TagGroups* out, std::string* error) {
  // tags 表是全库最小的一张（几十行），一条不带过滤的查询足够。
  // 排序按 (category, name) 而不是 name：分组后前端不需要再排，而"同组内按名字排"
  // 是筛选栏里能被人一眼看出的稳定次序。category 用字典序（algorithm < source）。
  static const char* kSql = "SELECT name, category FROM tags ORDER BY category ASC, name ASC";
  const db::ResultSet result = session.run(kSql);
  if (result.failed()) return fail(error, "读取标签失败", result.error());

  TagGroups groups;
  for (const db::Row& row : result.rows()) {
    const std::string name = row.str(0);
    if (name.empty()) continue;
    if (row.str(1) == "algorithm") {
      groups.algorithm.push_back(name);
    } else if (row.str(1) == "source") {
      groups.source.push_back(name);
    } else {
      // category 是 ENUM，别的值进不来。真出现了也不该让整个筛选栏空掉 ——
      // 未知类别的标签在筛选栏里没有位置可放，跳过是唯一不失真的处理。
      common::log_warn(common::LogChannel::kApp,
                       "题目列表/详情 API 跳过未知 category 的标签：" + name + " / " + row.str(1));
    }
  }
  *out = std::move(groups);
  return true;
}

bool load_tag_groups(TagGroups* out, std::string* error) {
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    return fail_text(error, "读取标签失败：连接池未就绪，取不到租约（见 app.log）");
  }
  return load_tag_groups(lease.session(), out, error);
}

// ---- 列表 ------------------------------------------------------------------------

bool query_problems(db::Session& session, const ViewerContext* viewer, const ListQuery& query,
                    ProblemList* out, std::string* error) {
  const unsigned long long viewer_id = viewer_id_of(viewer);

  // 纵深防御：见 validate_list_query 的注释。路由层应当先验一次，这里再兜一次。
  QueryParseError invalid;
  if (!validate_list_query(viewer, query, &invalid)) {
    return fail_text(error, "查询条件不合法：" + invalid.field + " " + invalid.reason);
  }

  // 页码/页大小护栏：parse_list_query 已经夹过一遍，直接构造的 ListQuery（如将来
  // 服务端内部调用）也要过一遍 —— 一个 size=0 会让 LIMIT 0 返回空列表，
  // 一个负 offset 会让 MySQL 直接报错。
  if (query.page < 1 || query.size < 1) {
    return fail_text(error, "查询条件不合法：page 与 size 必须 ≥ 1");
  }
  const int page_size = (query.size > kMaxPageSize) ? kMaxPageSize : query.size;

  std::string where;
  std::vector<db::Value> where_params;
  append_where(viewer_id, query, &where, &where_params);

  // ① COUNT。与取页共用 where，故"总数"与"翻页结果"永远是同一口径。
  //    LEFT JOIN user_problems 不会放大行数（主键 (user_id, problem_id) 至多一行），
  //    所以 COUNT(*) 就是题数，不需要 DISTINCT。
  std::vector<db::Value> count_params;
  count_params.emplace_back(db::Value::from_u64(viewer_id));   // LEFT JOIN 的 user_id
  count_params.insert(count_params.end(), where_params.begin(), where_params.end());
  const std::string count_sql =
      "SELECT COUNT(*) FROM problems p"
      " LEFT JOIN user_problems up ON up.problem_id = p.id AND up.user_id = ?" + where;
  const db::ResultSet counted = session.run(count_sql, count_params);
  if (counted.failed()) return fail(error, "统计题目总数失败", counted.error());
  if (counted.empty()) {
    return fail_text(error, "统计题目总数失败：查询返回 0 行（COUNT 本应恰好 1 行）");
  }

  // 服务端夹页（P1-6）：筛选变化后 page 可能越界，夹回真实最大页。
  const long long total_all = static_cast<long long>(counted.at(0).u64(0, 0));
  const long long max_page = total_all == 0 ? 1 : (total_all + page_size - 1) / page_size;
  const int effective_page = (query.page > max_page) ? static_cast<int>(max_page) : query.page;

  // ② 取页。参数顺序必须与 SQL 文本里 ? 的出现顺序严格一致：
  //    LEFT JOIN → SELECT 里的进度表达式（2 个）→ WHERE（append_where 追加的）→ LIMIT/OFFSET。
  std::string sql;
  sql +=
      "SELECT p.id, p.slug, p.title, p.difficulty, p.time_limit_ms, p.mem_limit_kb,"
      "       p.case_count, p.is_featured, p.sort_no, ";
  sql += kProgressExpression;
  // ⚠️ is_favorite 必须写成聚合：ONLY_FULL_GROUP_BY 下，MySQL 只能从 p.id 推断 p.* 的
  //   函数依赖，推不出 up.*（LEFT JOIN 的条件里带 user_id 参数）—— 裸列会报 1055
  //   "not functionally dependent on columns in GROUP BY clause"（实测）。
  //   MAX() 在这里等价于取值本身：user_problems 主键是 (user_id, problem_id)，
  //   同一个 (题, 用户) 至多一行，被标签 JOIN 放大成几行也是同一个值。
  sql += " AS my_progress, COALESCE(MAX(up.favorite), 0) AS is_favorite, ";
  sql += kTagsAggregate;
  sql += " AS tag_json FROM problems p"
         " LEFT JOIN user_problems up ON up.problem_id = p.id AND up.user_id = ?"
         " LEFT JOIN problem_tags pt ON pt.problem_id = p.id"
         " LEFT JOIN tags t ON t.id = pt.tag_id";
  sql += where;
  sql += " GROUP BY p.id ORDER BY " + order_by_clause(query.sort) + " LIMIT ? OFFSET ?";

  std::vector<db::Value> params;
  params.emplace_back(db::Value::from_u64(viewer_id));   // LEFT JOIN user_problems
  params.emplace_back(db::Value::from_u64(viewer_id));   // 进度表达式：AC 优先
  params.emplace_back(db::Value::from_u64(viewer_id));   // 进度表达式：任意提交
  params.insert(params.end(), where_params.begin(), where_params.end());
  params.emplace_back(db::Value::from_i64(page_size));
  params.emplace_back(db::Value::from_i64(
      static_cast<long long>(effective_page - 1) * static_cast<long long>(page_size)));

  const db::ResultSet result = session.run(sql, params);
  if (result.failed()) return fail(error, "查询题目列表失败", result.error());

  ProblemList list;
  list.total = total_all;
  list.page = effective_page;
  list.size = page_size;
  list.list.reserve(result.size());
  for (const db::Row& row : result.rows()) {
    ProblemListItem item;
    item.id = static_cast<int>(row.i64(0));
    item.slug = row.str(1);
    item.title = row.str(2);
    item.difficulty = row.str(3);
    item.time_limit_ms = static_cast<unsigned int>(row.u64(4, 3000));
    item.mem_limit_kb = static_cast<unsigned int>(row.u64(5, 131072));
    item.case_count = static_cast<unsigned int>(row.u64(6, 0));
    item.is_featured = row.boolean(7);
    item.sort_no = static_cast<int>(row.i64(8));
    item.my_progress = progress_from_text(row.str(9));
    item.is_favorite = row.boolean(10);
    item.tags = parse_tag_array(row.str(11));
    list.list.push_back(std::move(item));
  }
  *out = std::move(list);
  return true;
}

bool query_problems(const ViewerContext* viewer, const ListQuery& query, ProblemList* out,
                    std::string* error) {
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    return fail_text(error, "查询题目列表失败：连接池未就绪，取不到租约（见 app.log）");
  }
  return query_problems(lease.session(), viewer, query, out, error);
}

// ---- 详情 ------------------------------------------------------------------------

namespace {

// 最近 5 条提交。只在已登录时执行 —— 匿名用户没有"最近提交"这回事。
bool load_recent_submissions(db::Session& session, const ViewerContext& viewer, int problem_id,
                             std::vector<RecentSubmission>* out, std::string* error) {
  static const char* kSql =
      "SELECT id, status, verdict, message, case_passed, case_total, total_ms, max_mem_kb,"
      "       created_at"
      " FROM submissions WHERE user_id = ? AND problem_id = ?"
      " ORDER BY id DESC LIMIT 5";
  const db::ResultSet result =
      session.run(kSql, {db::Value::from_u64(viewer.user_id),
                         db::Value::from_i64(static_cast<long long>(problem_id))});
  if (result.failed()) return fail(error, "读取最近提交失败", result.error());

  std::vector<RecentSubmission> rows;
  rows.reserve(result.size());
  for (const db::Row& row : result.rows()) {
    RecentSubmission entry;
    entry.id = row.u64(0);
    entry.status = row.str(1);
    entry.verdict = row.str(2);      // 空 = NULL（还在 pending/judging）
    entry.message = row.str(3);
    entry.has_case_passed = !row.is_null(4);
    entry.case_passed = static_cast<unsigned int>(row.u64(4, 0));
    entry.has_case_total = !row.is_null(5);
    entry.case_total = static_cast<unsigned int>(row.u64(5, 0));
    entry.has_total_ms = !row.is_null(6);
    entry.total_ms = static_cast<unsigned int>(row.u64(6, 0));
    entry.has_max_mem_kb = !row.is_null(7);
    entry.max_mem_kb = static_cast<unsigned int>(row.u64(7, 0));
    entry.created_at = row.str(8);
    rows.push_back(std::move(entry));
  }
  *out = std::move(rows);
  return true;
}

}  // namespace

DetailStatus load_problem_detail(db::Session& session, const std::string& slug,
                                 const ViewerContext* viewer, ProblemDetail* out,
                                 std::string* error) {
  const unsigned long long viewer_id = viewer_id_of(viewer);

  // 一条语句取回题目本体 + 进度 + 收藏 + 标签 + 形参。
  // 标签与形参用【相关子查询】而不是 JOIN+GROUP BY：这里只有一行结果，
  // 相关子查询的"重复执行"代价是每行各多一次索引查找（各走 problem_tags 主键），
  // 换来的是没有 GROUP BY、也没有"空标签行"那个 LEFT JOIN 的 NULL 陷阱。
  std::string sql;
  sql +=
      "SELECT p.id, p.slug, p.title, p.difficulty, p.statement_md, p.hint_md, p.signature,"
      "       p.method_name, p.return_type, p.harness_prelude, p.code_template,"
      "       p.time_limit_ms, p.mem_limit_kb, p.case_count, p.is_featured, p.sort_no, ";
  sql += kProgressExpression;
  sql += " AS my_progress, COALESCE(up.favorite, 0) AS is_favorite, ";
  sql +=
      "COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('name', t.name, 'category', t.category))"
      "            FROM problem_tags pt JOIN tags t ON t.id = pt.tag_id"
      "           WHERE pt.problem_id = p.id), JSON_ARRAY()) AS tag_json, ";
  sql +=
      "COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('idx', pp.param_idx, 'name', pp.name,"
      "                                        'cppType', pp.cpp_type))"
      "            FROM problem_params pp WHERE pp.problem_id = p.id), JSON_ARRAY()) AS param_json"
      " FROM problems p LEFT JOIN user_problems up ON up.problem_id = p.id AND up.user_id = ?"
      " WHERE p.slug = ? AND p.archived = 0";   // 归档题对用户不可见（阶段 2）→ 1004

  const db::ResultSet result =
      session.run(sql, {db::Value::from_u64(viewer_id),   // LEFT JOIN user_problems
                        db::Value::from_u64(viewer_id),   // 进度表达式：AC 优先
                        db::Value::from_u64(viewer_id),   // 进度表达式：任意提交
                        db::Value::from_text(slug)});
  if (result.failed()) {
    fail(error, "读取题目详情失败", result.error());
    return DetailStatus::kError;
  }
  if (result.empty()) {
    // slug 不存在与题库文件被删掉，在这一层是同一件事：库里没有这道题。
    // 故 1004，不区分"从来没有过"与"被移走了"—— §4.3 规定改题只经文件+重启，
    // 前端对这两种情况的处置是一样的（404 + 回列表）。
    return DetailStatus::kNotFound;
  }

  const db::Row& row = result.at(0);
  ProblemDetail detail;
  detail.authenticated = is_authenticated(viewer);
  detail.id = static_cast<int>(row.i64(0));
  detail.slug = row.str(1);
  detail.title = row.str(2);
  detail.difficulty = row.str(3);
  detail.statement_md = row.str(4);
  detail.hint_md = row.str(5);         // NULL → 空串 → to_json 里是 null
  detail.signature = row.str(6);
  detail.method_name = row.str(7);
  detail.return_type = row.str(8);
  detail.harness_prelude = row.str(9);
  detail.code_template = row.str(10);
  detail.time_limit_ms = static_cast<unsigned int>(row.u64(11, 3000));
  detail.mem_limit_kb = static_cast<unsigned int>(row.u64(12, 131072));
  detail.case_count = static_cast<unsigned int>(row.u64(13, 0));
  detail.is_featured = row.boolean(14);
  detail.sort_no = static_cast<int>(row.i64(15));
  detail.my_progress = progress_from_text(row.str(16));
  detail.is_favorite = row.boolean(17);
  detail.tags = parse_tag_array(row.str(18));

  {
    // 形参按 param_idx 升序（决定 harness 的调用顺序，§3.2）—— JSON_ARRAYAGG 不保证
    // 顺序，故显式排序后再装配；idx 只是排序用的搬运字段，不进响应体。
    std::vector<std::pair<unsigned long long, ParamRef>> params;
    const nlohmann::json parsed = nlohmann::json::parse(row.str(19), nullptr, false);
    if (parsed.is_array()) {
      for (const nlohmann::json& item : parsed) {
        if (!item.is_object()) continue;
        ParamRef ref;
        ref.name = item.value("name", std::string());
        ref.cpp_type = item.value("cppType", std::string());
        params.emplace_back(static_cast<unsigned long long>(item.value("idx", 0LL)), std::move(ref));
      }
    }
    std::sort(params.begin(), params.end(),
              [](const std::pair<unsigned long long, ParamRef>& a,
                 const std::pair<unsigned long long, ParamRef>& b) { return a.first < b.first; });
    detail.params.reserve(params.size());
    for (auto& entry : params) detail.params.push_back(std::move(entry.second));
  }

  if (detail.authenticated) {
    std::vector<RecentSubmission> recent;
    if (!load_recent_submissions(session, *viewer, detail.id, &recent, error)) {
      return DetailStatus::kError;
    }
    detail.recent_submissions = std::move(recent);

    // 推荐的失败【不让整个详情端点失败】：它是页面底部的一张建议卡，
    // 而题面是页面的主体。宁可少一张卡，也不要用户在读题时看到 1008。
    // ⚠️ 但失败必须可见：compute_next_recommendation 内部已把 DB 错误打进 app.log，
    //   这里不再重复记一条（否则同一个 DB 故障会在 app.log 里出现两次）。
    std::optional<NextRecommendation> recommendation;
    std::string ignored;
    if (!compute_next_recommendation(session, *viewer, &recommendation, &ignored)) {
      recommendation.reset();
    }
    detail.next_recommendation = std::move(recommendation);
  }

  *out = std::move(detail);
  return DetailStatus::kFound;
}

DetailStatus load_problem_detail(const std::string& slug, const ViewerContext* viewer,
                                 ProblemDetail* out, std::string* error) {
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    fail_text(error, "读取题目详情失败：连接池未就绪，取不到租约（见 app.log）");
    return DetailStatus::kError;
  }
  return load_problem_detail(lease.session(), slug, viewer, out, error);
}

// ---- 下一题建议（T3.17）----------------------------------------------------------

std::optional<NextRecommendation> pick_recommendation(
    const std::vector<TagStat>& tag_stats, const std::vector<RecommendationCandidate>& candidates) {
  std::unordered_map<long long, const TagStat*> by_id;
  by_id.reserve(tag_stats.size());
  for (const TagStat& stat : tag_stats) by_id.emplace(stat.tag_id, &stat);

  const RecommendationCandidate* best = nullptr;
  const TagStat* best_tag = nullptr;
  long long best_num = 0;   // 薄弱度的分子：total - solved
  long long best_den = 0;   // 薄弱度的分母：total

  for (const RecommendationCandidate& candidate : candidates) {
    // ---- 主标签 [D-68]：该题 algorithm 标签中【用户通过率最低】者，并列取 tags.id 最小 ----
    const TagStat* primary = nullptr;
    for (const std::pair<long long, std::string>& tag : candidate.algorithm_tags) {
      const auto found = by_id.find(tag.first);
      if (found == by_id.end()) continue;                 // 理论上不会发生（标签统计覆盖全部 algorithm 标签）
      const TagStat* stat = found->second;
      if (stat->total <= 0) continue;                     // 同上：候选题挂着该标签 ⇒ total ≥ 1
      if (primary == nullptr) {
        primary = stat;
        continue;
      }
      // 通过率更低者胜出：solvedA*totalB < solvedB*totalA（整数比较，不用 double）
      const long long lhs = stat->solved * primary->total;
      const long long rhs = primary->solved * stat->total;
      if (lhs < rhs || (lhs == rhs && stat->tag_id < primary->tag_id)) primary = stat;
    }
    if (primary == nullptr) continue;   // 该题挂的 algorithm 标签没有统计：跳过而不是瞎选

    // ---- 比薄弱度：(total - solved) / total，越大越薄弱，分越高（§6.6 的 score）----
    const long long num = primary->total - primary->solved;
    const long long den = primary->total;
    bool better = (best == nullptr);
    if (!better) {
      // weaker(candidate) < weaker(best)  ⇔  num*best_den < best_num*den
      const long long lhs = num * best_den;
      const long long rhs = best_num * den;
      if (lhs != rhs) {
        better = (lhs < rhs);   // 分数更高（更不薄弱）者胜
      } else {
        // 同分：difficulty ASC → sort_no ASC → id ASC（§6.6；id 是最后的可复现兜底）
        const int a_rank = difficulty_rank(candidate.difficulty);
        const int b_rank = difficulty_rank(best->difficulty);
        if (a_rank != b_rank) {
          better = (a_rank < b_rank);
        } else if (candidate.sort_no != best->sort_no) {
          better = (candidate.sort_no < best->sort_no);
        } else {
          better = (candidate.id < best->id);
        }
      }
    }
    if (better) {
      best = &candidate;
      best_tag = primary;
      best_num = num;
      best_den = den;
    }
  }

  if (best == nullptr || best_tag == nullptr) return std::nullopt;

  NextRecommendation recommendation;
  recommendation.id = best->id;
  recommendation.slug = best->slug;
  recommendation.title = best->title;
  recommendation.difficulty = best->difficulty;
  recommendation.tag = best_tag->name;
  recommendation.tag_total = best_tag->total;
  recommendation.tag_solved = best_tag->solved;
  // 可解释性内生 [D-59]：文案里的标签就是上面算分用的那个标签，
  // 不存在"理由说的和算的不是同一个"这种自相矛盾（AC-34 核对的正是这一点）。
  recommendation.reason = "「" + best_tag->name + "」标签你通过 " +
                          std::to_string(best_tag->solved) + "/" +
                          std::to_string(best_tag->total) + "，本题尚未通过";
  recommendation.score = 1000.0 * static_cast<double>(best_den) /
                         static_cast<double>(best_den + best_num);
  return recommendation;
}

bool compute_next_recommendation(db::Session& session, const ViewerContext& viewer,
                                 std::optional<NextRecommendation>* out, std::string* error) {
  if (viewer.user_id == kAnonymousUserId) {
    return fail_text(error, "计算下一题建议失败：调用方递进来的是匿名上下文（user_id=0）");
  }

  // ① 每个 algorithm 标签的通过率。COUNT(DISTINCT) 两处都不能省：
  //    p 侧要与 problem_tags 的多行去重，ac 侧是 DISTINCT 子查询（§6.5 的同一手法）。
  static const char* kTagSql =
      "SELECT t.id, t.name,"
      "       COUNT(DISTINCT p.id) AS total,"
      "       COUNT(DISTINCT ac.problem_id) AS solved"
      " FROM tags t"
      " JOIN problem_tags pt ON pt.tag_id = t.id"
      " JOIN problems p ON p.id = pt.problem_id"
      " LEFT JOIN (SELECT DISTINCT problem_id FROM submissions"
      "             WHERE user_id = ? AND verdict = 'AC') ac ON ac.problem_id = p.id"
      " WHERE t.category = 'algorithm' AND p.archived = 0"   // 归档题不参与"下一题建议"
      " GROUP BY t.id, t.name"
      " ORDER BY t.id";
  const db::ResultSet tag_result = session.run(kTagSql, {db::Value::from_u64(viewer.user_id)});
  if (tag_result.failed()) return fail(error, "统计标签通过率失败", tag_result.error());

  std::vector<TagStat> tag_stats;
  tag_stats.reserve(tag_result.size());
  for (const db::Row& row : tag_result.rows()) {
    TagStat stat;
    stat.tag_id = static_cast<long long>(row.i64(0));
    stat.name = row.str(1);
    stat.total = static_cast<long long>(row.i64(2));
    stat.solved = static_cast<long long>(row.i64(3));
    tag_stats.push_back(std::move(stat));
  }

  // ② 候选集：progress ≠ 'solved'（= 无 AC 提交）且至少挂一个 algorithm 标签。
  //    ORDER BY 只是让取回顺序稳定；真正的排序在 pick_recommendation 里做（那里才有
  //    全部候选的上下文）。
  static const char* kCandidateSql =
      "SELECT p.id, p.slug, p.title, p.difficulty, p.sort_no,"
      "       COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('id', t2.id, 'name', t2.name))"
      "                   FROM problem_tags pt2 JOIN tags t2 ON t2.id = pt2.tag_id"
      "                  WHERE pt2.problem_id = p.id AND t2.category = 'algorithm'),"
      "                JSON_ARRAY()) AS algo_tags"
      " FROM problems p"
      " WHERE p.archived = 0"                                  // 归档题不参与"下一题建议"
      "   AND NOT EXISTS (SELECT 1 FROM submissions s"
      "                    WHERE s.problem_id = p.id AND s.user_id = ? AND s.verdict = 'AC')"
      "   AND EXISTS (SELECT 1 FROM problem_tags pt3 JOIN tags t3 ON t3.id = pt3.tag_id"
      "                WHERE pt3.problem_id = p.id AND t3.category = 'algorithm')"
      " ORDER BY p.difficulty ASC, p.sort_no ASC, p.id ASC";
  const db::ResultSet candidate_result =
      session.run(kCandidateSql, {db::Value::from_u64(viewer.user_id)});
  if (candidate_result.failed()) return fail(error, "读取推荐候选题失败", candidate_result.error());

  std::vector<RecommendationCandidate> candidates;
  candidates.reserve(candidate_result.size());
  for (const db::Row& row : candidate_result.rows()) {
    RecommendationCandidate candidate;
    candidate.id = static_cast<int>(row.i64(0));
    candidate.slug = row.str(1);
    candidate.title = row.str(2);
    candidate.difficulty = row.str(3);
    candidate.sort_no = static_cast<int>(row.i64(4));
    const nlohmann::json parsed = nlohmann::json::parse(row.str(5), nullptr, false);
    if (parsed.is_array()) {
      for (const nlohmann::json& item : parsed) {
        if (!item.is_object()) continue;
        candidate.algorithm_tags.emplace_back(item.value("id", 0LL), item.value("name", std::string()));
      }
    }
    // JSON_ARRAYAGG 不保证顺序，而主标签的并列判定要求输入顺序稳定
    std::sort(candidate.algorithm_tags.begin(), candidate.algorithm_tags.end());
    candidates.push_back(std::move(candidate));
  }

  if (out != nullptr) *out = pick_recommendation(tag_stats, candidates);
  return true;
}

bool compute_next_recommendation(const ViewerContext& viewer,
                                 std::optional<NextRecommendation>* out, std::string* error) {
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    return fail_text(error, "计算下一题建议失败：连接池未就绪，取不到租约（见 app.log）");
  }
  return compute_next_recommendation(lease.session(), viewer, out, error);
}

// ---- 序列化 ----------------------------------------------------------------------

std::string to_json(const TagGroups& tags) {
  return nlohmann::json{{"algorithm", tags.algorithm}, {"source", tags.source}}.dump();
}

std::string to_json(const ProblemList& list) {
  nlohmann::json items = nlohmann::json::array();
  for (const ProblemListItem& item : list.list) {
    items.push_back({
        {"id", item.id},
        {"slug", item.slug},
        {"title", item.title},
        {"difficulty", item.difficulty},
        {"tags", tags_to_json(item.tags)},
        {"myProgress", to_string(item.my_progress)},
        {"isFavorite", item.is_favorite},
        {"timeLimitMs", item.time_limit_ms},
        {"memLimitKb", item.mem_limit_kb},
        {"caseCount", item.case_count},
        {"isFeatured", item.is_featured},
        {"sortNo", item.sort_no},
    });
  }
  return nlohmann::json{
             {"list", std::move(items)},
             {"total", list.total},
             {"page", list.page},
             {"size", list.size},
         }
      .dump();
}

std::string to_json(const ProblemDetail& detail) {
  nlohmann::json params = nlohmann::json::array();
  for (const ParamRef& param : detail.params) {
    params.push_back({{"name", param.name}, {"cppType", param.cpp_type}});
  }

  nlohmann::json recent = nlohmann::json::array();
  for (const RecentSubmission& entry : detail.recent_submissions) {
    recent.push_back({
        {"id", entry.id},
        {"status", entry.status},
        {"verdict", nullable_text(entry.verdict)},
        {"message", nullable_text(entry.message)},
        {"casePassed", entry.has_case_passed ? nlohmann::json(entry.case_passed) : nlohmann::json(nullptr)},
        {"caseTotal", entry.has_case_total ? nlohmann::json(entry.case_total) : nlohmann::json(nullptr)},
        {"totalMs", entry.has_total_ms ? nlohmann::json(entry.total_ms) : nlohmann::json(nullptr)},
        {"maxMemKb", entry.has_max_mem_kb ? nlohmann::json(entry.max_mem_kb) : nlohmann::json(nullptr)},
        {"createdAt", entry.created_at},
    });
  }

  nlohmann::json body = {
      {"id", detail.id},
      {"slug", detail.slug},
      {"title", detail.title},
      {"difficulty", detail.difficulty},
      {"statementMd", detail.statement_md},
      // ⚠️ 键名是 snake_case：§6.3 端点表把这一项写作 hint_md（与相邻的 isFavorite
      //   等 camelCase 不同）。照抄 SPEC 的拼写 —— 改成 hintMd 就要同步改 T4.20 与 AC-26。
      //   值为 null 表示库里是 NULL ⇒ 前端【不渲染】提示 tab（T4.20 / AC-26）。
      {"hint_md", nullable_text(detail.hint_md)},
      {"signature", detail.signature},
      {"methodName", detail.method_name},
      {"returnType", detail.return_type},
      {"harnessPrelude", nullable_text(detail.harness_prelude)},
      {"codeTemplate", detail.code_template},
      {"params", std::move(params)},
      {"tags", tags_to_json(detail.tags)},
      {"timeLimitMs", detail.time_limit_ms},
      {"memLimitKb", detail.mem_limit_kb},
      {"caseCount", detail.case_count},
      {"isFeatured", detail.is_featured},
      {"sortNo", detail.sort_no},
      {"myProgress", to_string(detail.my_progress)},
      {"isFavorite", detail.is_favorite},
      {"recentSubmissions", std::move(recent)},
  };

  // [D-43] / AC-25：未登录时 nextRecommendation 【整个字段不下发】，
  // 而不是下发 null —— 两者在前端的含义不同（"没有推荐" vs "登录后才有推荐"），
  // 前端据此决定【整张卡不渲染】（T4.22）。
  if (detail.authenticated) {
    if (detail.next_recommendation.has_value()) {
      const NextRecommendation& next = *detail.next_recommendation;
      body["nextRecommendation"] = {
          {"id", next.id},
          {"slug", next.slug},
          {"title", next.title},
          {"difficulty", next.difficulty},
          {"tag", next.tag},
          {"tagSolved", next.tag_solved},
          {"tagTotal", next.tag_total},
          {"reason", next.reason},
      };
    } else {
      body["nextRecommendation"] = nlohmann::json(nullptr);
    }
  }

  return body.dump();
}

}  // namespace web
}  // namespace oj
