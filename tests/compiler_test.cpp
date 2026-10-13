// =============================================================================
// 编译模块单测（SPEC §9 M2 T2.1 / T2.2）
//
// 【测试为什么这么分：11 条纯解析 + 5 条真跑 g++】
//   §0.0 的开发机实测是 2 核 / 1967MB，mysqld 常驻后余量只有几百 MB。每多一条
//   "真跑一次编译"的用例就多一次 spawn 与峰值内存，而【解析逻辑】才是本模块最容易
//   写错的部分（归属分流 [D-64] 尤其如此：它错的时候不会崩，只会静默地把一条
//   tester.cpp 的错误当成用户代码的行号）。所以：解析用穷举字符串覆盖，编译链路只留
//   覆盖四种结局的最少用例（成功 / 阶段一失败 / 阶段二编译失败 / 阶段二链接失败），
//   加上闸门与超时各一条。
//
// 【真跑用例的前置条件】
//   需要 g++-12 在 PATH 上（D-13、§6.7.1）。没有就整体 GTEST_SKIP 而不是失败 ——
//   "环境没装好"与"代码有 bug"必须在校验时就能区分开，否则本机换台机器就红一片。
//
// 【临时目录】
//   每个用例在 mkdtemp 出来的独立目录里编译，跑完递归删掉（NF-4：压测后
//   /tmp/oj-judge 为空；单测同理，不能留下 user.o 与 run）。
// =============================================================================
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "judge/compiler.h"

namespace oj {
namespace judge {
namespace {

// ---- 临时工作区 ----------------------------------------------------------------

class Workspace {
 public:
  Workspace() {
    char pattern[] = "/tmp/oj-compiler-test-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (made != nullptr) path_ = made;
  }
  ~Workspace() { remove_all(path_); }

  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  const std::string& path() const { return path_; }
  bool valid() const { return !path_.empty(); }

  void write(const std::string& name, const std::string& content) const {
    std::ofstream out(path_ + "/" + name, std::ios::binary | std::ios::trunc);
    out << content;
  }

  bool exists(const std::string& name) const {
    struct stat st;
    return ::stat((path_ + "/" + name).c_str(), &st) == 0;
  }

 private:
  // 递归删干净（只有一层，但按目录枚举而不是逐个已知文件名）：用例会往工作区里写
  // 各种临时文件（user.o / run / 假编译器 / 标记文件），写死清单等于给"漏删"留后门，
  // 而漏删的目录会一直堆在 /tmp 上（NF-4 的同一条要求）。
  static void remove_all(const std::string& dir) {
    if (dir.empty()) return;
    DIR* handle = ::opendir(dir.c_str());
    if (handle != nullptr) {
      while (const dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        ::unlink((dir + "/" + name).c_str());
      }
      ::closedir(handle);
    }
    ::rmdir(dir.c_str());
  }

  std::string path_;
};

// ---- 可编译的最小题面（两个答案各一份，便于构造三种结局）-------------------------
// 与 problems/two-sum.json 的签名保持一致，因此这里的链接错误就是 D-64 说的
// "用户把方法名写成 twoSum2"，而不是一个与题库无关的假场景。
const char* kSignature =
    "#include <vector>\n"
    "using std::vector;\n"
    "class Solution {\n"
    " public:\n"
    "  std::vector<int> twoSum(std::vector<int>& nums, int target);\n"
    "};\n";

const char* kCorrectUserSource =
    "#include \"solution_decl.h\"\n"
    "std::vector<int> Solution::twoSum(std::vector<int>& nums, int target) {\n"
    "  for (std::size_t i = 0; i + 1 < nums.size(); ++i)\n"
    "    for (std::size_t j = i + 1; j < nums.size(); ++j)\n"
    "      if (nums[i] + nums[j] == target) return {static_cast<int>(i), static_cast<int>(j)};\n"
    "  return {};\n"
    "}\n";

const char* kTesterSourceText =
    "#include \"solution_decl.h\"\n"
    "#include <iostream>\n"
    "int main() {\n"
    "  Solution sol;\n"
    "  std::vector<int> nums = {2, 7, 11, 15};\n"
    "  int target = 9;\n"
    "  auto out = sol.twoSum(nums, target);\n"
    "  std::cerr << \"@@OJ@@{\\\"idx\\\":0,\\\"passed\\\":true}\" << std::endl;\n"
    "  return 0;\n"
    "}\n";

// 判定真跑用例能否执行的唯一依据：编译器在不在（§6.7.1 的 D-13 工具链）。
// "环境没装好"与"代码有 bug"必须在校验时就能区分开，否则换台机器就红一片。
bool compiler_available() {
  Workspace probe;
  if (!probe.valid()) return false;
  probe.write(kUserSource, "int main() { return 0; }\n");
  CompileOptions options;
  options.timeout_ms = 30000;
  options.write_judge_log = false;
  const CompileResult result = compile_submission(probe.path(), options);
  if (result.ok) return true;
  // execvp 失败时子进程写 "judge: 无法执行 …" 后以 127 退出 —— 那是环境未就绪。
  return result.output.find("无法执行") == std::string::npos;
}

class RealCompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!compiler_available()) GTEST_SKIP() << "本机没有可用的 g++-12（§6.7.1），跳过真编译用例";
  }
  CompileOptions options() const {
    CompileOptions opts;
    opts.timeout_ms = 30000;
    opts.write_judge_log = false;  // 单测不往 ./logs 写东西
    return opts;
  }
};

// ---- T2.2：解析（纯函数，不碰文件系统）------------------------------------------

TEST(CompilerParse, UserCodeDiagnosticKeepsFileLineAndMessage) {
  const std::string output =
      "/tmp/oj-judge/42/user.cpp:7:20: error: 've' was not declared in this scope\n"
      "    7 |     vecto<int> ans;\n"
      "      |     ^~~~\n";
  const auto diagnostics = parse_compile_output(output);
  ASSERT_EQ(diagnostics.size(), 1u);
  const CompileDiagnostic& d = diagnostics[0];
  EXPECT_EQ(d.seq, 1u);
  EXPECT_EQ(d.file_name, "user.cpp");
  EXPECT_EQ(d.line, 7u);
  EXPECT_EQ(d.col, 20u);
  EXPECT_EQ(d.severity, DiagnosticSeverity::kError);
  EXPECT_EQ(d.owner, DiagnosticOwner::kUserCode);
  EXPECT_EQ(d.message, "'ve' was not declared in this scope");
}

TEST(CompilerParse, RelativePathIsAttributedLikeAbsoluteOne) {
  // g++ 收到的是相对文件名（我们 chdir 进工作区后再编译），因此它回显的也是相对路径。
  // 归属判定只认 basename，两种形态必须得到同一结论，否则换个调用方式就换一套行为。
  const auto diagnostics = parse_compile_output("user.cpp:3:1: error: expected ';' before '}'\n");
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].file_name, "user.cpp");
  EXPECT_EQ(diagnostics[0].line, 3u);
}

TEST(CompilerParse, SeqIsStableAndSeverityIsKept) {
  const std::string output =
      "user.cpp:5:9: warning: unused variable 'x'\n"
      "user.cpp:5:9: note: 'x' declared here\n"
      "user.cpp:11:3: error: expected ';' before '}' token\n";
  const auto diagnostics = parse_compile_output(output);
  ASSERT_EQ(diagnostics.size(), 3u);
  EXPECT_EQ(diagnostics[0].seq, 1u);
  EXPECT_EQ(diagnostics[0].severity, DiagnosticSeverity::kWarning);
  EXPECT_EQ(diagnostics[1].seq, 2u);
  EXPECT_EQ(diagnostics[1].severity, DiagnosticSeverity::kNote);
  EXPECT_EQ(diagnostics[2].seq, 3u);
  EXPECT_EQ(diagnostics[2].severity, DiagnosticSeverity::kError);
  EXPECT_EQ(diagnostics[2].line, 11u);
}

TEST(CompilerParse, GeneratedFilesAreAttributedToHarness) {
  // D-64：阶段二编的是平台生成的文件，它编译不过说明【题库配置有误】，
  // 用户改 tester.cpp 毫无作用（下次提交会重新生成）。
  const std::string output =
      "tester.cpp:12:1: error: expected primary-expression before '}' token\n";
  const auto diagnostics = parse_compile_output(output);
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].file_name, "harness");
  EXPECT_EQ(diagnostics[0].owner, DiagnosticOwner::kHarness);
  EXPECT_EQ(diagnostics[0].message, harness_ownership_message());
}

TEST(CompilerParse, SolutionDeclHeaderIsAttributedToHarness) {
  const auto diagnostics =
      parse_compile_output("solution_decl.h:5:20: error: expected '>'\n");
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].file_name, "harness");
}

TEST(CompilerParse, SystemHeadersAreAttributedToHarnessNotUser) {
  // 模板实例化错误最常指向系统头。它同样不是用户代码 —— 归给 harness 才是诚实的说法。
  const std::string output =
      "/usr/include/c++/12/bits/stl_vector.h:1819:9: note: candidate: 'template<class ...\n";
  const auto diagnostics = parse_compile_output(output);
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].file_name, "harness");
  EXPECT_EQ(diagnostics[0].owner, DiagnosticOwner::kHarness);
}

TEST(CompilerParse, UndefinedReferenceBecomesLinkDiagnosticWithoutLine) {
  // §5.6：链接器输出没有 file:line:col，因此 line/col 必须为 0 ——
  // 前端据此【不提供行高亮】。填一个假的行号会让编辑器去标红一行无关代码。
  const std::string output =
      "/usr/bin/ld: /tmp/oj-judge/42/user.o: in function `main':\n"
      "user.o: in function `Solution::twoSum(std::vector<int>&, int)':\n"
      "/tmp/oj-judge/42/tester.cpp: In function `int main()':\n"
      "user.o: undefined reference to `Solution::twoSum2(std::vector<int>&, int)'\n"
      "collect2: error: ld returned 1 exit status\n";
  const auto diagnostics = parse_compile_output(output);
  ASSERT_EQ(diagnostics.size(), 1u);
  const CompileDiagnostic& d = diagnostics[0];
  EXPECT_EQ(d.file_name, "link");
  EXPECT_EQ(d.owner, DiagnosticOwner::kLink);
  EXPECT_EQ(d.line, 0u);
  EXPECT_EQ(d.col, 0u);
  EXPECT_EQ(d.severity, DiagnosticSeverity::kError);
  EXPECT_EQ(d.message, link_signature_message());
}

TEST(CompilerParse, UnmatchedLinesAreDroppedFromTable) {
  // 这些行不进 submission_compile_errors，但必须原样留在 output 里（§5.6 的降级规则）。
  const std::string output =
      "In file included from tester.cpp:3:\n"
      "tester.cpp: In function 'int main()':\n"
      "   12 |   }\n"
      "      |   ^\n"
      "collect2: error: ld returned 1 exit status\n"
      "make: *** [Makefile:7: run] Error 1\n";
  EXPECT_TRUE(parse_compile_output(output).empty());
}

TEST(CompilerParse, FatalErrorAndUserErrorBothParse) {
  const std::string output =
      "user.cpp:1:10: fatal error: bits/stdc++.h: No such file or directory\n"
      "user.cpp:2:1: error: 'x' was not declared in this scope\n";
  const auto diagnostics = parse_compile_output(output);
  ASSERT_EQ(diagnostics.size(), 2u);
  EXPECT_EQ(diagnostics[0].line, 1u);
  EXPECT_EQ(diagnostics[0].severity, DiagnosticSeverity::kError);
  EXPECT_EQ(diagnostics[1].seq, 2u);
}

TEST(CompilerParse, EmptyOutputYieldsNoDiagnostics) {
  EXPECT_TRUE(parse_compile_output("").empty());
  EXPECT_TRUE(parse_compile_output("\n\n").empty());
}

TEST(CompilerParse, HugeLineNumberDoesNotThrow) {
  // 溢出时钳到 UINT_MAX 而不是抛 std::out_of_range：这是 worker 的判定路径，
  // 一条诊断不该有能力把评测线程抛下去。
  const auto diagnostics =
      parse_compile_output("user.cpp:99999999999999999999:1: error: boom\n");
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].line, 4294967295u);
}

// ---- T2.1：工作区缺失（不 spawn 就能判定的分支）--------------------------------

TEST(CompilerWorkspace, MissingUserSourceIsNotCompileError) {
  Workspace workspace;
  ASSERT_TRUE(workspace.valid());
  workspace.write(kTesterSource, kTesterSourceText);

  CompileOptions options;
  options.write_judge_log = false;
  const CompileResult result = compile_submission(workspace.path(), options);
  EXPECT_FALSE(result.ok);
  // stage=kNone 而不是 kUserObject：调用点据此判 JE 而非 CE —— 这是"上游没准备好
  // 文件"，不是"用户代码编译不过"。§5.6.2 判定表里没有这一行。
  EXPECT_EQ(result.stage, CompileStage::kNone);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_FALSE(result.output.empty());  // 给 worker 的日志留一句人可读原因
}

TEST(CompilerWorkspace, NonExistingDirectoryIsHandled) {
  CompileOptions options;
  options.write_judge_log = false;
  const CompileResult result = compile_submission("/tmp/oj-judge-does-not-exist-12345", options);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.stage, CompileStage::kNone);
}

// ---- T2.1：两阶段真编译 --------------------------------------------------------

TEST_F(RealCompilerTest, TwoStageCompileSucceedsAndProducesBothArtifacts) {
  Workspace workspace;
  ASSERT_TRUE(workspace.valid());
  workspace.write(kSolutionDecl, kSignature);
  workspace.write(kUserSource, kCorrectUserSource);
  workspace.write(kTesterSource, kTesterSourceText);

  const CompileResult result = compile_submission(workspace.path(), options());
  EXPECT_TRUE(result.ok) << result.output;
  EXPECT_EQ(result.stage, CompileStage::kNone);
  EXPECT_FALSE(result.timed_out);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(workspace.exists(kUserObject));
  EXPECT_TRUE(workspace.exists(kRunnerBinary));
  ASSERT_EQ(result.commands.size(), 2u) << "§5.2 定义了两条命令，少一条说明阶段被跳过";
  EXPECT_NE(result.commands[0].find("-c user.cpp -o user.o"), std::string::npos);
  EXPECT_NE(result.commands[1].find("user.o tester.cpp -o run"), std::string::npos);
  EXPECT_TRUE(result.diagnostics.empty()) << "成功时不该有诊断：" << result.output;
}

TEST_F(RealCompilerTest, UserCodeSyntaxErrorIsCeOwnedByUser) {
  Workspace workspace;
  ASSERT_TRUE(workspace.valid());
  workspace.write(kSolutionDecl, kSignature);
  workspace.write(kUserSource, "class Solution { std::vector<int> twoSum( };  // 少了右括号\n");
  workspace.write(kTesterSource, kTesterSourceText);

  const CompileResult result = compile_submission(workspace.path(), options());
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.stage, CompileStage::kUserObject);
  EXPECT_EQ(result.exit_code, 1);
  ASSERT_FALSE(result.diagnostics.empty()) << "编译失败却零条诊断，解析一定坏了：" << result.output;
  for (const CompileDiagnostic& d : result.diagnostics) {
    EXPECT_EQ(d.owner, DiagnosticOwner::kUserCode) << d.message;
    EXPECT_EQ(d.file_name, "user.cpp");
    EXPECT_GT(d.line, 0u);
  }
  EXPECT_FALSE(workspace.exists(kUserObject)) << "阶段一失败就不该留下 user.o";
  EXPECT_FALSE(workspace.exists(kRunnerBinary));
  // §5.6 的最后一句：未命中的行留在原始输出里，它是用户排障的最后一道兜底
  EXPECT_NE(result.output.find("error:"), std::string::npos);
}

TEST_F(RealCompilerTest, HarnessErrorIsCeOwnedByHarnessNotUser) {
  // AC-42 的场景：题库生成出来的 tester.cpp 编译不过（这里用一句故意的语法错模拟）。
  // 关键不是"判 CE"（那由 worker 做），而是诊断归属必须是 harness 且带专属文案 ——
  // 归属错了，用户就会去编辑器里找一条根本不存在的标红行。
  Workspace workspace;
  ASSERT_TRUE(workspace.valid());
  workspace.write(kSolutionDecl, kSignature);
  workspace.write(kUserSource, kCorrectUserSource);
  workspace.write(kTesterSource,
                 "#include \"solution_decl.h\"\nint main() { return 0  // 缺分号\n");

  const CompileResult result = compile_submission(workspace.path(), options());
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.stage, CompileStage::kHarnessLink);
  EXPECT_TRUE(workspace.exists(kUserObject)) << "阶段一成功了，user.o 应当还在";
  EXPECT_FALSE(workspace.exists(kRunnerBinary));
  ASSERT_FALSE(result.diagnostics.empty());
  for (const CompileDiagnostic& d : result.diagnostics) {
    EXPECT_EQ(d.owner, DiagnosticOwner::kHarness) << d.message;
    EXPECT_EQ(d.file_name, "harness");
    EXPECT_EQ(d.message, harness_ownership_message());
  }
}

TEST_F(RealCompilerTest, MethodNameMismatchIsAttributedToLink) {
  // AC-43：用户把方法名写成 twoSum2。关键在于【用户代码本身是自洽的】
  // （自带类声明、不 include solution_decl.h），因此阶段一编译通过，
  // 阶段二链接期才暴露 —— 这正是 §2.2 说的"签名不匹配在链接期暴露"。
  // 若让 user.cpp 也 include solution_decl.h，阶段一就会先报"no declaration matches"，
  // 那样测到的是另一条路径，链接归属根本没被覆盖。
  Workspace workspace;
  ASSERT_TRUE(workspace.valid());
  workspace.write(kSolutionDecl, kSignature);
  workspace.write(kUserSource,
                 "#include <vector>\n"
                 "class Solution {\n"
                 " public:\n"
                 "  std::vector<int> twoSum2(std::vector<int>& nums, int target);\n"
                 "};\n"
                 "std::vector<int> Solution::twoSum2(std::vector<int>& nums, int target) {\n"
                 "  return {};\n"
                 "}\n");
  workspace.write(kTesterSource, kTesterSourceText);

  const CompileResult result = compile_submission(workspace.path(), options());
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.stage, CompileStage::kHarnessLink);

  bool has_link = false;
  for (const CompileDiagnostic& d : result.diagnostics) {
    if (d.owner != DiagnosticOwner::kLink) continue;
    has_link = true;
    EXPECT_EQ(d.file_name, "link");
    EXPECT_EQ(d.line, 0u);
    EXPECT_EQ(d.message, link_signature_message());
  }
  EXPECT_TRUE(has_link) << "输出里应能看出 undefined reference：" << result.output;
  EXPECT_NE(result.output.find("undefined reference"), std::string::npos);
}

TEST(CompilerTimeout, KillsTheCompilerBeforeItFinishes) {
  // §5.2 的编译超时。用一个"假装编译器"的脚本而不是"一个很难编译的源文件"：
  //   · 难编译的源文件在 2 核机器上要吃掉真实内存与数秒 CPU（§0.0 的硬约束），
  //     而且"到底多久算久"依赖机器，这条用例就只在慢机器上成立；
  //   · 脚本路径与真编译器完全一样（同一段 spawn / 同一段回收 / 同一条 kill 路径），
  //     于是超时这件事在 200ms 内就能确定性地验完。
  Workspace workspace;
  ASSERT_TRUE(workspace.valid());
  workspace.write(kUserSource, kCorrectUserSource);
  workspace.write("fake-gcc", "#!/bin/sh\ntouch started\nsleep 30\ntouch survived\n");
  ASSERT_EQ(::chmod((workspace.path() + "/fake-gcc").c_str(), 0700), 0);

  CompileOptions opts;
  opts.compiler = workspace.path() + "/fake-gcc";
  opts.timeout_ms = 400;
  opts.write_judge_log = false;
  const CompileResult result = compile_submission(workspace.path(), opts);

  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.timed_out);
  EXPECT_EQ(result.stage, CompileStage::kUserObject);
  EXPECT_FALSE(workspace.exists(kRunnerBinary));
  EXPECT_FALSE(workspace.exists(kUserObject));

  // started 存在 = 子进程真的起来了（否则这条用例可能只是"没找到编译器"）；
  // survived 不存在 = 进程组被 SIGKILL 了，它没有跑到 sleep 结束那一步。
  EXPECT_TRUE(workspace.exists("started"));
  EXPECT_FALSE(workspace.exists("survived")) << "超时后子进程仍在运行，kill 没生效";
  // 再等一小会儿：如果只是杀掉了直接子进程而留下孤儿，survived 会在此后出现。
  ::usleep(200 * 1000);
  EXPECT_FALSE(workspace.exists("survived"));
}

// ---- 开发机约束：编译闸门串行化 ------------------------------------------------

TEST(CompilerGate, DefaultIsOneAndZeroIsClampedToOne) {
  const unsigned int original = compile_max_concurrent();
  EXPECT_EQ(original, 1u) << "§0.0 的开发机实测 2 核/1967MB，默认必须是 1";
  set_compile_max_concurrent(0);
  EXPECT_EQ(compile_max_concurrent(), 1u) << "0 会让 CompileSlot 永远等下去（死锁），必须钳到 1";
  set_compile_max_concurrent(3);
  EXPECT_EQ(compile_max_concurrent(), 3u);
  set_compile_max_concurrent(original);
  EXPECT_EQ(compile_max_concurrent(), original);
}

TEST(CompilerGate, ConcurrentSubmissionsDoNotCompileInParallel) {
  // §0.0：两个 g++ -O2 同时跑就会把 1967MB 的机器推进 swap，表现为"服务卡死"。
  // 这条用例把它变成可断言的性质。
  //
  // 【为什么用"假编译器"而不是真 g++】① 真编译在 2 核实测机上每次几百毫秒且吃真实
  // 内存，两条线程并发跑反而让这台机器本身先出问题；② 闸门拦的是"同时占用编译槽的
  // 次数"，与被编译的东西是不是 g++ 无关 —— 同一段 spawn/回收路径，同一把锁。
  // 【为什么不直接数同时进入 compile_submission 的线程数】那会把"排队等闸门"也算成
  // 并发：被闸门挡住的那条线程同样待在 compile_submission 里。因此改成让子进程自己
  // 记录时间戳，比较两段执行区间是否重叠。
  const auto fake_compiler = [](const Workspace& w) {
    w.write(kUserSource, kCorrectUserSource);
    w.write("fake-gcc",
            "#!/bin/sh\ndate +%s%N >> events\nsleep 0.3\ndate +%s%N >> events\n");
    EXPECT_EQ(::chmod((w.path() + "/fake-gcc").c_str(), 0700), 0);
  };
  const auto events_of = [](const Workspace& w) {
    std::ifstream in(w.path() + "/events");
    std::vector<unsigned long long> stamps;
    std::string line;
    while (std::getline(in, line)) stamps.push_back(std::strtoull(line.c_str(), nullptr, 10));
    return stamps;
  };

  Workspace first;
  Workspace second;
  ASSERT_TRUE(first.valid() && second.valid());
  fake_compiler(first);
  fake_compiler(second);

  const unsigned int original = compile_max_concurrent();
  set_compile_max_concurrent(1);
  const auto run = [&](const Workspace& w) {
    CompileOptions opts;
    opts.compiler = w.path() + "/fake-gcc";
    opts.timeout_ms = 30000;
    opts.write_judge_log = false;
    compile_submission(w.path(), opts);
  };
  std::thread t1(run, std::cref(first));
  std::thread t2(run, std::cref(second));
  t1.join();
  t2.join();
  set_compile_max_concurrent(original);

  const std::vector<unsigned long long> a = events_of(first);
  const std::vector<unsigned long long> b = events_of(second);
  ASSERT_FALSE(a.empty()) << "假编译器根本没跑起来，这条用例等于没测";
  ASSERT_FALSE(b.empty());
  // 每次 compile 跑两个阶段 → 4 个时间戳。串行时一个工作区的 4 个戳要么全在另一个
  // 工作区之前，要么全在之后；只要有一对交错就说明两个编译同时占着 CPU。
  EXPECT_TRUE(a.back() <= b.front() || b.back() <= a.front())
      << "两次编译的时间区间有重叠，说明编译闸门没串行化（§0.0：1967MB 上两个 g++ "
         "同时跑会把机器推进 swap）";
}

}  // namespace
}  // namespace judge
}  // namespace oj