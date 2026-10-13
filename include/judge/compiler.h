// =============================================================================
// CppOJ 编译模块 —— 两阶段编译 + 超时 + g++ 输出解析 + 归属分流 [D-64]
// 对应任务：SPEC.md §9 M2 T2.1「alarm 超时 + 两阶段编译 + 输出捕获」
//           + T2.2「g++ 输出正则解析 → submission_compile_errors 行（seq 保序），路径归一化」
// 规格来源：SPEC.md §5.2（工作目录与两条编译命令、编译超时 10s）
//           / §5.6（正则、路径归一化、user.cpp / harness / link 三种归属 [D-64]）
//           / §5.6.2（verdict 完备判定表的第 1~4 行，本模块只负责产出"观测到的事实"）
//           / §2.2（两阶段编译的理由）/ §3.4（compiler_output 是不可分的原子文本）
//           / §5.8（单次提交 64KB 的代码上限）/ §0.0（开发机 2 核 1967MB 的硬约束）
//
// 【本模块只做"观测"，不判 verdict】
//   §5.6.2 的判定表把 verdict 映射放在 worker（谁把观测写进 submissions.verdict 是
//   T2.11 的事）。这里刻意不产出 AC/CE/... —— 因为 §5.6.2 的第 5 行（0 条 @@OJ@@ → JE）
//   根本不是编译阶段的事，把 verdict 放进编译模块会诱使调用点"拿到 CompileResult 就直接
//   填 verdict"，那正好绕过第 5 行。本模块的产出是【阶段 + 诊断 + 原始输出】三样，
//   足够 worker 无歧义地推导 verdict，且推导规则集中在 worker 一处。
//
// 【两阶段为什么要分开编】（§2.2 的原文理由照抄，因为它决定了本文件的形状）
//   阶段一：g++-12 -std=c++17 -O2 -Wall -c user.cpp -o user.o
//   阶段二：g++-12 -std=c++17 -O2 user.o tester.cpp -o run
//   用户代码的语法/类型错误在阶段一暴露，此时 tester.cpp 尚未参与，报错信息干净；
//   签名不匹配（方法名/参数类型/返回类型不一致）在阶段二链接期暴露。
//   ⚠️ 由此产生一个必须记住的推论：**阶段二失败永远不是"用户代码语法错"** ——
//     阶段二参与编译的 tester.cpp / solution_decl.h / tester_util.h 是平台生成的，
//     它们编译不过说明【题库配置有误】（§5.6 的 harness 归属）。用户在那道题上无论写
//     什么都过不了。这就是 D-64 要单独发明一种归属、并且给它一份"这不是你的代码出错"
//     文案的原因，也是 D-64 要在【题库加载期】再做一层静态校验（§5.6.1）的原因。
//
// 【开发机实测约束：编译闸门默认 1】（§0.0「开发机硬约束」）
//   本机 2 核 / 1967MB，mysqld 常驻后可用内存常只剩 300MB 上下。一个 `g++ -O2` 峰值
//   在 100MB 量级，两个同时跑再加 cc1plus 的 fork 就会把机器推进 swap，表现为
//   "服务卡死"而不是"编译变慢"。因此本模块自带一把【进程级编译闸门】，默认容量 1：
//   一次只允许一个线程进入两阶段编译，其余排队。
//   ⚠️ 这与 §2.3 的"全局评测闸门 4"不是同一个东西，别混：
//     · 评测闸门 4 限制的是【子进程执行】（每个都受 RLIMIT_AS 128MB 约束，很轻）
//     · 编译闸门 1 限制的是【编译器自身】（不受 RLIMIT_AS 约束，重）
//   闸门的另一个作用是让 SIGALRM 可用：alarm() 是进程级的，N 个线程各设一个 alarm
//   会互相覆盖（后设的顶掉先设的），只有一个线程在编译期时这个问题不存在。
//
// 【输出捕获的三条硬规则】
//   1. 合并 stdout 与 stderr 到同一根管道。g++ 的诊断走 stderr、`collect2` 的摘要走
//      stderr，但模板实例化失败时还有别的东西走 stdout；分两根管道就必须事后按时间戳
//      归并，而归并出来的顺序与用户看到的不同。合并成一根则顺序即原序。
//   2. 【有上限地读】。用户可以写 `#error` 之外的东西诱发海量输出（template 深度展开），
//      无上限的读会把 judge worker 的内存吃光 —— 在 1967MB 的机器上这是最容易踩的雷。
//      超上限即截断 + 杀进程组，并在 output 末尾留一行显式标记（截断必须是可见的，
//      否则用户看到一段"看起来完整"的输出却少了尾巴，排障时会被误导）。
//   3. 原始输出【永远保留】。§5.6 的最后一句是规格：未命中正则的行留在
//      submissions.compiler_output 全文里，它是用户排障的最后一道兜底。
//
// 【超时机制：alarm + 墙钟双判，两者都走进程组 kill】
//   规格要求 `alarm()` + SIGALRM handler → 记为 CE。实现上 handler 只置一个
//   `volatile sig_atomic_t` 标志，真正的 kill 由回收循环发起，原因有二：
//     · 在信号处理函数里调 kill/waitpid 都不是异步信号安全的，出了问题只会更难查；
//     · g++ 会 fork 出 cc1plus 与 collect2，杀直接子进程会留下孤儿进程继续吃 CPU，
//       而 §5.4.3 明确要求两条超时路径都必须走 kill(-pgid)。
//   墙钟判据是 alarm 的兜底：alarm 的粒度是 1 秒，且 SIGALRM 在多线程下可能落到别的
//   线程上（编译闸门=1 已把这个面压到 0，但仍保留这道判据以防有人把闸门调大）。
//
// 【不做的事】
//   · 不生成 tester.cpp（judge/generator.h 归 T1.3~T1.6）：本模块假定工作区里已经有
//     user.cpp / tester.cpp / solution_decl.h / tester_util.h，文件从哪来它不关心
//   · 不建/删工作区（§5.2 的 rm -rf 与启动扫残归 T2.9）：本模块对目录只读不写，
//     唯一例外是编译产物 user.o 与 run
//   · 不写库（写回归 T2.11）、不打 verdict（见本文件头部）
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oj {
namespace judge {

// ---- §5.2 的工作区文件名（唯一真源，生成器与 runner 都从这里取）-----------------
inline constexpr const char* kWorkspaceRoot = "/tmp/oj-judge";
inline constexpr const char* kUserSource = "user.cpp";
inline constexpr const char* kUserObject = "user.o";
inline constexpr const char* kTesterSource = "tester.cpp";
inline constexpr const char* kSolutionDecl = "solution_decl.h";
inline constexpr const char* kTesterUtil = "tester_util.h";
inline constexpr const char* kRunnerBinary = "run";

// §5.2「编译超时：10s」
inline constexpr unsigned int kDefaultCompileTimeoutMs = 10000;

// ---- 失败发生在哪一步（§5.6.2 判定表第 1/3/4 行的唯一区分依据）-----------------
// kNone      ：没有阶段失败。【成功时也保持 kNone】—— 它的含义是"哪一步都没栽"，
//              既包括编译成功，也包括"根本没编"（工作区缺 user.cpp / spawn 失败）。
//              调用点要区分这两者时看 ok，不看 stage
// kUserObject：阶段一失败 → CE，且诊断归属恒为 user.cpp（用户代码）
// kHarnessLink：阶段二失败 → CE，诊断归属是 harness（题库配置有误）或 link（签名不一致）
enum class CompileStage { kNone, kUserObject, kHarnessLink };

// 诊断的归属（[D-64]）。它决定前端【要不要行高亮】，因此是数据而不是展示层的判断：
//   kUserCode → file_name='user.cpp'，编辑器标红行
//   kHarness  → file_name='harness'，不提供行号，显示"这不是你的代码出错"
//   kLink     → file_name='link'，不提供行号，显示签名核对清单
enum class DiagnosticOwner { kUserCode, kHarness, kLink };

// 与 §3.2 的 ENUM('error','warning','note') 逐字对应
enum class DiagnosticSeverity { kError, kWarning, kNote };

const char* to_string(CompileStage stage);
const char* to_string(DiagnosticOwner owner);
const char* to_string(DiagnosticSeverity severity);

struct CompileDiagnostic {
  unsigned int seq = 0;        // 出现顺序（§3.2 uk_sce_seq），1-based
  std::string file_name;       // 'user.cpp' | 'harness' | 'link' —— 前端据此决定是否高亮
  unsigned int line = 0;       // 1-based；0 = 该归属本就无定位信息（§5.6 的 link 归属）
  unsigned int col = 0;
  DiagnosticSeverity severity = DiagnosticSeverity::kError;
  DiagnosticOwner owner = DiagnosticOwner::kUserCode;
  std::string message;         // harness / link 归属下是专属文案（D-64），原始文本仍在 output
};

struct CompileOptions {
  // D-13：工具链锁 g++-12。刻意【不带版本兜底】：找不到就 spawn 失败并如实上报，
  // 而不是悄悄换成本机的 g++（版本不同则题目在不同机器上表现不同，那是最难查的一类 bug）。
  std::string compiler = "g++-12";
  std::string language_standard = "-std=c++17";
  std::string optimization = "-O2";
  std::string warnings = "-Wall";
  unsigned int timeout_ms = kDefaultCompileTimeoutMs;  // §5.2
  // 输出上限。默认 256KB：一次语法错误通常只产出几 KB，而 template 深度展开能把
  // 输出顶到几十 KB；256KB 足够把话说全，又不至于让一个畸形提交吃掉 worker 的内存。
  std::size_t max_output_bytes = 256u * 1024u;
  // 往 judge.log 记命令与输出（T2.10）。单测可关掉保持输出干净。
  bool write_judge_log = true;
};

struct CompileResult {
  bool ok = false;
  CompileStage stage = CompileStage::kNone;
  bool timed_out = false;
  int exit_code = -1;
  int term_signal = 0;
  bool output_truncated = false;
  bool user_object_created = false;
  bool binary_created = false;
  std::string output;                         // 两阶段合并的原始输出全文（§3.4 原子文本）
  std::vector<CompileDiagnostic> diagnostics;  // 已归属、已保序（seq 从 1 起）
  std::vector<std::string> commands;          // 实际执行的两条命令行（T2.10 记进 judge.log）
};

// [D-64] 两种"不是用户代码错"的专属文案。放在本模块而不是前端，是为了让
// 「生成 → 落库 → API → 前端」四段共用【同一份字符串】：题库里那句
// "这不是你的代码出错…" 一旦在前端被改写，它与落库时的文案就不再是同一件事。
const char* harness_ownership_message();
const char* link_signature_message();

// 纯文本 → 诊断列表。纯函数：不碰文件系统、不fork，因此单测可以穷举。
//   · 命中的行按出现顺序编号（seq 保序，§3.2 uk_sce_seq）
//   · 未命中的行（`In function ...`、源码摘录、^~~~ 指示符、collect2 摘要）不进本表，
//     它们原样留在 CompileResult::output 里
//   · 输出里出现 undefined reference / undefined symbol 时，额外在【末尾】追加一条
//     owner=link 的诊断（§5.6 的链接期归属）
std::vector<CompileDiagnostic> parse_compile_output(const std::string& output);

// 两阶段编译。workspace_dir 里应已有 user.cpp / tester.cpp。
//   · 不创建、不删除工作区（§5.2 的生命周期归 T2.9），只产出 user.o 与 run
//   · 任一阶段失败【立即返回】，不做第二阶段（spec 的两阶段本就是"上一阶段通过才继续"）
//   · user.cpp 缺失时返回 stage=kNone —— 调用点据此判 JE 而非 CE（题库/生成环节的问题，
//     §5.6.2 判定表里没有它）
CompileResult compile_submission(const std::string& workspace_dir,
                                 const CompileOptions& options = CompileOptions{});

// ---- 编译闸门（开发机 2 核 / 1967MB 的落地，见本文件头部【开发机实测约束】）------
// 容量默认 1。0 不是"无限并发"而是无效配置，会让所有编译永远排队 —— 按 1 处理。
// 它是【进程级】的而不是每次调用的：它约束的是这台机器，不是某次提交。
unsigned int compile_max_concurrent();
void set_compile_max_concurrent(unsigned int slots);

}  // namespace judge
}  // namespace oj