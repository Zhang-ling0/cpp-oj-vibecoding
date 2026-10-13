// =============================================================================
// CppOJ 题目数据模型 —— 题库 JSON 的【进程内表示】+ §4.2 的形状推导原语
// 对应任务：SPEC.md §9 M1 T1.2「problem/loader.h」的前半段 + T1.4「类型推导」
// 规格来源：SPEC.md §4.1（JSON → 库的字段映射表）/ §4.2（字面量映射规则）
//           / §5.6.1（题库类型一致性静态校验 D-64）/ §3.4（受控反范式的维护契约）
//           / §3.1（用例/形参拆表的 1NF 理由）/ §4.3（题目录入工作流）
//
// 【为什么模型层要带 nlohmann::json 而不是纯文本】
//   §4.1 的映射表把 cases[].args / expected 落成 MEDIUMTEXT 文本列，那是【落库形态】；
//   而加载期校验（§5.6.1 的「容器深度」「元素类型」两条）与生成期（§4.2/T1.3 的
//   「JSON 值 → C++ 字面量」）都需要【结构化】的值。若内存模型只存文本，这两处就得
//   各自再 parse 一次，而 parse 的选项、容错、深度语义一旦不同步，两道校验就会给出
//   互相矛盾的结论 —— 那是"同一个 JSON 值在一处合法、另一处非法"这类最难查的 bug。
//   故本模块【以结构化值为真源】，文本只在 two 边界上产生：
//     · 写库边界  args_json() / expected_json()（§3.2 的 MEDIUMTEXT 列）
//     · 读库边界  parse_case_text()（未来 T3.2 的 DAO 从 TEXT 列还原成 json）
//   dump() 的键序是确定的（nlohmann 的 object 按键排序），因此同一份 JSON 反复同步
//   得到的文本逐字节相同 —— 这让"重启后用例文本不变"成为可断言的性质。
//
// 【为什么 ProblemBundle 既是「解析产物」又是「judge 的只读缓存单元」】
//   §2.1-A 的线程模型里，judge worker 取题 + 读用例走的是【内存缓存】，不再读文件、
//   也不再查库（库里那份是给列表页与 API 用的）。因此缓存单元必须自带
//   statement / signature / params / cases —— 即"题面 + 形参 + 用例"三件套，
//   少任何一件 judge 都得回源。用一个结构体同时承担"解析产物"与"缓存单元"，
//   意味着【解析时漏掉的字段不可能在缓存里凭空出现】：缓存不是从库里再拼一遍的副本。
//
// 【id 的含义：加载后才有意义】
//   题库 JSON 里没有 id（slug 才是题目的稳定标识）。ProblemBundle::id 由加载器在
//   upsert 后用 INSERT/UPDATE 的返回填上，供 judge 写回提交结果时免去一次回查。
//   纯解析（write_to_db=false 的自检模式）下 id == 0，这是"未落库"的确定标记。
//
// 【不做的事】
//   · 不含 DAO（db/dao.h 归 T3.2）：本模块不碰 SQL、不碰连接，只描述数据与形状
//   · 不含生成器（judge/generator.h 归 T1.3）：JSON → C++ 字面量的【文本拼接】在那边，
//     本模块只提供它需要的形状判定原语（避免两边各写一套深度推导）
//   · 不含进度/收藏：progress 由提交历史实时推导（§6.4），不在题目模型里
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace oj {
namespace model {

// ---- 枚举 ---------------------------------------------------------------------
// 与 §3.2 的 ENUM 列逐字对应：任何一侧改了都必须改另一侧，否则 MySQL 在
// STRICT_TRANS_TABLES 下会把越界值变成一条看不懂的 Data truncated 错误（§1.4 校验 ①）。

enum class Difficulty { kEasy, kMedium, kHard };
const char* to_string(Difficulty value);
// 大小写不敏感地解析；未命中返回 false 且不写出参（避免调用点忘记判返回值）
bool parse_difficulty(const std::string& text, Difficulty* out);

enum class TagCategory { kAlgorithm, kSource };
const char* to_string(TagCategory value);
bool parse_tag_category(const std::string& text, TagCategory* out);

// ---- 题目侧形参（problem_params 的内存形态）-----------------------------------

struct ProblemParam {
  unsigned int param_idx = 0;   // 形参序号，决定 harness 的调用顺序（§3.2 param_idx）
  std::string name;             // 如 nums
  std::string cpp_type;         // 如 std::vector<int>&（含引用符，原样存）

  // 该形参期望的容器嵌套层数（§4.2 类型推导）。std::vector<int> → 1，二维 → 2，int → 0
  int vector_depth() const;
};

// ---- 题目侧用例（problem_cases 的内存形态）-------------------------------------

struct ProblemCase {
  unsigned int case_idx = 0;   // 题内 0-based 序号，与 @@OJ@@ 的 idx 对齐（§5.3）
  nlohmann::json args = nlohmann::json::array();  // 数组：元素与 params 一一对应
  nlohmann::json expected = nullptr;              // 任意：标量 / 数组 / {"__expr": …}
  std::string brief;                              // 可选简述；空串落库为 NULL

  // 落库文本（§4.1 映射表第 4 行）。dump() 输出紧凑单行，键序确定（见本文件头部）。
  std::string args_json() const { return args.dump(); }
  std::string expected_json() const { return expected.dump(); }
};

// ---- 题目（judge 只读缓存单元）------------------------------------------------

struct ProblemBundle {
  int id = 0;                    // 落库后的自增主键；未落库为 0（见本文件【id 的含义】）

  std::string slug;              // URL 友好标识，如 two-sum（§3.2 UNIQUE）
  std::string title;
  Difficulty difficulty = Difficulty::kEasy;
  std::string statement_md;      // Markdown 原文，禁图片 [D-48]
  std::string hint_md;           // 空串 = 落库 NULL = 前端不渲染提示 tab（D-47）
  std::string signature;         // Solution 类声明
  std::string method_name;       // harness 调用的方法名
  std::string return_type;       // Solution 方法返回类型
  std::string harness_prelude;   // __expr 逃生舱的前置声明；空串 = 落库 NULL（§4.2）
  std::string code_template;     // 编辑器初始内容
  unsigned int time_limit_ms = 3000;    // §3.2 DDL 默认值
  unsigned int mem_limit_kb = 131072;   // AS 口径，非 RSS（D-63）；树/图题须逐题调大
  bool is_featured = false;
  int sort_no = 0;

  std::vector<ProblemParam> params;
  std::vector<ProblemCase> cases;
  std::vector<std::string> algorithm_tags;
  std::vector<std::string> source_tags;

  // case_count 是【受控反范式】列（§3.4）。内存里不存第二份：它恒等于 cases.size()，
  // 写库时由加载器在同一事务回填，读库时由 COUNT(problem_cases) 保证 —— 少一份副本
  // 就少一处可能与 cases.size() 悄悄不一致的地方。
  std::size_t case_count() const { return cases.size(); }

  std::vector<std::string> tags_of(TagCategory category) const;
  const ProblemParam* param(unsigned int idx) const;
  const ProblemCase* test_case(unsigned int idx) const;

  // 题库里任何一个用例用到 __expr（§4.2 逃生舱）时，harness_prelude 必须非空 ——
  // 静态校验第 7 条的判据。做成函数而不是让校验器自己遍历 cases，是为了让
  // 「这条题是否需要 prelude」只有一个定义。
  bool uses_expr() const;
};

// ---- §4.2 形状推导原语（T1.4）-------------------------------------------------
// 这一对原语是【加载期校验 §5.6.1】与【生成期转换 T1.3】共用的唯一定义处。
// 两边各写一套的代价不是"多几行代码"，而是同一份题库在两处得到不同结论，
// 而这种矛盾只会以"题目偶尔判错"的形式出现在线上。

struct CppTypeShape {
  int vector_depth = 0;      // std::vector<> 的嵌套层数；0 = 非容器
  std::string base_type;     // 剥掉 std::vector<> 与 & / * 后的类型名，如 int
  bool is_pointer = false;   // 元素是指针（TreeNode* 等，走 __expr 逃生舱）
  bool well_formed = false;  // cpp_type 能否被解析（括号不配对 → false）
  bool scalar_known = false; // base_type 是否属于【可静态判定 JSON 元素类型】的那批
};
// 解析 cpp_type：std::vector<std::vector<int>>& → depth=2, base=int
// 容忍 std::vector 与 vector 两种写法（code_template 里有 using namespace std;），
// 容忍 const / & / 空格；不认识的类型不报错，只是 scalar_known=false（放行，见下）。
CppTypeShape analyze_cpp_type(const std::string& cpp_type);
// 还原成给 人 看的类型串（"std::vector<std::vector<int>>"），错误消息用
std::string describe_cpp_shape(const CppTypeShape& shape);

struct JsonShape {
  int array_depth = 0;   // 顶层数组的嵌套层数：[[1,2],[3,4]] → 2；9 → 0
  bool is_expr = false;  // {"__expr": "buildTree(...)"} —— §4.2 逃生舱
  bool is_null = false;
  bool is_scalar = false;  // 布尔 / 数字 / 字符串（既不是数组也不是对象）
};
JsonShape analyze_json(const nlohmann::json& value);

// §4.2 逃生舱的判据：恰好是 {"__expr": "<C++ 表达式文本>"} 的对象。
// ⚠️ 必须是【恰好这一个键】：写成 {"__expr": "...", "note": "..."} 时生成器拿到的
//   仍是原样文本，而这类多余键十有八九是录题时的手误 —— 它会让"这题用了逃生舱"
//   的统计与 prelude 配对校验建立在错误的输入上，因此判 false（随后按普通对象报错）。
bool is_expr_value(const nlohmann::json& value);

// 判断 value 是否与 cpp_type 相容。
//   · 不相容 → 返回 false，reason 里带 json_path（如 cases[1].args[0]）+ 期望 + 实际
//   · 【无法判定则放行】：base_type 是自定义类型（TreeNode*、ListNode* 等）时，
//     本函数不猜。它只做"看得懂的那部分校验"，剩下的交给两道现成的兜底：
//     §5.6 的 harness 归属分流（错误文案指向题库而非用户）与 §4.2.1 的生成期校验（JE）。
//   · __expr 对象恒放行：逃生舱的语义就是"这段我自己负责"。
bool value_matches_cpp_type(const nlohmann::json& value, const std::string& cpp_type,
                            const std::string& json_path, std::string* reason);

// ---- 读库边界（T3.2 的 DAO 会用到，这里先给唯一入口）--------------------------
// 从 MEDIUMTEXT 列还原成结构化值。非法 JSON 返回 nullptr 且 reason 非空 ——
// 库里的文本只可能来自本模块的 dump()，真出现非法值说明有别的写入路径（见 header 的边界说明）。
nlohmann::json parse_case_text(const std::string& text, std::string* reason);

}  // namespace model
}  // namespace oj
