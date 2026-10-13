// =============================================================================
// 工作区生命周期的单元测试（gtest）
// 被测：include/judge/workspace.h + src/judge/workspace.cpp（SPEC.md §9 M2 T2.9）
// 运行：make test（即 ctest，工作目录固定为仓库根）
//
// 【为什么这些用例必须真跑文件系统】本模块的价值全在【删除纪律】上，而那三条纪律
//   （只删自己命名的形状 / 不跟随符号链接 / 不越界）一旦写错，症状是"某天早上
//   /etc 少了一个文件"，而不是测试挂掉。纯函数化的 mock 恰好把最危险的那一层
//   （递归遍历 + unlink/rmdir 的真实语义）换成了假的。因此这里全部用 mkdtemp
//   在临时目录里造真实的目录 / 文件 / 符号链接。
//
// 【为什么不需要 MySQL】本模块不碰 DB（workspace.h【本模块只管目录，不管编译】），
//   因此这批用例在本机没起 mysqld 时也照跑不误 —— 与 mysql_pool_test 那批不同。
//
// 【每个用例自建自销】根目录一律 mkdtemp 出 /tmp/ojws-XXXXXX，TearDown 里整棵删掉；
//   绝不用仓库里的 judge_workdir（那可能是用户正在跑的判题工作区）。
// =============================================================================
#include <gtest/gtest.h>

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "judge/compiler.h"
#include "judge/workspace.h"

namespace {

using oj::judge::CreateResult;
using oj::judge::SweepReport;
using oj::judge::WorkspacePaths;

// 判断路径是否存在（区分"不存在"与"不可访问"：ENOTDIR/EACCES 也要算存在）
bool path_exists(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0;
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

// 目录里除了 "." 与 ".." 之外什么都没有。注意不能只判第一次 readdir 是否为空 ——
// 那两条永远存在，直接判会把每个空目录都判成"非空"。
bool dir_has_no_entries(const std::string& path) {
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) return false;
  bool empty = true;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") empty = false;
  }
  ::closedir(dir);
  return empty;
}

class WorkspaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/ojws-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    ASSERT_NE(made, nullptr) << "mkdtemp 失败，无法准备测试目录";
    root_ = made;
  }

  void TearDown() override {
    if (root_.empty()) return;
    std::string error;
    // 递归删根目录：借本模块自己的 remove_workspace 的删除原语是不可行的
    // （它只接受 <root>/<数字> 形状），所以这里用 system 删一次性测试目录。
    // 这是【测试代码】里唯一允许出现 rm -rf 的地方：路径由 mkdtemp 生成，
    // 不是一个可被外部影响的变量。
    const std::string command = "rm -rf '" + root_ + "'";
    const int rc = std::system(command.c_str());
    (void)rc;
    root_.clear();
  }

  std::string at(const std::string& name) const { return root_ + "/" + name; }

  // 在 root 下造一个带内容的工作区目录（模拟一次编译跑完的样子）
  std::string make_workspace(unsigned long long id, bool with_content = true) {
    const CreateResult created = oj::judge::create_workspace(root_, id);
    EXPECT_TRUE(created.ok) << created.error;
    if (with_content) {
      const WorkspacePaths paths = oj::judge::workspace_paths(root_, id);
      write_file(paths.user_source, "class Solution { public: int f() { return 0; } };");
      write_file(paths.user_object, "\177ELF fake object");
      write_file(paths.tester_source, "// generated");
      write_file(paths.runner_binary, "\177ELF fake binary");
      EXPECT_EQ(::chmod(paths.runner_binary.c_str(), 0755), 0);  // 产物通常带执行位
    }
    return created.dir;
  }

  std::string root_;
};

// ---- 路径拼装（纯函数，不碰文件系统）--------------------------------------------

TEST_F(WorkspaceTest, PathsAreBuiltFromRootAndSubmissionId) {
  const WorkspacePaths paths = oj::judge::workspace_paths("/tmp/oj-judge", 42);
  EXPECT_EQ(paths.dir, "/tmp/oj-judge/42");
  EXPECT_EQ(paths.user_source, "/tmp/oj-judge/42/user.cpp");
  EXPECT_EQ(paths.user_object, "/tmp/oj-judge/42/user.o");
  EXPECT_EQ(paths.tester_source, "/tmp/oj-judge/42/tester.cpp");
  EXPECT_EQ(paths.solution_decl, "/tmp/oj-judge/42/solution_decl.h");
  EXPECT_EQ(paths.tester_util, "/tmp/oj-judge/42/tester_util.h");
  EXPECT_EQ(paths.runner_binary, "/tmp/oj-judge/42/run");
}

// 文件名必须取自 judge/compiler.h 的常量，不能在本模块另抄一份（头文件【实现要点】1）：
// 两处各写一份常量时，"编译时找不到 user.cpp"会表现为"偶尔有个用户提交被判 JE"。
TEST_F(WorkspaceTest, FileNamesComeFromTheCompilerConstants) {
  const WorkspacePaths paths = oj::judge::workspace_paths(root_, 7);
EXPECT_EQ(paths.user_source.substr(paths.dir.size() + 1), std::string(oj::judge::kUserSource));
      EXPECT_EQ(paths.user_object.substr(paths.dir.size() + 1), std::string(oj::judge::kUserObject));
      EXPECT_EQ(paths.tester_source.substr(paths.dir.size() + 1), std::string(oj::judge::kTesterSource));
      EXPECT_EQ(paths.solution_decl.substr(paths.dir.size() + 1), std::string(oj::judge::kSolutionDecl));
      EXPECT_EQ(paths.tester_util.substr(paths.dir.size() + 1), std::string(oj::judge::kTesterUtil));
      EXPECT_EQ(paths.runner_binary.substr(paths.dir.size() + 1), std::string(oj::judge::kRunnerBinary));
}

// root 带尾斜杠与不带必须给出同一个目录（.env 里写 OJ_JUDGE_WORKDIR=/tmp/oj-judge/ 很常见）
TEST_F(WorkspaceTest, TrailingSlashInRootDoesNotProduceDoubleSlash) {
  EXPECT_EQ(oj::judge::workspace_paths(root_ + "/", 9).dir, at("9"));
}

// ---- 创建 ----------------------------------------------------------------------

TEST_F(WorkspaceTest, CreateMakesRootAndSubmissionDirWithOwnerOnlyPermission) {
  const CreateResult created = oj::judge::create_workspace(root_, 11);
  ASSERT_TRUE(created.ok) << created.error;
  EXPECT_FALSE(created.reused);
  EXPECT_EQ(created.dir, at("11"));

  struct stat st;
  ASSERT_EQ(::stat(created.dir.c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  // 0700：§1.4 的安全需求是"用户代码不可读文件系统"，工作区里放的是用户源码与
  // 编译产物，同机其他本地用户不该读得到（头文件【权限用 0700 而不是 0755】）
  EXPECT_EQ(st.st_mode & 07777, 0700);
}

// 同一 id 重复进入时目录可以复用，但必须如实标出 reused —— 里面可能残留上一次的 .o/run
TEST_F(WorkspaceTest, CreateOnExistingDirectoryReportsReuse) {
  ASSERT_TRUE(oj::judge::create_workspace(root_, 12).ok);
  const CreateResult again = oj::judge::create_workspace(root_, 12);
  EXPECT_TRUE(again.ok);
  EXPECT_TRUE(again.reused);
}

// 同名条目是【文件】或【符号链接】时必须失败：复用它等于让编译产物写到别人的地方
TEST_F(WorkspaceTest, CreateRefusesToReuseAFile) {
  write_file(at("13"), "not a directory");
  const CreateResult created = oj::judge::create_workspace(root_, 13);
  EXPECT_FALSE(created.ok);
  EXPECT_NE(created.error.find("不是目录"), std::string::npos) << created.error;
}

TEST_F(WorkspaceTest, CreateRefusesToReuseASymlink) {
  const std::string outside = at("outside-target");
  ASSERT_EQ(::mkdir(outside.c_str(), 0700), 0);
  ASSERT_EQ(::symlink(outside.c_str(), at("14").c_str()), 0);

  const CreateResult created = oj::judge::create_workspace(root_, 14);
  EXPECT_FALSE(created.ok);
  // 关键不是错误文案，而是"没把链接当目录复用"：复用后 user.o 会写进 outside-target
  EXPECT_TRUE(dir_has_no_entries(outside)) << "符号链接目标被写入了内容";
}

TEST_F(WorkspaceTest, CreateFailsLoudlyWhenRootIsAFile) {
  const std::string bogus = at("not-a-root");
  write_file(bogus, "x");
  const CreateResult created = oj::judge::create_workspace(bogus, 15);
  EXPECT_FALSE(created.ok);
  EXPECT_NE(created.error.find("不是目录"), std::string::npos) << created.error;
}

TEST_F(WorkspaceTest, EmptyRootIsRejected) {
  const CreateResult created = oj::judge::create_workspace("", 16);
  EXPECT_FALSE(created.ok);
  EXPECT_NE(created.error.find("根目录"), std::string::npos) << created.error;
}

// ---- 删除（§5.2「worker 结束前 rm -rf」）----------------------------------------

TEST_F(WorkspaceTest, RemoveDeletesTheWholeTree) {
  const std::string dir = make_workspace(20);
  ASSERT_TRUE(path_exists(at("20/run")));

  std::string error;
  ASSERT_TRUE(oj::judge::remove_workspace(root_, dir, &error)) << error;
  EXPECT_FALSE(path_exists(dir));
}

// 幂等：worker 的清理在 finally 里调用，而"上一轮已经删过"不是失败
TEST_F(WorkspaceTest, RemoveIsIdempotent) {
  const std::string dir = make_workspace(21);
  std::string error;
  ASSERT_TRUE(oj::judge::remove_workspace(root_, dir, &error)) << error;
  EXPECT_TRUE(oj::judge::remove_workspace(root_, dir, &error)) << error;
  EXPECT_TRUE(oj::judge::remove_workspace(root_, root_ + "/999999", &error)) << error;
}

// 删除纪律 ③：不能删 root 自身，也不能删 root 之外的路径
TEST_F(WorkspaceTest, RemoveRefusesRootItself) {
  write_file(at("keep.txt"), "keep");
  std::string error;
  EXPECT_FALSE(oj::judge::remove_workspace(root_, root_, &error));
  EXPECT_NE(error.find("拒绝删除"), std::string::npos) << error;
  EXPECT_TRUE(path_exists(root_)) << "root 被删了";
  EXPECT_TRUE(path_exists(at("keep.txt")));
}

TEST_F(WorkspaceTest, RemoveRefusesPathOutsideRoot) {
  const std::string sibling = root_ + "-sibling";
  ASSERT_EQ(::mkdir(sibling.c_str(), 0700), 0);
  write_file(sibling + "/user.cpp", "outside");

  std::string error;
  EXPECT_FALSE(oj::judge::remove_workspace(root_, sibling + "/1", &error));
  EXPECT_NE(error.find("不在工作区根目录"), std::string::npos) << error;

  // 连父目录不是 root 的多级路径一并拒掉（"../" 形状同理，它同样过不了父目录比较）
  EXPECT_FALSE(oj::judge::remove_workspace(root_, root_ + "/1/nested", &error));
  EXPECT_FALSE(oj::judge::remove_workspace(root_, root_ + "/../1", &error));

  EXPECT_TRUE(path_exists(sibling + "/user.cpp")) << "root 之外的目录被删了";
  ::remove((sibling + "/user.cpp").c_str());
  ::rmdir(sibling.c_str());
}

// 删除纪律 ①：末段不是 submissionId 数字形状的一律不删
TEST_F(WorkspaceTest, RemoveRefusesNonNumericDirectoryName) {
  write_file(at("somebody-elses-file"), "x");
  std::string error;
  EXPECT_FALSE(oj::judge::remove_workspace(root_, at("somebody-elses-file"), &error));
  EXPECT_NE(error.find("数字形式"), std::string::npos) << error;
  EXPECT_TRUE(path_exists(at("somebody-elses-file")));
}

// 删除纪律 ②：工作区里的符号链接只删链接本身，绝不跟随到目标
TEST_F(WorkspaceTest, RemoveDoesNotFollowSymlinks) {
  const std::string precious = root_ + "-precious";
  ASSERT_EQ(::mkdir(precious.c_str(), 0700), 0);
  write_file(precious + "/important.txt", "do not delete me");

  const std::string dir = make_workspace(22);
  ASSERT_EQ(::symlink(precious.c_str(), (dir + "/link-to-outside").c_str()), 0);
  // 目录内部再套一层，符号链接在里面 —— 递归删除最容易在这里出事
  ASSERT_EQ(::mkdir((dir + "/nested").c_str(), 0700), 0);
  ASSERT_EQ(::symlink(precious.c_str(), (dir + "/nested/link").c_str()), 0);

  std::string error;
  ASSERT_TRUE(oj::judge::remove_workspace(root_, dir, &error)) << error;
  EXPECT_FALSE(path_exists(dir));
  EXPECT_TRUE(path_exists(precious + "/important.txt")) << "递归删除跟随了符号链接";

  ::remove((precious + "/important.txt").c_str());
  ::rmdir(precious.c_str());
}

// 删除一个"末段是数字的符号链接"时也只删链接：这是扫残会遇到的真实形状
TEST_F(WorkspaceTest, RemoveUnlinksASymlinkNamedLikeAWorkspace) {
  const std::string precious = root_ + "-precious2";
  ASSERT_EQ(::mkdir(precious.c_str(), 0700), 0);
  write_file(precious + "/important.txt", "still here");

  ASSERT_EQ(::symlink(precious.c_str(), at("23").c_str()), 0);
  std::string error;
  ASSERT_TRUE(oj::judge::remove_workspace(root_, at("23"), &error)) << error;
  EXPECT_FALSE(path_exists(at("23")));
  EXPECT_TRUE(path_exists(precious + "/important.txt"));

  ::remove((precious + "/important.txt").c_str());
  ::rmdir(precious.c_str());
}

// ---- 启动扫残（§5.2「服务启动时先扫描 /tmp/oj-judge/* 清空残留」）----------------

TEST_F(WorkspaceTest, SweepRemovesResidueAndKeepsForeignEntries) {
  make_workspace(30);
  make_workspace(31);
  // 形状不符的条目：只点名、不删（头文件【删除纪律】①）
  write_file(at("README"), "手工放的文件");
  ASSERT_EQ(::mkdir(at(".ipynb_checkpoints").c_str(), 0700), 0);
  write_file(at(".ipynb_checkpoints/junk"), "x");

  const SweepReport report = oj::judge::sweep_workspace(root_);

  EXPECT_EQ(report.removed, 2u);
  EXPECT_EQ(report.skipped, 2u);
  EXPECT_EQ(report.failed, 0u);
  EXPECT_FALSE(path_exists(at("30")));
  EXPECT_FALSE(path_exists(at("31")));
  EXPECT_TRUE(path_exists(at("README"))) << "扫残删掉了不属于本模块的文件";
  EXPECT_TRUE(path_exists(at(".ipynb_checkpoints/junk")));
  EXPECT_NE(report.notes.find("README"), std::string::npos) << report.notes;
  EXPECT_NE(report.summary().find("删除残留 2"), std::string::npos) << report.summary();
}

TEST_F(WorkspaceTest, SweepOnMissingRootIsNotAnError) {
  const SweepReport report = oj::judge::sweep_workspace(root_ + "/never-created");
  EXPECT_EQ(report.removed, 0u);
  EXPECT_EQ(report.failed, 0u);
  EXPECT_TRUE(report.notes.empty());
  EXPECT_NE(report.summary().find("无需清理"), std::string::npos) << report.summary();
}

TEST_F(WorkspaceTest, SweepOnEmptyRootReportsNothingToDo) {
  ASSERT_EQ(::mkdir(at("sub").c_str(), 0700), 0);
  const SweepReport report = oj::judge::sweep_workspace(root_);
  EXPECT_EQ(report.removed, 0u);
  EXPECT_EQ(report.skipped, 1u);  // "sub" 不是数字形状
  EXPECT_EQ(report.failed, 0u);
}

// 扫残必须在 judge 启动【之前】跑：这一条由 main.cpp 的启动顺序保证，
// 用例能验的是它的另一面 —— 扫残只认数字形状，因此"当前正在用的工作区"
// 若尚未删干净会被当成残留清掉。这正是必须排在 worker 之前的原因，
// 本例把它固化成文档：扫残不区分"新旧"，它没有时间概念。
TEST_F(WorkspaceTest, SweepHasNoNotionOfFreshness) {
  make_workspace(40);
  const SweepReport report = oj::judge::sweep_workspace(root_);
  EXPECT_EQ(report.removed, 1u) << "刚建的工作区也被扫掉了 —— 扫残只能用在 judge 启动之前";
}

// ---- 根目录取值 ----------------------------------------------------------------

// workspace_root() 必须与配置同源：§6.7.3 的 OJ_JUDGE_WORKDIR 是唯一的真源，
// 而 judge/compiler.h 的 kWorkspaceRoot 只是"没加载配置时"的兜底。两处各算一次
// 会让运维改了 .env 却发现工作区还在老地方 —— 表现为"重启后残留没被清掉"。
TEST_F(WorkspaceTest, WorkspaceRootFollowsConfig) {
  const std::string root = oj::judge::workspace_root();
  EXPECT_FALSE(root.empty());
  EXPECT_EQ(root, oj::common::config().judge_workdir);
  // 尾斜杠必须已被去掉：拼接是 root + "/" + id，保留尾斜杠会拼出 "//"
  if (root != "/") {
    EXPECT_NE(root.back(), '/') << root;
  }
}

// ---- 并发 ----------------------------------------------------------------------
// judge worker 是【多个线程】（§2.3：worker × 2），而工作区是它们每个都要创建与
// 删除的。因此本模块必须可被多线程同时调用且互不干扰 —— 不是因为内部有共享状态
// （没有），而是因为"两个线程各自建各自的工作区"是真实并发形态，且一旦有人在
// create/remove 里加了进程级缓存或临时名，它就会在这里被抓住。

TEST_F(WorkspaceTest, ConcurrentCreateAndRemoveDoNotInterfere) {
  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t]() {
      // 编号从 100 起：避开本文件其它用例用过的号段，防止"某个用例顺手扫掉别人的"
      // 这类互相干扰在断言里被误读成竞态
      const unsigned long long id = 100 + static_cast<unsigned long long>(t);
      const CreateResult created = oj::judge::create_workspace(root_, id);
      EXPECT_TRUE(created.ok) << created.error;
      const WorkspacePaths paths = oj::judge::workspace_paths(root_, id);
      write_file(paths.user_source, "int main() { return 0; }");
      EXPECT_TRUE(path_exists(paths.user_source));

      std::string error;
      EXPECT_TRUE(oj::judge::remove_workspace(root_, paths.dir, &error)) << error;
      EXPECT_FALSE(path_exists(paths.dir));
    });
  }
  for (std::thread& thread : threads) thread.join();

  // 一个都不能留：漏删的那个在生产里就是下一次启动的"残留"，而扫残会把它连同
  // 别的提交目录一起清掉 —— 所以"漏删"不是浪费磁盘，是会误删的隐患
  const SweepReport report = oj::judge::sweep_workspace(root_);
  EXPECT_EQ(report.removed, 0u) << "有工作区没删干净";
}

}  // namespace