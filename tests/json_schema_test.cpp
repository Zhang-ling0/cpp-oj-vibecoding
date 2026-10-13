// =============================================================================
// 题库 JSON 解析与校验的单元测试（gtest）
// 被测：include/problem/json_schema.h + src/problem/json_schema.cpp（T1.2 的校验层）
//       include/model/problem.h + src/model/problem.cpp（T1.4 的形状推导原语）
// 运行：tests/run_unit_tests.sh（即 ctest）
//
// 【为什么用「逐字段改写」而不是「维护一批 fixture 文件」】
//   本批用例的价值全在【同一份题库的各个变体】上：把 return_type 改成 int、把
//   cases[].args 少写一个、给题面塞一张图。每存一个 fixture 文件，就多一份会与
//   §4.1 的字段表悄悄漂移的副本；而这里每个用例只写【它要破坏的那一处】，
//   其余字段由 valid_doc() 提供 —— 断言失败时"是哪一处坏了"一目了然。
//
// 【不测什么】
//   · 落库结果与幂等同步 → problem_loader_test.cpp（需要真实 MySQL）
//   · JSON 值 → C++ 字面量的转换（T1.3 生成器）
//   · 题面内容质量（提示是不是剧透、题面有没有歧义）—— 那是人的判断，不是校验器的
// =============================================================================
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/problem.h"
#include "problem/json_schema.h"

namespace {

using oj::model::CppTypeShape;
using oj::problem::IssueKind;
using oj::problem::ParseOptions;
using oj::problem::ParseResult;
using oj::problem::ValidationIssue;
using oj::problem::parse_problem_json;
using oj::problem::render_issue;

// ---- 题库文本的构造与改写 ------------------------------------------------------

// 生成合法的 JSON 字符串字面量（含必要的转义）。非 ASCII 不转义 —— UTF-8 原样写在
// JSON 字符串里是合法的，而题库全是中文，转义成 \uXXXX 只会让测试读不懂。
std::string quoted(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        const auto ch = static_cast<unsigned char>(c);
        if (ch < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(ch >> 4) & 0xF];
          out += kHex[ch & 0xF];
        } else {
          out += c;
        }
      }
    }
  }
  out += "\"";
  return out;
}

// 一份可逐字段改写的题库。值以【原始 JSON 片段】保存，因此测试能故意塞进错类型
// （title 写成 42、cases 写成 {}），而不必先构造合法值再想办法弄坏它。
class ProblemDoc {
 public:
  ProblemDoc& set_text(const std::string& key, const std::string& text) {
    return set_raw(key, quoted(text));
  }
  ProblemDoc& set_raw(const std::string& key, const std::string& raw_json) {
    for (auto& entry : entries_) {
      if (entry.first == key) {
        entry.second = raw_json;
        return *this;
      }
    }
    entries_.emplace_back(key, raw_json);
    return *this;
  }
  ProblemDoc& drop(const std::string& key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->first == key) {
        entries_.erase(it);
        return *this;
      }
    }
    return *this;
  }
  std::string text() const {
    std::string out = "{";
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (i != 0) out += ",";
      out += quoted(entries_[i].first) + ":" + entries_[i].second;
    }
    return out + "}";
  }

  // 从一段合法 JSON 对象文本构造（用于「最小题库」这类独立场景）
  static ProblemDoc from(const std::string& json_text) {
    ProblemDoc doc;
    const nlohmann::json parsed = nlohmann::json::parse(json_text);
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
      doc.entries_.emplace_back(it.key(), it.value().dump());
    }
    return doc;
  }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

// 一份【合法】题库：two-sum（§4.1 的示例题），每条用例只破坏它关心的那一处。
ProblemDoc valid_doc() {
  ProblemDoc doc;
  doc.set_text("slug", "two-sum")
      .set_text("title", "两数之和")
      .set_text("difficulty", "medium")
      .set_text("statement_md", "## 题目描述\n\n给定数组 nums 与 target")
      .set_text("hint_md", "边遍历边记录已经见过的值")
      .set_text("signature",
                "class Solution {\npublic:\n    std::vector<int> twoSum("
                "std::vector<int>& nums, int target);\n};")
      .set_text("method_name", "twoSum")
      .set_raw("params",
               R"([{"name":"nums","cpp_type":"std::vector<int>&"},)"
               R"({"name":"target","cpp_type":"int"}])")
      .set_text("return_type", "std::vector<int>")
      .set_text("code_template", "#include <vector>\nclass Solution {};")
      .set_raw("time_limit_ms", "3000")
      .set_raw("mem_limit_kb", "131072")
      .set_text("harness_prelude", "")
      .set_raw("is_featured", "true")
      .set_raw("sort_no", "10")
      .set_raw("tags", R"({"algorithm":["数组","哈希表"],"source":["高频面试题"]})")
      .set_raw("cases",
               R"([{"args":[[2,7,11,15],9],"expected":[0,1],"brief":"基础用例"},)"
               R"({"args":[[3,2,4],6],"expected":[1,2]}])");
  return doc;
}

// 大多数用例的 slug 是 two-sum，文件名比对才有意义；个别题（如树题）用别的 slug，
// 于是把"文件名"也做成参数，而不是让那条用例自己去绕过校验。
ParseResult parse_with_stem(const std::string& text, const std::string& expect_slug) {
  ParseOptions options;
  options.source = expect_slug + ".json";
  options.expect_slug = expect_slug;
  return parse_problem_json(text, options);
}

ParseResult parse(const std::string& text) { return parse_with_stem(text, "two-sum"); }

bool has_issue(const ParseResult& result, const std::string& field, IssueKind kind) {
  for (const ValidationIssue& issue : result.issues) {
    if (issue.field == field && issue.kind == kind) return true;
  }
  return false;
}

bool has_field(const ParseResult& result, const std::string& field) {
  for (const ValidationIssue& issue : result.issues) {
    if (issue.field == field) return true;
  }
  return false;
}

// 拼接所有 field 为 field 的问题文本，便于断言"说了原因"而不只是"报了错"
std::string issues_at(const ParseResult& result, const std::string& field) {
  std::string text;
  for (const ValidationIssue& issue : result.issues) {
    if (issue.field == field) text += render_issue(issue) + "\n";
  }
  return text;
}

std::string issues_text(const ParseResult& result) {
  return oj::problem::format_issues(result.issues, "two-sum.json");
}

// ---- 用例 / 标签片段的构造 --------------------------------------------------
// 【为什么不直接写 R"([{"args":…,"expected":…}])"】
//   裸 raw string 以 )" 收尾，而 JSON 片段里成对出现的 `:"` 会让"内容"多吞一个右括号
//   之前的那一段（`brief":` 后面紧跟 `)"`，本该是收尾符，被当成了内容的一部分）——
//   于是拼出来的 JSON 变成 `"brief":""xxx""`，多两个引号，解析直接失败，而报错信息
//   指向"JSON 语法错误"，与真正的原因（我写错了字面量）隔着两层。
//   所以这里全部改用 std::string 拼接：拼错了一眼能看见，且不必记 raw string 的收尾规则。
std::string one_case(const std::string& args, const std::string& expected,
                     const std::string& brief = std::string()) {
  std::string text = "{\"args\":" + args + ",\"expected\":" + expected;
  if (!brief.empty()) text += ",\"brief\":" + quoted(brief);
  return text + "}";
}

std::string cases_json(const std::vector<std::string>& items) {
  std::string text = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) text += ",";
    text += items[i];
  }
  return text + "]";
}

std::string tags_json(const std::vector<std::string>& algorithm,
                      const std::vector<std::string>& source) {
  const auto quoted_list = [](const std::vector<std::string>& names) {
    std::string text = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i != 0) text += ",";
      text += quoted(names[i]);
    }
    return text + "]";
  };
  return "{\"algorithm\":" + quoted_list(algorithm) + ",\"source\":" + quoted_list(source) + "}";
}

// ============================================================================
// 形状推导原语（T1.4）
// ============================================================================

TEST(ProblemShape, ParsesVectorDepthAndBaseType) {
  const CppTypeShape flat = oj::model::analyze_cpp_type("std::vector<int>&");
  EXPECT_TRUE(flat.well_formed);
  EXPECT_EQ(flat.vector_depth, 1);
  EXPECT_EQ(flat.base_type, "int");
  EXPECT_FALSE(flat.is_pointer);

  // 二维：内层的 '>' 不能当成外层的结束符 —— 这是本函数唯一的难点
  const CppTypeShape nested = oj::model::analyze_cpp_type("std::vector<std::vector<int>>");
  EXPECT_TRUE(nested.well_formed);
  EXPECT_EQ(nested.vector_depth, 2);
  EXPECT_EQ(nested.base_type, "int");

  const CppTypeShape scalar = oj::model::analyze_cpp_type("int");
  EXPECT_TRUE(scalar.well_formed);
  EXPECT_EQ(scalar.vector_depth, 0);
  EXPECT_TRUE(scalar.scalar_known);

  // code_template 里普遍 using namespace std;，因此 vector<int> 与 std::vector<int> 等价
  EXPECT_EQ(oj::model::analyze_cpp_type("vector<int>").vector_depth, 1);
  EXPECT_EQ(oj::model::analyze_cpp_type("const std::string &").base_type, "std::string");
}

TEST(ProblemShape, RecognizesPointerTypesAndUnknowns) {
  const CppTypeShape pointer = oj::model::analyze_cpp_type("TreeNode*");
  EXPECT_TRUE(pointer.well_formed);
  EXPECT_TRUE(pointer.is_pointer);
  EXPECT_EQ(pointer.vector_depth, 0);
  // 自定义类型不在可静态判定的表内 → 放行（§5.6.1 的"查不出就不猜"）
  EXPECT_FALSE(pointer.scalar_known);

  EXPECT_TRUE(oj::model::analyze_cpp_type("std::string").scalar_known);
  EXPECT_FALSE(oj::model::analyze_cpp_type("std::map<int, int>").well_formed);
  EXPECT_FALSE(oj::model::analyze_cpp_type("std::vector<int").well_formed);
  EXPECT_FALSE(oj::model::analyze_cpp_type("").well_formed);
}

TEST(ProblemShape, DescribesShapeBackToCppText) {
  EXPECT_EQ(oj::model::describe_cpp_shape(oj::model::analyze_cpp_type("std::vector<std::vector<int>>")),
            "std::vector<std::vector<int>>");
  EXPECT_EQ(oj::model::describe_cpp_shape(oj::model::analyze_cpp_type("TreeNode*")), "TreeNode*");
}

TEST(ProblemShape, MeasuresJsonDepthAndExpr) {
  EXPECT_EQ(oj::model::analyze_json(nlohmann::json::array({1, 2})).array_depth, 1);
  // 取最深处：混合深度不是"第一个元素"的深度
  EXPECT_EQ(oj::model::analyze_json(nlohmann::json::array({nlohmann::json::array({1, 2}), 3}))
                .array_depth,
            2);
  EXPECT_EQ(oj::model::analyze_json(nlohmann::json::array()).array_depth, 1);
  EXPECT_EQ(oj::model::analyze_json(9).array_depth, 0);
  EXPECT_TRUE(oj::model::analyze_json(9).is_scalar);
}

TEST(ProblemShape, ExprObjectMustHaveExactlyOneKey) {
  EXPECT_TRUE(oj::model::is_expr_value(nlohmann::json::parse(R"json({"__expr":"buildTree(1)"})json")));
  // 多一个键就判 false：那多半是录题时的手误，而生成器拿到的仍是原样文本
  EXPECT_FALSE(oj::model::is_expr_value(nlohmann::json::parse(R"({"__expr":"x","note":"y"})")));
  EXPECT_FALSE(oj::model::is_expr_value(nlohmann::json::parse(R"({"expr":"x"})")));
  EXPECT_FALSE(oj::model::is_expr_value(nlohmann::json::parse(R"(["__expr"])")));
}

TEST(ProblemShape, MatchesScalarAndContainerValues) {
  std::string reason;
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse("[1,2]"),
                                                "std::vector<int>", "x", &reason));
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse("[[1],[2,3]]"),
                                                "std::vector<std::vector<int>>", "x", &reason));
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse("3.5"), "double", "x",
                                                &reason));
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse("true"), "bool", "x",
                                                &reason));
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse(R"("ab")"),
                                                "std::string", "x", &reason));
  // 整数类型接受整数，不接受浮点（2.0 写成 2 才是题库该有的样子）
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse("2"), "int", "x", &reason));
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse("2.0"), "int", "x",
                                                  &reason));

  // 深度不符：容器类型给了标量
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse("9"),
                                                 "std::vector<int>", "x", &reason));
  EXPECT_NE(reason.find("vector"), std::string::npos) << reason;

  // 元素类型不符：容器层数对得上，元素不是整数
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse(R"(["a","b"])"),
                                                 "std::vector<int>", "x", &reason));
  EXPECT_NE(reason.find("整数"), std::string::npos) << reason;

  // 消息里要能看出是哪个下标
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse("[[1],[2,\"x\"]]"),
                                                 "std::vector<std::vector<int>>", "cases[0].args[0]",
                                                 &reason));
  EXPECT_NE(reason.find("cases[0].args[0][1][1]"), std::string::npos) << reason;
}

TEST(ProblemShape, UnknownTypesAreLetThroughExceptArraysAndObjects) {
  std::string reason;
  const nlohmann::json expr = nlohmann::json::parse(R"json({"__expr":"buildTree({1,null,2})"})json");
  // 逃生舱恒放行（§4.2）
  EXPECT_TRUE(oj::model::value_matches_cpp_type(expr, "TreeNode*", "x", &reason));
  // 自定义类型 + JSON null → nullptr（§4.2 映射表）
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json(), "TreeNode*", "x", &reason));
  // 自定义类型 + 数组：一定是错的（没有"用数组构造 TreeNode*"这种写法）
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse("[1,2]"), "TreeNode*", "x",
                                                 &reason));
}

TEST(ProblemShape, CaseTextRoundTripsThroughDump) {
  const nlohmann::json args = nlohmann::json::parse("[[2,7,11,15],9]");
  std::string reason;
  const nlohmann::json back = oj::model::parse_case_text(args.dump(), &reason);
  ASSERT_FALSE(back.is_null()) << reason;
  EXPECT_EQ(back, args);
  // dump 必须稳定：同一份值反复序列化得到逐字节相同的文本（幂等同步的前提）
  EXPECT_EQ(back.dump(), args.dump());

  EXPECT_TRUE(oj::model::parse_case_text("{不是 json", &reason).is_null());
  EXPECT_FALSE(reason.empty());
}

// ============================================================================
// 合法题库
// ============================================================================

TEST(ProblemSchema, AcceptsAValidProblem) {
  const ParseResult result = parse(valid_doc().text());
  ASSERT_TRUE(result.ok) << issues_text(result);

  const oj::model::ProblemBundle& problem = result.problem;
  EXPECT_EQ(problem.slug, "two-sum");
  EXPECT_EQ(problem.title, "两数之和");
  EXPECT_EQ(problem.difficulty, oj::model::Difficulty::kMedium);
  EXPECT_EQ(problem.method_name, "twoSum");
  EXPECT_EQ(problem.return_type, "std::vector<int>");
  EXPECT_EQ(problem.time_limit_ms, 3000u);
  EXPECT_EQ(problem.mem_limit_kb, 131072u);
  EXPECT_TRUE(problem.is_featured);
  EXPECT_EQ(problem.sort_no, 10);
  EXPECT_EQ(problem.harness_prelude, "");           // 空串 = 落库 NULL（§4.1）
  EXPECT_FALSE(problem.uses_expr());
  EXPECT_EQ(result.slug, "two-sum");

  ASSERT_EQ(problem.params.size(), 2u);
  EXPECT_EQ(problem.params[0].name, "nums");
  EXPECT_EQ(problem.params[0].cpp_type, "std::vector<int>&");
  EXPECT_EQ(problem.params[0].param_idx, 0u);
  EXPECT_EQ(problem.params[0].vector_depth(), 1);
  EXPECT_EQ(problem.params[1].param_idx, 1u);

  ASSERT_EQ(problem.cases.size(), 2u);
  EXPECT_EQ(problem.case_count(), 2u);              // 受控反范式列的内存口径
  EXPECT_EQ(problem.cases[1].case_idx, 1u);
  EXPECT_EQ(problem.cases[1].brief, "");            // brief 省略 → 空串 → 落库 NULL
  EXPECT_EQ(problem.cases[0].args_json(), "[[2,7,11,15],9]");
  EXPECT_EQ(problem.cases[0].expected_json(), "[0,1]");

  EXPECT_EQ(problem.algorithm_tags, (std::vector<std::string>{"数组", "哈希表"}));
  EXPECT_EQ(problem.source_tags, (std::vector<std::string>{"高频面试题"}));
  EXPECT_EQ(problem.tags_of(oj::model::TagCategory::kAlgorithm).size(), 2u);
  ASSERT_NE(problem.test_case(1), nullptr);
  EXPECT_EQ(problem.test_case(1)->case_idx, 1u);
  EXPECT_EQ(problem.test_case(2), nullptr);
}

TEST(ProblemSchema, OptionalFieldsFallBackToDdlDefaults) {
  // 最小题库：§4.1 里所有可省字段都不给，期望取 §3.2 的 DDL 默认值而不是 0
  const std::string minimal =
      R"({"slug":"two-sum","title":"两数之和","difficulty":"easy","statement_md":"题面",)"
      R"("signature":"class Solution { public: int f(); };","method_name":"f","return_type":"int",)"
      R"("code_template":"class Solution { public: int f() { return 0; } };",)"
      R"("params":[{"name":"x","cpp_type":"int"}],"cases":[{"args":[1],"expected":2}]})";
  const ParseResult result = parse(minimal);
  ASSERT_TRUE(result.ok) << issues_text(result);
  EXPECT_EQ(result.problem.time_limit_ms, 3000u);
  EXPECT_EQ(result.problem.mem_limit_kb, 131072u);
  EXPECT_FALSE(result.problem.is_featured);
  EXPECT_EQ(result.problem.sort_no, 0);
  EXPECT_TRUE(result.problem.hint_md.empty());
  EXPECT_TRUE(result.problem.algorithm_tags.empty());
}

TEST(ProblemSchema, DifficultyIsCaseInsensitiveButMustBeOneOfThree) {
  EXPECT_TRUE(parse(valid_doc().set_text("difficulty", "Easy").text()).ok);
  EXPECT_TRUE(parse(valid_doc().set_text("difficulty", "HARD").text()).ok);

  const ParseResult bad = parse(valid_doc().set_text("difficulty", "简单").text());
  EXPECT_FALSE(bad.ok);
  EXPECT_TRUE(has_issue(bad, "difficulty", IssueKind::kValue));
}

// ============================================================================
// 层 ①：必填字段 / 类型 / 值域
// ============================================================================

TEST(ProblemSchema, ReportsEveryMissingRequiredField) {
  for (const char* key : {"slug", "title", "difficulty", "statement_md", "signature",
                          "method_name", "return_type", "code_template", "params", "cases"}) {
    const ParseResult result = parse(valid_doc().drop(key).text());
    EXPECT_FALSE(result.ok) << key;
    EXPECT_TRUE(has_field(result, key)) << key << " → " << issues_text(result);
  }
}

TEST(ProblemSchema, EmptyStringCountsAsMissing) {
  const ParseResult result = parse(valid_doc().set_text("title", "   ").text());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(has_issue(result, "title", IssueKind::kMissing));
}

TEST(ProblemSchema, WrongJsonTypeIsReported) {
  const ParseResult title = parse(valid_doc().set_raw("title", "42").text());
  EXPECT_FALSE(title.ok);
  EXPECT_TRUE(has_issue(title, "title", IssueKind::kType));

  const ParseResult cases_not_array = parse(valid_doc().set_raw("cases", "{}").text());
  EXPECT_FALSE(cases_not_array.ok);
  EXPECT_TRUE(has_issue(cases_not_array, "cases", IssueKind::kType));
}

TEST(ProblemSchema, ValuesLongerThanColumnsAreRejected) {
  // slug 65 字节 > VARCHAR(64)：宁可在录题时报错，也不要等 MySQL 报 Data too long
  const ParseResult slug = parse(valid_doc().set_text("slug", std::string(65, 'a')).text());
  EXPECT_FALSE(slug.ok);
  EXPECT_TRUE(has_issue(slug, "slug", IssueKind::kValue));
  EXPECT_NE(issues_at(slug, "slug").find("64"), std::string::npos);

  // TEXT 列（signature / harness_prelude）的上限是 65535 字节，不是 MEDIUMTEXT 的 16MB。
  // 这条用例守着一条明确的教训：曾经把 signature 当成"不限长"放行，结果超长的题在
  // INSERT 那一刻才被 MySQL 拒 —— 报错现场从录题现场搬到了 DB 现场。
  const std::string long_signature = std::string(70000, 'x');
  const ParseResult signature =
      parse(valid_doc().set_text("signature", long_signature).text());
  EXPECT_FALSE(signature.ok);
  EXPECT_TRUE(has_issue(signature, "signature", IssueKind::kValue)) << issues_text(signature);
  EXPECT_NE(issues_at(signature, "signature").find("65535"), std::string::npos);

  const ParseResult prelude =
      parse(valid_doc().set_text("harness_prelude", long_signature).text());
  EXPECT_FALSE(prelude.ok);
  EXPECT_TRUE(has_issue(prelude, "harness_prelude", IssueKind::kValue)) << issues_text(prelude);

  // MEDIUMTEXT 列（statement_md / code_template / hint_md）仍不限长：70000 字节的题面
  // 是合法的，不该被拦
  EXPECT_TRUE(parse(valid_doc().set_text("statement_md", long_signature).text()).ok);

  const std::string long_brief = std::string(300, 'x');
  const ParseResult brief = parse(valid_doc()
                                      .set_raw("cases", cases_json({one_case(
                                          "[[2,7,11,15],9]", "[0,1]", long_brief)}))
                                      .text());
  EXPECT_FALSE(brief.ok) << issues_text(brief);
  EXPECT_TRUE(has_field(brief, "cases[0].brief")) << issues_text(brief);
}

TEST(ProblemSchema, NumericFieldsAreRangeChecked) {
  EXPECT_FALSE(parse(valid_doc().set_raw("time_limit_ms", "0").text()).ok);
  EXPECT_TRUE(parse(valid_doc().set_raw("time_limit_ms", "600000").text()).ok);
  EXPECT_FALSE(parse(valid_doc().set_raw("time_limit_ms", "600001").text()).ok);
  EXPECT_FALSE(parse(valid_doc().set_raw("time_limit_ms", "-1").text()).ok);
  EXPECT_FALSE(parse(valid_doc().set_raw("mem_limit_kb", "0").text()).ok);
  EXPECT_FALSE(parse(valid_doc().set_raw("mem_limit_kb", quoted("131072")).text()).ok);
  // JSON 里写成 3000.0（值恰为整数）应当被接受 —— 常见于从表格里复制出来的数值
  EXPECT_TRUE(parse(valid_doc().set_raw("time_limit_ms", "3000.0").text()).ok);
}

TEST(ProblemSchema, SlugMustBeUrlFriendlyAndMatchFileName) {
  EXPECT_FALSE(parse(valid_doc().set_text("slug", "Two-Sum").text()).ok);
  EXPECT_FALSE(parse(valid_doc().set_text("slug", "two_sum").text()).ok);
  EXPECT_FALSE(parse(valid_doc().set_text("slug", "-two-sum").text()).ok);
  ParseOptions no_stem_check;   // 不比对文件名时，格式合法的 slug 都接受
  EXPECT_TRUE(
      parse_problem_json(valid_doc().set_text("slug", "two-sum-2").text(), no_stem_check).ok);

  const ParseResult mismatched = parse(valid_doc().set_text("slug", "other-sum").text());
  EXPECT_FALSE(mismatched.ok);
  EXPECT_TRUE(has_issue(mismatched, "slug", IssueKind::kValue));
  EXPECT_NE(issues_at(mismatched, "slug").find("文件名"), std::string::npos);

  // 不校验文件名时（单测直接喂内存文本），只查格式
  ParseOptions options;
  EXPECT_TRUE(parse_problem_json(valid_doc().set_text("slug", "other-sum").text(), options).ok);
}

TEST(ProblemSchema, UnknownTopLevelFieldIsReported) {
  // 拼错的 statement_md：若只查必填，报错会指向 statement_md 缺失，
  // 而真正的原因（多了一个 statements_md）没人看得见
  const ParseResult result = parse(valid_doc().set_text("statements_md", "题面").text());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(has_issue(result, "statements_md", IssueKind::kUnknownField));
}

TEST(ProblemSchema, MethodNameMustBeIdentifierAndAppearInSignature) {
  EXPECT_FALSE(parse(valid_doc().set_text("method_name", "two sum").text()).ok);

  const ParseResult absent =
      parse(valid_doc()
                .set_text("signature", "class Solution { public: int other(); };")
                .text());
  EXPECT_FALSE(absent.ok);
  EXPECT_TRUE(has_issue(absent, "method_name", IssueKind::kConsistency));

  // 子串不算出现过：签名里只有 twoSumPair 时，twoSum 仍然是【没被声明】
  const ParseResult prefix =
      parse(valid_doc()
                .set_text("signature", "class Solution { public: int twoSumPair(); };")
                .text());
  EXPECT_FALSE(prefix.ok);
  EXPECT_TRUE(has_issue(prefix, "method_name", IssueKind::kConsistency));
}

// ============================================================================
// 层 ②：类型一致性静态校验（§5.6.1 的 7 条，D-64）
// ============================================================================

TEST(ProblemSchema, ArgsCountMustMatchParamsCount) {
  const ParseResult result = parse(valid_doc()
                                       .set_raw("cases", cases_json({one_case("[9]", "[0,1]")}))
                                       .text());
  EXPECT_FALSE(result.ok) << issues_text(result);
  EXPECT_TRUE(has_issue(result, "cases[0].args", IssueKind::kConsistency));
}

TEST(ProblemSchema, ArgsContainerDepthMustMatchCppType) {
  // 第二个用例把 nums 写成标量 1：容器类型收到非数组，深度对不上（层 ② 第 4 条）
  const ParseResult result = parse(
      valid_doc()
          .set_raw("cases", cases_json({one_case("[[2,7,11,15],9]", "[0,1]", "基础用例"),
                                        one_case("[1,9]", "[0,1]", "深度少一层")}))
          .text());
  EXPECT_FALSE(result.ok) << issues_text(result);
  EXPECT_TRUE(has_issue(result, "cases[1].args[0]", IssueKind::kConsistency))
      << issues_text(result);
  EXPECT_NE(issues_at(result, "cases[1].args[0]").find("vector"), std::string::npos);
}

TEST(ProblemSchema, ArgsElementTypeMustMatchCppType) {
  const ParseResult result = parse(valid_doc()
                                       .set_raw("cases", cases_json(
                                                            {one_case(R"([["a","b"],9])", "[0,1]")}))
                                       .text());
  EXPECT_FALSE(result.ok) << issues_text(result);
  // issue 的 field 是【出错的形参】（一个形参一条问题），更细的路径在 message 里
  EXPECT_TRUE(has_issue(result, "cases[0].args[0]", IssueKind::kConsistency));
  const std::string text = issues_at(result, "cases[0].args[0]");
  EXPECT_NE(text.find("cases[0].args[0][0]"), std::string::npos) << text;
  EXPECT_NE(text.find("整数"), std::string::npos) << text;
}

TEST(ProblemSchema, ExpectedDepthMustMatchReturnType) {
  // return_type 是 int，expected 却是数组：这是 harness 编译错误的最大来源（§4.2.1）
  const ParseResult result = parse(valid_doc().set_text("return_type", "int").text());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(has_issue(result, "cases[0].expected", IssueKind::kConsistency));
  EXPECT_TRUE(has_issue(result, "cases[1].expected", IssueKind::kConsistency));
}

TEST(ProblemSchema, ExpectedElementTypeMustMatchReturnType) {
  const ParseResult result = parse(valid_doc()
                                       .set_raw("cases", cases_json(
                                                            {one_case("[[2,7],9]", R"(["0","1"])")}))
                                       .text());
  EXPECT_FALSE(result.ok) << issues_text(result);
  EXPECT_TRUE(has_issue(result, "cases[0].expected", IssueKind::kConsistency));
  EXPECT_NE(issues_at(result, "cases[0].expected").find("cases[0].expected[0]"), std::string::npos);
}

TEST(ProblemSchema, EmptyParamsAndCasesAreRejected) {
  EXPECT_TRUE(has_issue(parse(valid_doc().set_raw("params", "[]").text()), "params",
                        IssueKind::kMissing));
  EXPECT_TRUE(has_issue(parse(valid_doc().set_raw("cases", "[]").text()), "cases",
                        IssueKind::kMissing));
}

TEST(ProblemSchema, DuplicateOrMalformedParamNamesAreRejected) {
  const ParseResult duplicate = parse(
      valid_doc()
          .set_raw("params",
                   R"([{"name":"nums","cpp_type":"std::vector<int>&"},)"
                   R"({"name":"nums","cpp_type":"int"}])")
          .text());
  EXPECT_FALSE(duplicate.ok);
  EXPECT_TRUE(has_issue(duplicate, "params[1].name", IssueKind::kConsistency));

  const ParseResult malformed = parse(
      valid_doc()
          .set_raw("params",
                   R"([{"name":"2nums","cpp_type":"std::vector<int>&"},)"
                   R"({"name":"target","cpp_type":"int"}])")
          .text());
  EXPECT_FALSE(malformed.ok);
  EXPECT_TRUE(has_issue(malformed, "params[0].name", IssueKind::kConsistency));

  // cpp_type 解析不出元素类型（不认识的模板）
  const ParseResult bad_type = parse(
      valid_doc()
          .set_raw("params",
                   R"([{"name":"nums","cpp_type":"std::map<int,int>"},)"
                   R"({"name":"target","cpp_type":"int"}])")
          .text());
  EXPECT_FALSE(bad_type.ok);
  EXPECT_TRUE(has_issue(bad_type, "params[0].cpp_type", IssueKind::kConsistency));
}

TEST(ProblemSchema, ExprRequiresHarnessPrelude) {
  // 树题：__expr 构造入参，prelude 提供构造函数声明（§4.2 的约定）
  ProblemDoc tree;
  tree.set_text("slug", "max-depth")
      .set_text("title", "二叉树的最大深度")
      .set_text("difficulty", "easy")
      .set_text("statement_md", "题面")
      .set_text("method_name", "maxDepth")
      .set_text("return_type", "int")
      .set_text("signature", "class Solution { public: int maxDepth(TreeNode* root); };")
      .set_text("code_template", "class Solution {};")
      .set_raw("params", R"([{"name":"root","cpp_type":"TreeNode*"}])")
      .set_raw("cases",
               cases_json({one_case(R"json([{"__expr":"buildTree({1,null,2})"}])json", "2")}));

  const ParseResult missing_prelude = parse_with_stem(tree.text(), "max-depth");
  EXPECT_FALSE(missing_prelude.ok);
  EXPECT_TRUE(has_issue(missing_prelude, "harness_prelude", IssueKind::kConsistency))
      << issues_text(missing_prelude);

  tree.set_text("harness_prelude", "TreeNode* buildTree(std::vector<int>);");
  const ParseResult with_prelude = parse_with_stem(tree.text(), "max-depth");
  EXPECT_TRUE(with_prelude.ok) << issues_text(with_prelude);
  EXPECT_TRUE(with_prelude.problem.uses_expr());
}

TEST(ProblemSchema, TagsAreValidatedAndDeduplicated) {
  const ParseResult bad_category = parse(
      valid_doc().set_raw("tags", R"({"algorithm":["数组"],"topics":["链表"]})").text());
  EXPECT_FALSE(bad_category.ok);
  EXPECT_TRUE(has_issue(bad_category, "tags.topics", IssueKind::kValue))
      << issues_text(bad_category);

  const ParseResult dedup =
      parse(valid_doc().set_raw("tags", tags_json({"数组", "数组"}, {})).text());
  ASSERT_TRUE(dedup.ok) << issues_text(dedup);
  EXPECT_EQ(dedup.problem.algorithm_tags, (std::vector<std::string>{"数组"}));
  EXPECT_TRUE(dedup.problem.source_tags.empty());

  const std::string long_tag = std::string(40, 'x');
  const ParseResult too_long =
      parse(valid_doc().set_raw("tags", tags_json({long_tag}, {})).text());
  EXPECT_FALSE(too_long.ok);
  EXPECT_TRUE(has_field(too_long, "tags.algorithm[0]")) << issues_text(too_long);
}

// ============================================================================
// [D-48] 题面禁图片
// ============================================================================

TEST(ProblemSchema, StatementAndHintMustNotContainImages) {
  EXPECT_TRUE(has_issue(
      parse(valid_doc().set_text("statement_md", "看图 ![](../img/tree.png)").text()),
      "statement_md", IssueKind::kImage));
  EXPECT_TRUE(has_issue(parse(valid_doc().set_text("hint_md", "![](./a.png)").text()), "hint_md",
                        IssueKind::kImage));
  // 字符画是允许的（[D-48] 的正面要求）
  EXPECT_TRUE(parse(valid_doc()
                        .set_text("statement_md", "```\n   1\n  / \\\n 2   3\n```")
                        .text())
                  .ok);
  // 关掉这个开关后放行（默认必须是开的）
  ParseOptions lenient;
  lenient.expect_slug = "two-sum";
  lenient.forbid_images = false;
  EXPECT_TRUE(
      parse_problem_json(valid_doc().set_text("statement_md", "![](./a.png)").text(), lenient).ok);
}

// ============================================================================
// 坏输入与报告形态
// ============================================================================

TEST(ProblemSchema, BrokenJsonIsReportedNotThrown) {
  for (const char* text : {"", "   ", "{", "[]", "\"字符串\"", "{}{}", "{\"slug\":}"}) {
    const ParseResult result = parse(text);
    EXPECT_FALSE(result.ok) << text;
    EXPECT_FALSE(result.issues.empty()) << text;
  }
}

TEST(ProblemSchema, CollectsAllIssuesInsteadOfFailingFast) {
  // 同时破坏三处：加载期必须一次报全，否则录题的人要反复重启才能改完一道题
  const ParseResult result = parse(valid_doc()
                                       .set_text("slug", "Bad_Slug")
                                       .set_text("method_name", "absent")
                                       .set_raw("cases", cases_json({one_case("[9]", "[0,1]")}))
                                       .text());
  EXPECT_FALSE(result.ok);
  EXPECT_GE(result.issues.size(), 3u) << issues_text(result);
  EXPECT_TRUE(has_field(result, "slug"));
  EXPECT_TRUE(has_field(result, "method_name"));
  EXPECT_TRUE(has_field(result, "cases[0].args"));
}

TEST(ProblemSchema, FailedParseYieldsNoHalfFilledProblemButKeepsSlug) {
  const ParseResult result = parse(valid_doc().set_text("title", "").text());
  EXPECT_FALSE(result.ok);
  // 半份数据比空数据更危险：调用点一律看 ok，但也不该从 problem 里读到别的字段
  EXPECT_TRUE(result.problem.params.empty());
  EXPECT_TRUE(result.problem.cases.empty());
  EXPECT_TRUE(result.problem.slug.empty());
  // 但 slug 要留着 —— 加载器靠它把库里那条旧行标记为失效
  EXPECT_EQ(result.slug, "two-sum");
}

TEST(ProblemSchema, FormatIssuesPrefixedWithSource) {
  const ParseResult result = parse(valid_doc().set_text("title", "").text());
  const std::string text = oj::problem::format_issues(result.issues, "two-sum.json");
  EXPECT_NE(text.find("two-sum.json: title:"), std::string::npos) << text;
  EXPECT_NE(text.find("必填缺失"), std::string::npos) << text;
}

TEST(ProblemShape, DeepNestingAndScalarShortcuts) {
  // 三层嵌套：递归不止两层，报错路径也要能递归下去
  const CppTypeShape triple =
      oj::model::analyze_cpp_type("std::vector<std::vector<std::vector<int>>>");
  ASSERT_TRUE(triple.well_formed) << "括号不配对的类型必须判为不合形";
  EXPECT_EQ(triple.vector_depth, 3);
  EXPECT_EQ(triple.base_type, "int");
  // 少写一个 '>' 的写法：必须判不合形，而不是"当成一种类型放行"
  EXPECT_FALSE(oj::model::analyze_cpp_type("std::vector<std::vector<std::vector<int>>").well_formed);
  EXPECT_TRUE(oj::model::value_matches_cpp_type(
      nlohmann::json::parse("[[[1]],[[2,3]]]"), "std::vector<std::vector<std::vector<int>>>", "x",
      nullptr));
  EXPECT_FALSE(oj::model::value_matches_cpp_type(
      nlohmann::json::parse("[[1,2],[3]]"), "std::vector<std::vector<std::vector<int>>>", "x",
      nullptr));

  // 常用标量的快捷判定：unsigned 收整数、bool 只收布尔、vector<string> 收字符串数组
  std::string reason;
  EXPECT_TRUE(oj::model::value_matches_cpp_type(nlohmann::json::parse("7"), "unsigned", "x", &reason));
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse("-1"), "unsigned", "x", &reason));
  EXPECT_TRUE(oj::model::value_matches_cpp_type(
      nlohmann::json::parse(R"(["a","b"])"), "std::vector<std::string>", "x", &reason));
  EXPECT_FALSE(oj::model::value_matches_cpp_type(
      nlohmann::json::parse("[1]"), "std::vector<std::string>", "x", &reason));
  // bool 不接受整数：JSON 里 1/0 不是 true/false，写成 1 会在生成后判 WA，
  // 而那正是"题面示例看着对、用例却永远错"的那类难查问题
  EXPECT_FALSE(oj::model::value_matches_cpp_type(nlohmann::json::parse("1"), "bool", "x", &reason));
}

TEST(ProblemSchema, ParseProblemFileReportsUnreadablePath) {
  const ParseResult result =
      oj::problem::parse_problem_file("/tmp/oj-不存在的题库文件-42.json", "whatever");
  EXPECT_FALSE(result.ok);
  ASSERT_EQ(result.issues.size(), 1u);
  EXPECT_EQ(result.issues[0].kind, IssueKind::kJson);
  EXPECT_NE(result.issues[0].message.find("读不到文件"), std::string::npos);
}

TEST(ProblemSchema, CasesAreNotCheckedWhenReturnTypeIsUnreadable) {
  // return_type 缺失时，cases 里的一切推断都无从谈起（推导的基准没了）。
  // 此时只报 return_type 一条：把 cases 也逐条报一遍，会让真正的原因淹没在几十条派生错误里。
  const ParseResult result = parse(valid_doc().drop("return_type").text());
  ASSERT_FALSE(result.ok);
  EXPECT_TRUE(has_field(result, "return_type"));
  EXPECT_FALSE(has_field(result, "cases"));
  EXPECT_FALSE(has_field(result, "cases[0].expected"));
}

TEST(ProblemSchema, IssueKindsHaveReadableNames) {
  EXPECT_STREQ(oj::problem::to_string(IssueKind::kMissing), "必填缺失");
  EXPECT_STREQ(oj::problem::to_string(IssueKind::kConsistency), "类型不一致");
  EXPECT_STREQ(oj::problem::to_string(IssueKind::kImage), "禁图片");
}

}  // namespace
