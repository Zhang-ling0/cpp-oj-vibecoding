// =============================================================================
// CppOJ 题库加载器 —— 读目录 → 校验 → 按 §4.1 映射表同步 MySQL → 灌进程内只读缓存
// 对应任务：SPEC.md §9 M1 T1.2「problem/loader.h：读目录 → 校验必填字段 → 同步 MySQL」
// 规格来源：SPEC.md §4.1（映射表 / delete-then-insert 同步语义）
//           / §4.3（题目录入工作流 D-30）/ §5.6.1（跳过该题而非整库失败 D-64）
//           / §3.4（case_count 的维护契约：只由加载器在同事务刷新）
//           / §2.1-A 第 1 条 / §2.3 启动顺序第 5 步 / [D-30] 题库是唯一真源
//           / [D-21] DB 访问串行化（db/mysql_pool.h 是唯一入口）
//
// 【本模块的边界】
//   · 解析与校验在 problem/json_schema.h —— 本模块不重复实现任何一条校验
//   · SQL 只在这里（加载侧）。查询侧（列表页 / 详情页 / 统计）的 DAO 归 T3.2，
//     两者不会重叠：加载是"按 slug 幂等重写一整题"，查询是"按条件取若干行"，
//     语义、事务边界、返回形状都不同，硬合成一个函数只会两边都别扭。
//
// 【同步语义：delete-then-insert，不是逐行 upsert】§4.1 已定，理由照抄一遍是因为
//   它决定了本模块的形状：题库是"只增不改"的文件，改 JSON 后要能【删用例】。
//   逐行 upsert（比对已有行再决定改/插/删）需要把 DB 里的旧行读出来在应用层比对，
//   而那一刻就出现了两个真源（文件与 DB 快照）。先 DELETE 再整体 INSERT 则只有一条
//   真相：DB 里该题的用例行 = 当前 JSON 的 cases 数组。代价是 case_idx 主键会整体换新，
//   但 problem_cases 没有任何外键指向它（§3.1：提交结果存的是快照），换新不产生影响。
//
// 【一题一个事务，不是全库一个事务】
//   §5.6.1 要求"跳过该题不阻塞其他题"。若把 50 道题放进同一个事务，一道题写库失败就要
//   把已写好的 49 道一起回滚 —— 那与决策直接相反。反过来，校验是在【写之前】完成的，
//   所以进事务的题一定是"打算写成功"的，事务内失败只可能是 DB 自身的问题。
//
// 【为什么不删"文件已删除"的题】
//   problems.submissions 的外键是 ON DELETE CASCADE（§3.2）。删一道题的行，
//   连带删掉的是【所有用户在那道题上的全部提交历史与逐用例明细】——
//   而 §3.1 立这条 CASCADE 时想保的正是"历史可回放"。题库文件被删只说明"我不想再考它了"，
//   不等于"用户的练习记录该消失"。因此：
//     · 新题校验不过 → 不插行（§5.6.1 的第一种处置）
//     · 已有题校验不过 → 保留 problems 行，清空 problem_cases / problem_params /
//       problem_tags 并把 case_count 置 0（§5.6.1 的第二种处置"标记跳过"），
//       但历史提交一条不少。
//   ⚠️ 【实测修正，2026-10-15】这一版曾写着"列表页与缓存都不再出现它"——那是**错的**，
//     已由 tests/run_problemsync_test.sh 实测钉死：
//       · 读侧缓存（problem::cache）确实不再有它，因为那道题的文件没通过校验；
//       · 但【列表端点是直接查 DB 的】（problem_api.cpp 的 FROM problems，没有"失效题"过滤），
//         所以它**仍会出现在列表页**，只是一个 case_count=0、点进去没有用例的空题。
//     与 loader.cpp 里那句注释（「宁可库里留一道"坏题"（它会在列表页显示为 case_count=0
//     的空题，用户点进去才发现用例为空）」）一致，以那句为准。
//     要让列表真的过滤掉它，需要给 problems 加一个失效标记列并在两处读侧都带上条件 ——
//     这不是本轮要做的，写在这里是为了别再有人照着上面那句错的描述去验收。
//
// 【缓存为什么是 shared_ptr 而不是裸 vector】
//   §2.3 的启动顺序里，本模块跑在 judge 与 HTTP 之前，且 D-30 规定改题必须重启，
//   因此"读缓存"实际上只在启动后发生一次……但接口不该把"没人会在运行期重载它"
//   当成不变量：一旦将来有人加了重载入口，裸指针/引用就会在重载瞬间悬空，
//   而这种 bug 表现为"重启后偶发段错误"，几乎不可能当场定位。
//   故查找接口返回 shared_ptr<const T>：多一次引用计数，代价可忽略，换来没有悬空态。
//
// 【线程】只在启动线程调用（§2.3 第 5 步）。运行期并发只读缓存，由 §3.2 的
//   shared_ptr 原子性保证；DB 侧由 db::Pool 的全局互斥量串行化（D-21）。
// =============================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "model/problem.h"

namespace oj {
namespace problem {

// 一道被跳过的题。§5.6.1 要求"ERROR 日志点名 + 启动日志汇总所有被跳过的题"，
// 因此这里既保留结构化的 slug（用于汇总那一行），也保留逐条原因（用于定位）。
struct SkippedProblem {
  std::string file;     // 目录下的文件名（不是全路径：目录本身已在日志的行首出现过）
  std::string slug;     // 能解析出来就有；连 JSON 语法都错时为空
  std::vector<std::string> reasons;  // 每条一行，已含 JSON 路径与类别
};

struct LoadReport {
  bool ok = false;              // 目录可读 + （写库时）表齐全 + 无一道题写库失败
  std::string dir;
  std::size_t files_scanned = 0;
  std::size_t loaded = 0;       // 校验通过且已落库（write_to_db=false 时为"校验通过"）
  std::size_t skipped = 0;      // 校验不通过而跳过（§5.6.1）
  std::size_t deactivated = 0;  // 校验不通过、但库里有旧行且已被清空标记
  std::size_t db_failed = 0;    // 校验通过却写库失败 —— 基础设施问题，ok 会变 false
  std::size_t cases_written = 0;
  std::size_t params_written = 0;
  std::size_t tag_links_written = 0;
  std::vector<SkippedProblem> skipped_problems;
  std::vector<std::string> problems;  // 整体级原因（目录不存在 / 表不存在 / 单题写库失败）

  std::string summary() const;  // 一行：题库同步 完成/失败：扫描 N 个文件 …
  std::string format() const;   // 多行：汇总 + 每道被跳过的题的逐条原因
};

struct LoadOptions {
  std::string dir;          // 空 = 取 config().problems_dir（§6.7.3 的 OJ_PROBLEMS_DIR）
  // false = 只解析校验并灌缓存，不碰 DB。给"题库自检"与单测用：录题时可以在不写库
  // 的前提下看到全部问题，且不会把半套数据写进库（§2.2 的运维模式：改文件 + 重启）。
  bool write_to_db = true;
};

// 读目录下全部 *.json，逐个解析校验，按 slug 幂等同步，最后发布只读缓存。
// 全程记日志（app 通道）：每道被跳过的题一条 ERROR + 末尾一条汇总（§5.6.1）。
LoadReport load_all(const LoadOptions& options = LoadOptions{});

// ---- 进程内只读缓存（供 judge worker 使用，§2.1-A「取题 + 读用例(缓存)」）--------
using Cache = std::vector<model::ProblemBundle>;
using CachePtr = std::shared_ptr<const Cache>;

// 当前缓存快照（可能是空缓存：题库未加载或加载失败）。
// 快照一旦发布就不再改动，因此同一份 CachePtr 可被任意线程并发读。
CachePtr cache();
std::shared_ptr<const model::ProblemBundle> find_by_slug(const std::string& slug);
std::shared_ptr<const model::ProblemBundle> find_by_id(int id);
std::size_t cache_size();

// 仅供单元测试：清空缓存
void clear_cache();

}  // namespace problem
}  // namespace oj
