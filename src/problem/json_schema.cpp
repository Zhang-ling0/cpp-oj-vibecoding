// =============================================================================
// 题库 JSON 解析与校验的实现（SPEC.md §9 M1 T1.2 / T2.1.1）
//
// 【实现要点】
//   1. 全部检查点共享一个 Checker：它持有 issues 列表，任何检查失败都只是【追加一条】
//      并返回 false，不抛异常、不提前 return —— 理由见 json_schema.h 的【为什么不 fail-fast】。
//   2. 层 ① 的取值辅助在【解析成功后】才调用。nlohmann 的 value() 在键不存在时抛
//      out_of_range，所以每个入口都先 contains()；把这件事集中在辅助函数里，
//      免得每个字段各写一遍 if。
//   3. 层 ② 的 7 条里，第 1/2/4 条（容器深度 / 元素类型 / args 与 params 一一对应）
//      全部落在 model::value_matches_cpp_type 上 —— 与 T1.3 的生成器共用同一套形状口径。
//      本文件【不重复实现】深度比较，这是本轮最关键的一条纪律。
//   4. 【空数组的长度校验】cases[].args 长度为 0 且 params 非空时必然报"参数个数不符"，
//      这里不额外为"空数组"写分支 —— 少一条规则就少一处与生成器不一致的可能。
// =============================================================================
#include "problem/json_schema.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace oj {
namespace problem {
namespace {

// §3.2 的列宽上限。这些校验存在的唯一理由是【响亮地失败】：
// 连接池强制了 STRICT_TRANS_TABLES（否则超长会被静默截断，见 mysql_pool.h 顶部），
// 但静默截断只在"没开严格模式"的机器上发生，而列宽超了本身就是题库抄写错误，
// 早一步报出来比报一条 "Data too long for column 'slug'" 友好得多。
constexpr std::size_t kMaxSlugLen = 64;
constexpr std::size_t kMaxTitleLen = 128;
constexpr std::size_t kMaxMethodNameLen = 64;
constexpr std::size_t kMaxTypeLen = 128;
constexpr std::size_t kMaxParamNameLen = 64;
constexpr std::size_t kMaxBriefLen = 255;
constexpr std::size_t kMaxTagNameLen = 32;   // tags.name VARCHAR(32)
// TEXT 列的上限 = 65535 字节。signature 与 harness_prelude 落在 TEXT（不是 MEDIUMTEXT，
// §3.2），而 MEDIUMTEXT 是 16MB —— 把这两列也按"不限长"处理的话，一道签名超长的题会
// 顺利通过校验，直到 INSERT 那一刻才被 MySQL 用 "Data too long for column 'signature'"
// 拒绝。那时报错出现在【写库阶段】，而不是【录题阶段】，录题的人看到的是一条
// 与题库文件毫无关系的 DB 错误。故这里必须按 TEXT 的真实上限卡。
constexpr std::size_t kMaxTextLen = 65535;
constexpr std::size_t kMaxDifficultyLen = 16;  // ENUM('easy','medium','hard')：6 个字符足够，
                                               // 给 16 是为了留出"手抄成 Easy/Medium"的空间，
                                               // 而不只是为了卡住长度。
// 题库字段的取值上限。time_limit_ms 的 10 分钟与 mem_limit_kb 的 4GB 都是"人不可能真的
// 要这么多"的上限，作用是把抄错的单位（毫秒写成秒的 3000000、KB 写成字节）挡在入库前。
constexpr unsigned int kMaxTimeLimitMs = 600000;
constexpr unsigned int kMaxMemLimitKb = 4194304;

const char* const kAllowedKeys[] = {
    "slug",   "title",       "difficulty",     "statement_md", "hint_md",
    "signature", "method_name", "return_type", "code_template", "time_limit_ms",
    "mem_limit_kb", "harness_prelude", "is_featured", "sort_no", "tags",
    "params", "cases",
};

const char* json_kind(const nlohmann::json& value) {
  if (value.is_null()) return "null";
  if (value.is_boolean()) return "布尔";
  if (value.is_number_integer() || value.is_number_unsigned()) return "整数";
  if (value.is_number_float()) return "浮点数";
  if (value.is_string()) return "字符串";
  if (value.is_array()) return "数组";
  if (value.is_object()) return "对象";
  return "非法值";
}

std::string trim_copy(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

bool is_cpp_identifier(const std::string& text) {
  if (text.empty()) return false;
  const auto head = static_cast<unsigned char>(text.front());
  if (!(std::isalpha(head) || text.front() == '_')) return false;
  for (const char c : text) {
    const auto ch = static_cast<unsigned char>(c);
    if (!(std::isalnum(ch) || c == '_')) return false;
  }
  return true;
}

// URL 友好标识：小写字母 / 数字 / 连字符，首尾必须是字母或数字。
// 为什么卡得比 VARCHAR(64) 紧：这个字符串会直接拼进 /api/problems/<slug>，
// 大写、空格、斜杠都不是"不好看"的问题，而是会破坏路由与前端 URL 拼接的问题。
bool is_url_slug(const std::string& text) {
  if (text.empty() || text.size() > kMaxSlugLen) return false;
  const auto alnum = [](unsigned char c) { return std::isalnum(c) != 0; };
  if (!alnum(static_cast<unsigned char>(text.front()))) return false;
  if (!alnum(static_cast<unsigned char>(text.back()))) return false;
  for (const char c : text) {
    const auto ch = static_cast<unsigned char>(c);
    if (!(std::islower(ch) || std::isdigit(ch) || c == '-')) return false;
  }
  return true;
}

// 收集问题的容器。Checker 不是"校验器"，它只是 issues 的拥有者 + 几组取值辅助 ——
// 把它叫 Checker 是因为这些辅助会追加问题，名字得让人一眼看出"它会记问题"。
class Checker {
 public:
  explicit Checker(std::vector<ValidationIssue>* issues) : issues_(issues) {}

  void add(IssueKind kind, std::string field, std::string message) {
    issues_->push_back(ValidationIssue{kind, std::move(field), std::move(message)});
  }

  // ---- 层 ① 取值辅助 ----------------------------------------------------------
  // max_len 传 0 表示"不限长"，且只用于【MEDIUMTEXT 列】（statement_md / code_template /
  // hint_md，16MB）—— 对它做长度校验没有意义，真有超长内容时截断的代价远比报错大。
  // ⚠️ TEXT 列（signature / harness_prelude，65535 字节）必须显式传 kMaxTextLen：
  //   漏了就会变成"校验放行、写库被拒"，报错现场从录题现场搬到了 DB 现场。

  bool require_string(const nlohmann::json& node, const std::string& path, const char* key,
                      std::size_t max_len, std::string* out) {
    const std::string field = path.empty() ? std::string(key) : path + "." + key;
    if (!node.contains(key)) {
      add(IssueKind::kMissing, field, "缺少必填字段（§4.1）");
      return false;
    }
    const nlohmann::json& value = node.at(key);
    if (!value.is_string()) {
      add(IssueKind::kType, field,
          std::string("必须是字符串，实际是") + json_kind(value));
      return false;
    }
    const std::string text = value.get<std::string>();
    if (trim_copy(text).empty()) {
      add(IssueKind::kMissing, field, "必填但内容为空");
      return false;
    }
    if (text.size() > max_len && max_len != 0) {
      add(IssueKind::kValue, field,
          "长度 " + std::to_string(text.size()) + " 超过上限 " + std::to_string(max_len) +
              "（对应 §3.2 的列宽）");
      return false;
    }
    *out = text;
    return true;
  }

  // 可选字符串：缺失 / null / 空串都得到 out=""（落库时映射为 NULL，见 §4.1）。
  bool optional_string(const nlohmann::json& node, const std::string& path, const char* key,
                       std::size_t max_len, std::string* out) {
    out->clear();
    if (!node.contains(key)) return true;
    const nlohmann::json& value = node.at(key);
    if (value.is_null()) return true;
    if (!value.is_string()) {
      const std::string field = path.empty() ? std::string(key) : path + "." + key;
      add(IssueKind::kType, field, std::string("必须是字符串，实际是") + json_kind(value));
      return false;
    }
    *out = value.get<std::string>();
    if (out->size() > max_len && max_len != 0) {
      const std::string field = path.empty() ? std::string(key) : path + "." + key;
      add(IssueKind::kValue, field,
          "长度 " + std::to_string(out->size()) + " 超过上限 " + std::to_string(max_len) +
              "（对应 §3.2 的列宽）");
      return false;
    }
    return true;
  }

  bool optional_uint(const nlohmann::json& node, const std::string& path, const char* key,
                     unsigned int fallback, unsigned int min_value, unsigned int max_value,
                     unsigned int* out) {
    *out = fallback;
    if (!node.contains(key)) return true;
    const nlohmann::json& value = node.at(key);
    const std::string field = path.empty() ? std::string(key) : path + "." + key;
    // JSON 里 3000.0 也可能被写成浮点；整数列接受"值恰为整数"的浮点，
    // 但不接受负数与非数值（那两种在 STRICT 模式下会变成看不懂的 Data truncated）。
    if (!value.is_number_integer() && !value.is_number_unsigned() && !value.is_number_float()) {
      add(IssueKind::kType, field, std::string("必须是整数，实际是") + json_kind(value));
      return false;
    }
    const double raw = value.get<double>();
    if (raw < 0.0 || raw != static_cast<double>(static_cast<long long>(raw))) {
      add(IssueKind::kValue, field, "必须是非负整数，实际是 " + value.dump());
      return false;
    }
    const auto number = static_cast<unsigned int>(raw);
    if (number < min_value || number > max_value) {
      add(IssueKind::kValue, field,
          "取值 " + std::to_string(number) + " 超出允许区间 [" +
              std::to_string(min_value) + ", " + std::to_string(max_value) + "]");
      return false;
    }
    *out = number;
    return true;
  }

  bool optional_int(const nlohmann::json& node, const std::string& path, const char* key,
                    int fallback, int* out) {
    *out = fallback;
    if (!node.contains(key)) return true;
    const nlohmann::json& value = node.at(key);
    const std::string field = path.empty() ? std::string(key) : path + "." + key;
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
      add(IssueKind::kType, field, std::string("必须是整数，实际是") + json_kind(value));
      return false;
    }
    const long long raw = value.get<long long>();
    if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
      add(IssueKind::kValue, field, "超出 32 位有符号整数范围");
      return false;
    }
    *out = static_cast<int>(raw);
    return true;
  }

  bool optional_bool(const nlohmann::json& node, const std::string& path, const char* key,
                     bool fallback, bool* out) {
    *out = fallback;
    if (!node.contains(key)) return true;
    const nlohmann::json& value = node.at(key);
    const std::string field = path.empty() ? std::string(key) : path + "." + key;
    if (!value.is_boolean()) {
      add(IssueKind::kType, field, std::string("必须是 true/false，实际是") + json_kind(value));
      return false;
    }
    *out = value.get<bool>();
    return true;
  }

  bool fail_consistency(std::string field, std::string message) {
    add(IssueKind::kConsistency, std::move(field), std::move(message));
    return false;
  }

 private:
  std::vector<ValidationIssue>* issues_;
};

// 题面禁图片 [D-48]：判据是 Markdown 的图片语法起点 "!["，而不是匹配整条语法。
//   理由：录题的人写 "![" 时八成就是想插图，我们要给的信息是"这里会被前端拒绝"，
//   至于后面有没有配合法的 () 与路径，DOMPurify 白名单一律不允许 img（D-48 明确）。
void check_no_image(Checker* checker, const std::string& field, const std::string& markdown) {
  if (markdown.find("![") == std::string::npos) return;
  checker->add(IssueKind::kImage, field,
               "题面禁图片 [D-48]：树/图/矩阵请用 pre 代码块里的字符画（保留等宽对齐）");
}

// ---- tags --------------------------------------------------------------------

void parse_tags(const nlohmann::json& root, Checker* checker, model::ProblemBundle* out) {
  if (!root.contains("tags")) return;  // 可选：没有标签的题不出现在任何标签筛选里
  const nlohmann::json& tags = root.at("tags");
  if (!tags.is_object()) {
    checker->add(IssueKind::kType, "tags", std::string("必须是对象，实际是") + json_kind(tags));
    return;
  }
  for (auto it = tags.begin(); it != tags.end(); ++it) {
    const std::string& key = it.key();
    model::TagCategory category = model::TagCategory::kAlgorithm;
    if (!model::parse_tag_category(key, &category)) {
      checker->add(IssueKind::kValue, "tags." + key,
                   "未知标签类别，只允许 algorithm / source（§3.2 tags.category 的 ENUM）");
      continue;
    }
    if (!it.value().is_array()) {
      checker->add(IssueKind::kType, "tags." + key,
                   std::string("必须是数组，实际是") + json_kind(it.value()));
      continue;
    }
    std::vector<std::string>& target =
        (category == model::TagCategory::kAlgorithm) ? out->algorithm_tags : out->source_tags;
    for (std::size_t i = 0; i < it.value().size(); ++i) {
      const std::string path = "tags." + key + "[" + std::to_string(i) + "]";
      const nlohmann::json& item = it.value().at(i);
      if (!item.is_string()) {
        checker->add(IssueKind::kType, path, std::string("标签必须是字符串，实际是") +
                                                  json_kind(item));
        continue;
      }
      const std::string name = trim_copy(item.get<std::string>());
      if (name.empty()) {
        checker->add(IssueKind::kValue, path, "标签名不能为空");
        continue;
      }
      if (name.size() > kMaxTagNameLen) {
        checker->add(IssueKind::kValue, path,
                     "标签名长度 " + std::to_string(name.size()) + " 超过 " +
                         std::to_string(kMaxTagNameLen) + "（tags.name 列宽）");
        continue;
      }
      // 去重：tags 有 UNIQUE(name, category)，重复项若留到写库阶段只能靠 INSERT IGNORE
      // 吞掉 —— 那样会让"题库里写了两遍同一个标签"这件事彻底不可见。这里静默去重，
      // 保留首次出现的顺序。
      if (std::find(target.begin(), target.end(), name) == target.end()) {
        target.push_back(name);
      }
    }
  }
}

// ---- params（层 ① 非空 + 层 ② 的形参合法性）---------------------------------

bool parse_params(const nlohmann::json& root, Checker* checker,
                  std::vector<model::ProblemParam>* out) {
  if (!root.contains("params")) {
    checker->add(IssueKind::kMissing, "params", "缺少必填字段（§4.1）");
    return false;
  }
  const nlohmann::json& params = root.at("params");
  if (!params.is_array()) {
    checker->add(IssueKind::kType, "params", std::string("必须是数组，实际是") + json_kind(params));
    return false;
  }
  if (params.empty()) {
    checker->add(IssueKind::kMissing, "params", "至少要有一个形参（层 ② 第 6 条）");
    return false;
  }

  bool ok = true;
  std::set<std::string> seen_names;
  for (std::size_t i = 0; i < params.size(); ++i) {
    const std::string path = "params[" + std::to_string(i) + "]";
    const nlohmann::json& item = params.at(i);
    if (!item.is_object()) {
      checker->add(IssueKind::kType, path, std::string("必须是对象，实际是") + json_kind(item));
      ok = false;
      continue;
    }
    model::ProblemParam param;
    param.param_idx = static_cast<unsigned int>(i);   // param_idx = 数组下标（§4.1 映射表）
    ok &= checker->require_string(item, path, "name", kMaxParamNameLen, &param.name);
    ok &= checker->require_string(item, path, "cpp_type", kMaxTypeLen, &param.cpp_type);

    if (!param.name.empty()) {
      if (!is_cpp_identifier(param.name)) {
        ok = checker->fail_consistency(path + ".name",
                                       "形参名 \"" + param.name +
                                           "\" 不是合法的 C++ 标识符（生成代码里会被当成变量声明）");
      } else if (!seen_names.insert(param.name).second) {
        // 重名形参会让生成的 tester.cpp 里出现 solution.twoSum(nums, nums) 这类调用，
        // 而 user.cpp 那边是两个同名形参 —— 必然编译失败，且报错指向 harness（D-64）。
        ok = checker->fail_consistency(path + ".name",
                                       "形参名 \"" + param.name + "\" 与前面的形参重名");
      }
    }
    if (!param.cpp_type.empty()) {
      const model::CppTypeShape shape = model::analyze_cpp_type(param.cpp_type);
      if (!shape.well_formed) {
        ok = checker->fail_consistency(
            path + ".cpp_type",
            "cpp_type \"" + param.cpp_type +
                "\" 解析不出元素类型；支持 std::vector<T> 的多层嵌套、可带 const / & / *");
      }
    }
    out->push_back(std::move(param));
  }
  return ok;
}

// ---- cases（层 ② 第 1/2/3/4 条的主体）-----------------------------------------

bool parse_cases(const nlohmann::json& root, Checker* checker,
                 const std::vector<model::ProblemParam>& params,
                 const std::string& return_type, model::ProblemBundle* out) {
  if (!root.contains("cases")) {
    checker->add(IssueKind::kMissing, "cases", "缺少必填字段（§4.1）");
    return false;
  }
  const nlohmann::json& cases = root.at("cases");
  if (!cases.is_array()) {
    checker->add(IssueKind::kType, "cases", std::string("必须是数组，实际是") + json_kind(cases));
    return false;
  }
  if (cases.empty()) {
    checker->add(IssueKind::kMissing, "cases", "至少要有一个用例（层 ② 第 6 条）");
    return false;
  }

  bool ok = true;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const std::string path = "cases[" + std::to_string(i) + "]";
    const nlohmann::json& item = cases.at(i);
    if (!item.is_object()) {
      checker->add(IssueKind::kType, path, std::string("必须是对象，实际是") + json_kind(item));
      ok = false;
      continue;
    }
    model::ProblemCase test_case;
    test_case.case_idx = static_cast<unsigned int>(i);  // case_idx = 数组下标（§4.1 映射表）

    // brief 可选；空串 → 落库 NULL
    ok &= checker->optional_string(item, path, "brief", kMaxBriefLen, &test_case.brief);

    // 层 ② 第 3 条：args 与 params 一一对应
    const bool has_args = item.contains("args");
    if (!has_args) {
      checker->add(IssueKind::kMissing, path + ".args", "用例缺少 args");
      ok = false;
    } else if (!item.at("args").is_array()) {
      checker->add(IssueKind::kType, path + ".args",
                   std::string("必须是数组（元素与 params 一一对应），实际是") +
                       json_kind(item.at("args")));
      ok = false;
    } else if (item.at("args").size() != params.size()) {
      ok = checker->fail_consistency(
          path + ".args",
          "参数个数 " + std::to_string(item.at("args").size()) + " 与形参个数 " +
              std::to_string(params.size()) + " 不符（层 ② 第 3 条）");
    } else {
      for (std::size_t j = 0; j < item.at("args").size(); ++j) {
        const std::string arg_path = path + ".args[" + std::to_string(j) + "]";
        std::string reason;
        if (!model::value_matches_cpp_type(item.at("args").at(j),
                                           params[j].cpp_type, arg_path, &reason)) {
          ok = checker->fail_consistency(arg_path, reason + "（层 ② 第 4 条）");
        }
      }
    }
    if (has_args && item.at("args").is_array()) test_case.args = item.at("args");

    // 层 ② 第 1/2 条：expected 的容器深度与元素类型要对得上 return_type
    if (!item.contains("expected")) {
      checker->add(IssueKind::kMissing, path + ".expected", "用例缺少 expected");
      ok = false;
    } else {
      const std::string exp_path = path + ".expected";
      std::string reason;
      if (!model::value_matches_cpp_type(item.at("expected"), return_type, exp_path, &reason)) {
        ok = checker->fail_consistency(exp_path, reason + "（层 ② 第 1/2 条）");
      }
      test_case.expected = item.at("expected");
    }

    out->cases.push_back(std::move(test_case));
  }
  return ok;
}

// 层 ② 第 5 条：method_name 必须在 signature 中【作为独立标识符】出现。
//   朴素子串匹配会在 signature 里写 twoSumPair 时把 twoSum 也判成"出现过"，
//   于是 harness 生成出一个签名里没有的方法调用，错误推迟到编译期，
//   且按 D-64 归属到 harness —— 用户看到的文案是"题库配置有误"，但真正的原因是录题笔误。
bool signature_declares_method(const std::string& signature, const std::string& method_name) {
  if (method_name.empty()) return false;
  std::size_t from = 0;
  for (;;) {
    const std::size_t found = signature.find(method_name, from);
    if (found == std::string::npos) return false;
    const auto before = found == 0 ? 0 : static_cast<unsigned char>(signature[found - 1]);
    const std::size_t after_pos = found + method_name.size();
    const auto after = after_pos >= signature.size() ? 0
                                                     : static_cast<unsigned char>(signature[after_pos]);
    const bool left_ok = found == 0 || !(std::isalnum(before) || before == '_');
    const bool right_ok = after_pos >= signature.size() || !(std::isalnum(after) || after == '_');
    if (left_ok && right_ok) return true;
    from = found + 1;
  }
}

}  // namespace

// ---- 对外接口 -----------------------------------------------------------------

const char* to_string(IssueKind kind) {
  switch (kind) {
    case IssueKind::kJson: return "JSON 错误";
    case IssueKind::kMissing: return "必填缺失";
    case IssueKind::kType: return "类型错误";
    case IssueKind::kValue: return "取值错误";
    case IssueKind::kUnknownField: return "未知字段";
    case IssueKind::kConsistency: return "类型不一致";
    case IssueKind::kImage: return "禁图片";
  }
  return "未知";
}

std::string render_issue(const ValidationIssue& issue) {
  std::string text;
  if (!issue.field.empty()) {
    text += issue.field;
    text += ": ";
  }
  text += issue.message;
  text += "（";
  text += to_string(issue.kind);
  text += "）";
  return text;
}

std::string format_issues(const std::vector<ValidationIssue>& issues,
                          const std::string& source) {
  std::ostringstream out;
  for (const ValidationIssue& issue : issues) {
    out << source << ": " << render_issue(issue) << "\n";
  }
  return out.str();
}

ParseResult parse_problem_json(const std::string& text, const ParseOptions& options) {
  ParseResult result;
  Checker checker(&result.issues);
  model::ProblemBundle& problem = result.problem;

  nlohmann::json root = nullptr;
  if (trim_copy(text).empty()) {
    checker.add(IssueKind::kJson, "", "文件为空（0 字节）。若是新建的题库文件，先按 §4.1 填写");
    return result;
  }
  try {
    root = nlohmann::json::parse(text);
  } catch (const nlohmann::json::exception& error) {
    checker.add(IssueKind::kJson, "", std::string("JSON 解析失败：") + error.what());
    return result;
  }
  if (root.is_discarded() || !root.is_object()) {
    checker.add(IssueKind::kJson, "",
                std::string("顶层必须是 JSON 对象，实际是") + json_kind(root));
    return result;
  }

  // ---- 未知顶层字段 -----------------------------------------------------------
  // §4.1 的字段表是【封闭】的：多出来的键要么是拼写错误（statement 写成 statements_md），
  // 要么是抄了别处的题。两种情况下继续解析只会让拼错的那个字段"看起来没填"，
  // 报错还指向另一个字段 —— 比直接点名未知键难查得多。
  for (auto it = root.begin(); it != root.end(); ++it) {
    bool known = false;
    for (const char* key : kAllowedKeys) {
      if (it.key() == key) {
        known = true;
        break;
      }
    }
    if (!known) {
      checker.add(IssueKind::kUnknownField, it.key(),
                  "§4.1 的字段表里没有这个字段（若是拼写错误请改正；多余字段不会被写库）");
    }
  }

  // ---- 层 ①：必填字段与值域 ---------------------------------------------------
  if (checker.require_string(root, "", "slug", kMaxSlugLen, &problem.slug)) {
    result.slug = problem.slug;   // slug 读到了就先记下（失败时 problem 会被清空）
    if (!is_url_slug(problem.slug)) {
      checker.add(IssueKind::kValue, "slug",
                  "\"" + problem.slug +
                      "\" 不是 URL 友好标识：只允许小写字母、数字与连字符，且首尾是字母或数字");
    } else if (!options.expect_slug.empty() && problem.slug != options.expect_slug) {
      checker.add(IssueKind::kValue, "slug",
                  "slug \"" + problem.slug + "\" 与文件名 \"" + options.expect_slug +
                      "\" 不一致（题目 URL 由文件名决定，改其一即可）");
    }
  }

  checker.require_string(root, "", "title", kMaxTitleLen, &problem.title);
  checker.require_string(root, "", "signature", kMaxTextLen, &problem.signature);
  checker.require_string(root, "", "method_name", kMaxMethodNameLen, &problem.method_name);
  checker.require_string(root, "", "return_type", kMaxTypeLen, &problem.return_type);
  checker.require_string(root, "", "statement_md", 0, &problem.statement_md);
  checker.require_string(root, "", "code_template", 0, &problem.code_template);
  // harness_prelude / hint_md 可选：缺省或空串都映射为 NULL（§4.1 的两条脚注）。
  // hint_md 是 MEDIUMTEXT（16MB）故不限长；harness_prelude 是 TEXT，按 65535 卡。
  checker.optional_string(root, "", "harness_prelude", kMaxTextLen, &problem.harness_prelude);
  checker.optional_string(root, "", "hint_md", 0, &problem.hint_md);

  std::string difficulty_text;
  if (checker.require_string(root, "", "difficulty", kMaxDifficultyLen, &difficulty_text)) {
    if (!model::parse_difficulty(difficulty_text, &problem.difficulty)) {
      checker.add(IssueKind::kValue, "difficulty",
                  "\"" + difficulty_text + "\" 不是 easy/medium/hard（§3.2 的 ENUM）");
    }
  }

  if (!problem.method_name.empty() && !is_cpp_identifier(problem.method_name)) {
    checker.add(IssueKind::kConsistency, "method_name",
                "\"" + problem.method_name + "\" 不是合法的 C++ 标识符");
  }

  checker.optional_uint(root, "", "time_limit_ms", 3000, 1, kMaxTimeLimitMs,
                        &problem.time_limit_ms);
  checker.optional_uint(root, "", "mem_limit_kb", 131072, 1, kMaxMemLimitKb, &problem.mem_limit_kb);
  checker.optional_bool(root, "", "is_featured", false, &problem.is_featured);
  checker.optional_int(root, "", "sort_no", 0, &problem.sort_no);

  if (options.forbid_images) {
    check_no_image(&checker, "statement_md", problem.statement_md);
    check_no_image(&checker, "hint_md", problem.hint_md);
  }

  // ---- 层 ②：类型一致性静态校验（D-64 / §5.6.1 的 7 条）------------------------
  parse_params(root, &checker, &problem.params);
  parse_tags(root, &checker, &problem);
  if (!problem.return_type.empty()) {
    parse_cases(root, &checker, problem.params, problem.return_type, &problem);
  }

  // 第 5 条：method_name 必须在 signature 中作为独立标识符出现
  if (!problem.method_name.empty() && !problem.signature.empty() &&
      !signature_declares_method(problem.signature, problem.method_name)) {
    checker.fail_consistency("method_name",
                             "signature 里找不到方法 " + problem.method_name +
                                 "（harness 要调用它；检查是否写成了别的名字，或漏了它）");
  }

  // 第 7 条：用了 __expr 就必须有非空 harness_prelude
  if (problem.uses_expr() && problem.harness_prelude.empty()) {
    checker.fail_consistency("harness_prelude",
                             "用例里用到了 {\"__expr\": …} 逃生舱，但 harness_prelude 为空；"
                             "构造函数/辅助函数的声明必须写在 prelude 里（§4.2）");
  }

  result.ok = result.issues.empty();
  if (!result.ok) {
    result.problem = model::ProblemBundle();  // 半份数据比空数据更危险，调用点一律看 ok
  }
  return result;
}

ParseResult parse_problem_file(const std::string& path, const std::string& expect_slug) {
  ParseOptions options;
  options.source = path;
  options.expect_slug = expect_slug;

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    ParseResult result;
    result.issues.push_back(
        ValidationIssue{IssueKind::kJson, "", "读不到文件（权限 / 路径 / 是目录？）"});
    return result;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input && !input.eof()) {
    ParseResult result;
    result.issues.push_back(ValidationIssue{IssueKind::kJson, "", "读取文件出错"});
    return result;
  }
  return parse_problem_json(buffer.str(), options);
}

}  // namespace problem
}  // namespace oj
