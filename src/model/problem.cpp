// =============================================================================
// 题目数据模型与形状推导的实现
// 被测对象：include/model/problem.h（SPEC.md §9 M1 T1.2 / T1.4）
//
// 【实现要点】
//   1. analyze_cpp_type 是【纯文本解析】，不碰 nlohmann：它吃的是 problem_params.cpp_type
//      这一列的文本，DB 侧与 JSON 侧共用同一个函数，不存在"两份推导"的问题。
//   2. 匹配逻辑只有一个递归函数 match_shape()，深度由 CppTypeShape::vector_depth 递减。
//      写成"先比深度、再比元素类型"两段会重复遍历，并且二者的深度定义一旦漂移
//      （一个按最深层、一个按首层）就会互相矛盾 —— 本文件全程只用"剥一层"这一个口径。
//   3. 无法静态判定的类型【放行】而不是猜。这是 §5.6.1「静态校验查不出 __expr 内容」
//      的直接延伸：能查的查干净，查不了的留给 §5.6 的 harness 归属分流兜底。
// =============================================================================
#include "model/problem.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace oj {
namespace model {
namespace {

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

// JSON 值的种类描述（给人看的错误消息用）。刻意写中文：这些字符串的读者是录题的人。
const char* describe_json_value(const nlohmann::json& value) {
  if (value.is_null()) return "null";
  if (value.is_boolean()) return "布尔";
  if (value.is_number_integer() || value.is_number_unsigned()) return "整数";
  if (value.is_number_float()) return "浮点数";
  if (value.is_string()) return "字符串";
  if (value.is_array()) return "数组";
  if (value.is_object()) return "对象";
  return "已丢弃的非法值";
}

// 可静态判定 JSON 元素类型的那批基础类型（§4.2 映射表里【没有对应 JSON 形态】的类型
// 一律归 kUnknown，即放行）。char / size_t 故意不在表内：§4.2 未定义它们该写成
// JSON 字符串还是数字，猜错会把合法题库判死 —— 放行的代价只是少发现一类错误，
// 而误判合法题库的代价是那道题从列表里消失（§5.6.1）。
enum class ScalarClass { kUnknown, kInteger, kUnsignedInteger, kFloating, kBool, kString };

ScalarClass classify_scalar(const std::string& base_type) {
  // 有符号整数
  if (base_type == "int" || base_type == "short" || base_type == "long" ||
      base_type == "long long") {
    return ScalarClass::kInteger;
  }
  // 无符号整数单独一类：-1 必须被挡住（见 match_shape 的注释）
  if (base_type == "unsigned" || base_type == "unsigned short" ||
      base_type == "unsigned int" || base_type == "unsigned long" ||
      base_type == "unsigned long long") {
    return ScalarClass::kUnsignedInteger;
  }
  if (base_type == "float" || base_type == "double" || base_type == "long double") {
    return ScalarClass::kFloating;
  }
  if (base_type == "bool") return ScalarClass::kBool;
  if (base_type == "std::string" || base_type == "string") return ScalarClass::kString;
  return ScalarClass::kUnknown;
}

// 唯一的匹配实现：depth 从 shape.vector_depth 递减到 0 为止。
bool match_shape(const nlohmann::json& value, const CppTypeShape& shape, int depth,
                 const std::string& path, const std::string& cpp_type, std::string* reason) {
  const auto fail = [&](const std::string& detail) {
    if (reason != nullptr) {
      *reason = path + "：" + detail + "（cpp_type=" + cpp_type + "，期望 " +
                describe_cpp_shape(shape) + "）";
    }
    return false;
  };

  if (depth > 0) {
    if (!value.is_array()) {
      return fail("该位置要求 std::vector（还剩 " + std::to_string(depth) +
                  " 层嵌套），实际是" + describe_json_value(value));
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (!match_shape(value[i], shape, depth - 1, path + "[" + std::to_string(i) + "]",
                       cpp_type, reason)) {
        return false;
      }
    }
    return true;
  }

  const ScalarClass cls = classify_scalar(shape.base_type);
  switch (cls) {
    case ScalarClass::kInteger:
      if (value.is_number_integer() || value.is_number_unsigned()) return true;
      return fail(std::string("该位置要求整数（") + shape.base_type + "），实际是" +
                  describe_json_value(value));
    case ScalarClass::kUnsignedInteger:
      // ⚠️ 必须单独判负：nlohmann 把 JSON 里的 -1 解析成 number_integer，
      // 而 `is_number_integer()` 对它为真 —— 若与有符号整数共用一个分支，
      // `unsigned` 形参会收下 -1，生成的字面量 `-1` 赋给 std::vector<unsigned>
      // 会变成巨大的正数，用例在"看起来完全合理"的题面上永远判 WA，
      // 而且报错指向用户的代码（"你的输出和期望不符"），实际是题库写错了。
      if (value.is_number_unsigned()) return true;
      if (value.is_number_integer()) {
        if (value.get<long long>() >= 0) return true;
        return fail(std::string("该位置要求非负整数（") + shape.base_type + "），实际是 " +
                    value.dump());
      }
      return fail(std::string("该位置要求非负整数（") + shape.base_type + "），实际是" +
                  describe_json_value(value));
    case ScalarClass::kFloating:
      if (value.is_number()) return true;
      return fail(std::string("该位置要求浮点数（") + shape.base_type + "），实际是" +
                  describe_json_value(value));
    case ScalarClass::kBool:
      if (value.is_boolean()) return true;
      return fail(std::string("该位置要求布尔（") + shape.base_type + "），实际是" +
                  describe_json_value(value));
    case ScalarClass::kString:
      if (value.is_string()) return true;
      return fail(std::string("该位置要求字符串（") + shape.base_type + "），实际是" +
                  describe_json_value(value));
    case ScalarClass::kUnknown:
      break;
  }

  // 自定义类型：JSON null → nullptr 是 §4.2 映射表里明确写了的；
  // 数组值一定错（没有"用 JSON 数组构造 TreeNode*"这种写法，要构造就得用 __expr）；
  // 其余标量形式一概放行 —— 我们无法知道枚举/结构体该长什么样。
  if (value.is_null()) return true;
  if (value.is_array() || value.is_object()) {
    return fail(std::string("自定义类型只能由 {\"__expr\": \"…\"} 构造（§4.2 逃生舱），实际是") +
                describe_json_value(value));
  }
  return true;
}

}  // namespace

// ---- 枚举 ---------------------------------------------------------------------

const char* to_string(Difficulty value) {
  switch (value) {
    case Difficulty::kEasy: return "easy";
    case Difficulty::kMedium: return "medium";
    case Difficulty::kHard: return "hard";
  }
  return "easy";
}

bool parse_difficulty(const std::string& text, Difficulty* out) {
  const std::string key = to_lower(trim(text));
  Difficulty parsed = Difficulty::kEasy;
  if (key == "easy") {
    parsed = Difficulty::kEasy;
  } else if (key == "medium") {
    parsed = Difficulty::kMedium;
  } else if (key == "hard") {
    parsed = Difficulty::kHard;
  } else {
    return false;
  }
  if (out != nullptr) *out = parsed;
  return true;
}

const char* to_string(TagCategory value) {
  return value == TagCategory::kAlgorithm ? "algorithm" : "source";
}

bool parse_tag_category(const std::string& text, TagCategory* out) {
  const std::string key = to_lower(trim(text));
  TagCategory parsed = TagCategory::kAlgorithm;
  if (key == "algorithm") {
    parsed = TagCategory::kAlgorithm;
  } else if (key == "source") {
    parsed = TagCategory::kSource;
  } else {
    return false;
  }
  if (out != nullptr) *out = parsed;
  return true;
}

// ---- 题目侧形参 / 用例 / 题目 -------------------------------------------------

int ProblemParam::vector_depth() const {
  return analyze_cpp_type(cpp_type).vector_depth;
}

std::vector<std::string> ProblemBundle::tags_of(TagCategory category) const {
  return category == TagCategory::kAlgorithm ? algorithm_tags : source_tags;
}

const ProblemParam* ProblemBundle::param(unsigned int idx) const {
  if (idx >= params.size()) return nullptr;
  return &params[idx];
}

const ProblemCase* ProblemBundle::test_case(unsigned int idx) const {
  if (idx >= cases.size()) return nullptr;
  return &cases[idx];
}

bool ProblemBundle::uses_expr() const {
  for (const ProblemCase& test_case : cases) {
    if (is_expr_value(test_case.expected)) return true;
    for (const nlohmann::json& arg : test_case.args) {
      if (is_expr_value(arg)) return true;
    }
  }
  return false;
}

// ---- 形状推导 -----------------------------------------------------------------

CppTypeShape analyze_cpp_type(const std::string& cpp_type) {
  CppTypeShape shape;
  // 第一步：抹掉全部空白。cpp_type 是要原样贴进生成代码的，这里的规整只服务于解析，
  // 落库时仍存原文（§4.1 映射表：problem_params.cpp_type 原样保存）。
  std::string rest;
  rest.reserve(cpp_type.size());
  for (const char c : cpp_type) {
    if (!std::isspace(static_cast<unsigned char>(c))) rest.push_back(c);
  }

  // 第二步：剥掉 std::vector<...> 层。带嵌套的 < > 用计数配对，
  // 不能用 find('>') —— std::vector<std::vector<int>> 的第一个 '>' 是内层的。
  for (;;) {
    const std::size_t open = rest.find('<');
    if (open == std::string::npos) break;
    std::string prefix = rest.substr(0, open);
    // 允许 std::vector 与 vector 两种写法：code_template 里普遍有 using namespace std;，
    // 两边混着写时不必为了统一而改题库。
    if (!prefix.empty() && prefix.back() == ':') prefix.pop_back();
    if (prefix != "std::vector" && prefix != "vector") break;  // 不是容器，原样当元素类型

    std::size_t depth = 0;
    std::size_t close = std::string::npos;
    for (std::size_t i = open; i < rest.size(); ++i) {
      if (rest[i] == '<') {
        ++depth;
      } else if (rest[i] == '>') {
        --depth;
        if (depth == 0) {
          close = i;
          break;
        }
      }
    }
    if (close == std::string::npos) return shape;  // 括号不配对 → well_formed 保持 false
    shape.vector_depth += 1;
    rest = rest.substr(open + 1, close - open - 1);
  }

  // 第三步：元素类型上剥 const / 引用 / 指针。
  std::string base = rest;
  while (base.rfind("const", 0) == 0) base = base.substr(5);
  while (!base.empty()) {
    const char tail = base.back();
    if (tail == '&') {
      base.pop_back();
    } else if (tail == '*') {
      shape.is_pointer = true;
      base.pop_back();
    } else {
      break;
    }
  }

  shape.base_type = base;
  // 还残留 '<' 说明是个我们不认识的模板（std::map<…> / 模板别名）：
  // 判为不合形，交给必填字段校验报错，而不是"当成一种类型放行"。
  shape.well_formed = !base.empty() && base.find('<') == std::string::npos;
  shape.scalar_known = shape.well_formed && classify_scalar(base) != ScalarClass::kUnknown;
  return shape;
}

std::string describe_cpp_shape(const CppTypeShape& shape) {
  std::string inner = shape.base_type.empty() ? std::string("<未知名>") : shape.base_type;
  for (int i = 0; i < shape.vector_depth; ++i) inner = "std::vector<" + inner + ">";
  if (shape.is_pointer) inner += "*";
  return inner;
}

bool is_expr_value(const nlohmann::json& value) {
  if (!value.is_object()) return false;
  const auto it = value.find("__expr");
  if (it == value.end() || !it->is_string()) return false;
  return value.size() == 1;
}

JsonShape analyze_json(const nlohmann::json& value) {
  JsonShape shape;
  shape.is_null = value.is_null();
  if (value.is_object()) {
    shape.is_expr = is_expr_value(value);
    return shape;
  }
  if (value.is_array()) {
    // 取【最深处】：[[1,2],3] 的深度是 2 而不是 1。与 match_shape 的"逐层剥"口径一致。
    int deepest = 1;
    for (const nlohmann::json& element : value) {
      if (!element.is_array()) continue;
      deepest = std::max(deepest, 1 + analyze_json(element).array_depth);
    }
    shape.array_depth = deepest;
    return shape;
  }
  shape.is_scalar = value.is_boolean() || value.is_number() || value.is_string();
  return shape;
}

bool value_matches_cpp_type(const nlohmann::json& value, const std::string& cpp_type,
                            const std::string& json_path, std::string* reason) {
  // __expr 逃生舱恒放行：它的语义就是"这段文本我自己负责"，§4.2.1 也把它列在
  // 生成期校验之外（只校验"是否配了 prelude"，不校验表达式本身）。
  if (is_expr_value(value)) return true;

  const CppTypeShape shape = analyze_cpp_type(cpp_type);
  if (!shape.well_formed) {
    if (reason != nullptr) {
      *reason = json_path + "：cpp_type \"" + cpp_type +
                "\" 不是受支持的类型写法（支持 std::vector<T> 的嵌套、const、& 与 *）";
    }
    return false;
  }
  return match_shape(value, shape, shape.vector_depth, json_path, cpp_type, reason);
}

nlohmann::json parse_case_text(const std::string& text, std::string* reason) {
  try {
    nlohmann::json parsed = nlohmann::json::parse(text);
    if (parsed.is_discarded()) {
      if (reason != nullptr) *reason = "文本不是合法 JSON";
      return nullptr;
    }
    return parsed;
  } catch (const nlohmann::json::exception& error) {
    if (reason != nullptr) *reason = error.what();
    return nullptr;
  }
}

}  // namespace model
}  // namespace oj
