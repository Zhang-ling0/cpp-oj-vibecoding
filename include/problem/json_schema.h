// =============================================================================
// CppOJ 题库 JSON 的解析与校验 —— 把 problems/<slug>.json 变成 model::ProblemBundle
// 对应任务：SPEC.md §9 M1 T1.2 第一句「校验必填字段」+ T2.1.1「题库加载期静态校验」
// 规格来源：SPEC.md §4.1（JSON Schema / 必填与可选字段 / 禁图片 D-48）
//           / §5.6.1（类型一致性静态校验 7 条，D-64）
//           / §4.2（字面量映射、__expr 逃生舱）/ §4.2.1（生成期校验的分工说明）
//
// 【两层校验的分工与为什么一层不够】T1.2 的原文是「校验分两层 D-64」
//   ① 必填字段：slug / signature / method_name / params / cases / return_type 非空，
//      外加 §4.1 的 title / difficulty / statement_md / code_template，以及一组
//      【落库前必须确定】的值域（列宽、枚举取值、标识符合法性）。
//      缺这一层的后果很具体：slug 为空 → problems.slug NOT NULL 直接抛 DB 错误，
//      而那条错误在启动日志里长这样「Column 'slug' cannot be null」，与"哪个文件错了"
//      之间隔着一次人工关联。
//   ② 类型一致性（§5.6.1 的 7 条）：容器深度 / 元素类型 / 参数与形参一一对应 /
//      method_name 在 signature 中出现 / 非空 / __expr 配对。
//      缺这一层的后果是 §5.6 全篇在讲的那件事：错题进了库，用户第一次提交它就撞上
//      harness 编译错误，而 UI 会说那是"编译错误" —— 用户去查自己的代码，冤。
//   两层都在本模块，且【在写库之前】跑完：不通过就整题跳过（§5.6.1「跳过该题而不阻塞
//   其他题」），而不是写进去再报错。
//
// 【为什么不 fail-fast，而是收集全部问题】
//   录题的人一次只想改一处错误。若只报第一个，他会"改→重启→看到下一个错"，
//   50 道题里的一道可能反复重启十几次（SPEC §4.3 的工作流就是重启驱动的）。
//   代价是本文件不能"报错就返回"，每个检查点都要能在已有错误的前提下继续跑；
//   这也要求下面的取值辅助在【解析成功后】才做，避免对 nullptr 调 value()。
//
// 【不做什么】
//   · 不碰 MySQL：加载器（problem/loader.h）负责写库，本模块只保证"能不能写"的前置判断
//   · 不校验 __expr 表达式本身（§5.6.1 明说：任意 C++ 表达式无法静态校验）
//   · 不校验题面内容质量（hint_md 是不是剧透、题面有没有歧义）—— 那是人的事
// =============================================================================
#pragma once

#include <string>
#include <vector>

#include "model/problem.h"

namespace oj {
namespace problem {

// 问题类别。分类不是为了报表，而是为了让"【值域超长】"与"【类型不一致】"在日志里
// 一眼可辨：前者是抄写超长，后者是题意图错了（§5.6.1 的语境）。
enum class IssueKind {
  kJson,           // 整个文件不是合法 JSON / 顶层不是对象
  kMissing,        // 必填字段缺失或为空（层 ①）
  kType,           // 字段类型不对（string 写成对象、数组写成字符串……）
  kValue,          // 值域不合法（列宽、枚举取值、标识符）
  kUnknownField,   // 顶层出现了 §4.1 之外的字段 —— 抄写/拼错的唯一可观测信号
  kConsistency,    // 层 ② 的类型一致性 7 条
  kImage,          // 题面出现 ![...](...)  [D-48]
};
const char* to_string(IssueKind kind);

struct ValidationIssue {
  IssueKind kind = IssueKind::kMissing;
  std::string field;     // JSON 路径（cases[1].args[0]）或列名；无路径时为空
  std::string message;   // 人可读：期望什么、实际什么
};

// "cases[1].args: 该位置要求数组，实际是对象（类型不一致）"；无 field 时省去前缀。
std::string render_issue(const ValidationIssue& issue);

struct ParseOptions {
  std::string source = "<memory>";  // 文件路径，仅用于报错定位
  // 非空时要求 JSON 里的 slug 与之相等。加载器传文件主名 —— 文件名与 slug 不一致
  // 几乎总是"复制改名"留下的，而它会让 URL（/problem/<slug>）与题库文件对不上。
  std::string expect_slug;
  // [D-48] 题面禁图片。默认开；关掉它需要显式写 false，避免"顺手加个开关"绕过决策。
  bool forbid_images = true;
};

struct ParseResult {
  bool ok = false;                        // 无任何问题
  model::ProblemBundle problem;           // ok 为真时才有意义（否则为空对象）
  // slug 的"尽力而为"取值：即使校验不通过，只要 slug 本身读到了就填在这里。
  // 加载器要用它把【库里已有的旧行】标记为失效（清空用例 + case_count=0）——
  // 题目配置后来被改错了，不该让它继续留在列表页，但也不该连带删掉用户的历史提交。
  // 与 problem.slug 的区别：problem 只在 ok 时有值，这里总是尽力填。
  std::string slug;
  std::vector<ValidationIssue> issues;    // 全部问题（不止第一个），按发现顺序
};

// 层 ① + 层 ② 全跑。text 为文件全文（调用方负责读；这里不碰文件系统）。
ParseResult parse_problem_json(const std::string& text, const ParseOptions& options);

// 读文件 + 解析。读不到 / 空文件都表现为一条 Issue（不是崩溃、不是空 ok=true），
// 因为加载器要能把"坏文件"与"好文件"一起汇总成启动日志（§5.6.1 末尾那行要求）。
ParseResult parse_problem_file(const std::string& path, const std::string& expect_slug);

// 多行文本，直接进启动日志；每行带 source 前缀，便于在 50 道题里定位到具体文件。
std::string format_issues(const std::vector<ValidationIssue>& issues, const std::string& source);

}  // namespace problem
}  // namespace oj
