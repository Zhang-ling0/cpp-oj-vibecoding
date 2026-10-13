// =============================================================================
// 工作区生命周期实现（SPEC.md §9 M2 T2.9 / §5.2）
// 被测对象：include/judge/workspace.h
//
// 【实现要点】
//   1. 删除只有一条路径：递归删除统一走 nftw(FTW_DEPTH | FTW_PHYS) + unlink/rmdir。
//      写两套（"目录用 system("rm -rf")"、 "文件用 unlink"）会让人在两处各自演化，
//      而其中一处很容易忘掉 FTW_PHYS —— 那正是【不跟随符号链接】唯一生效的地方。
//   2. 形状校验（纯数字末段 + 父目录恰好是 root）在【删除之前】做，且用字符串比较
//      而非 realpath：realpath 会把符号链接解析成真路径，于是
//      "/tmp/oj-judge/42 -> /etc" 通过校验后反而变成"删 /etc"，恰好是它要防的事。
//   3. mkdir 用 0700；已存在时只做【类型核对】不改权限 —— 改别人的目录权限
//      属于本模块没有资格做的事。
// =============================================================================
#include "judge/workspace.h"

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "common/config.h"
#include "judge/compiler.h"

namespace oj {
namespace judge {
namespace {

// 末段必须是纯数字：/^[0-9]+$/。submission_id 是 BIGINT UNSIGNED（§3.2），
// 写出来的目录名就是十进制数字串，因此这个判据足以区分"我们建的"与"别人放的"。
bool is_submission_dir_name(const std::string& name) {
  if (name.empty()) return false;
  for (const char c : name) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 去掉尾部多余的 '/'（root 可能是 "/tmp/oj-judge/"）。只处理尾部，不做归一化收缩，
// 因为 ".." 这类路径要靠下面的形状校验直接拒掉，而不是悄悄解析它。
std::string strip_trailing_slash(const std::string& path) {
  std::string out = path;
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

std::string parent_of(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return std::string();
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

std::string base_of(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string errno_text(int code) { return std::string(std::strerror(code)); }

// nftw 的回调：FTW_DEPTH 保证先删内容再删目录；FTW_PHYS 保证符号链接按链接本身删
// （跟随过去删 = 把 /etc 当子树清空，见头文件【删除纪律】②）。
// 返回非 0 会中止整棵遍历，因此这里只对"删不掉"返回 1，由调用方决定继续还是放弃。
int remove_entry(const char* fpath, const struct stat*, int typeflag, struct FTW*) {
  if (typeflag == FTW_DP) {
    if (::rmdir(fpath) == 0) return 0;
  } else {
    // FTW_DNR（目录读不了）走到这里会 unlink 一个目录并失败，如实返回错误即可：
    // 目录是 0700 且归本进程所有，读不了只可能是外部改动（root 权限的进程把它改了什么）。
    if (::unlink(fpath) == 0) return 0;
  }
  return 1;
}

// 递归删除 dir（不跟随符号链接）。返回 false 时 error 里有人可读的原因。
bool remove_tree(const std::string& dir, std::string* error) {
  // nopenfd=16：nftw 同时打开的 fd 上限，取小值即可（本工作区最多几个文件），
  // 目的是别在 1967MB 的机器上白占 fd 额度给别的模块。
  const int rc = ::nftw(dir.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
  if (rc == 0) return true;
  if (error != nullptr) {
    *error = "递归删除失败：" + dir + "（errno=" + errno_text(errno) + "）";
  }
  return false;
}

}  // namespace

std::string workspace_root() {
  const std::string& configured = common::config().judge_workdir;
  if (!configured.empty()) return strip_trailing_slash(configured);
  return kWorkspaceRoot;  // 兜底常量（judge/compiler.h），仅在未加载配置时走到
}

WorkspacePaths workspace_paths(const std::string& root, unsigned long long submission_id) {
  const std::string base = strip_trailing_slash(root);
  WorkspacePaths paths;
  paths.dir = base + "/" + std::to_string(submission_id);
  paths.user_source = paths.dir + "/" + kUserSource;
  paths.user_object = paths.dir + "/" + kUserObject;
  paths.tester_source = paths.dir + "/" + kTesterSource;
  paths.solution_decl = paths.dir + "/" + kSolutionDecl;
  paths.tester_util = paths.dir + "/" + kTesterUtil;
  paths.runner_binary = paths.dir + "/" + kRunnerBinary;
  return paths;
}

CreateResult create_workspace(const std::string& root, unsigned long long submission_id) {
  CreateResult result;
  const std::string base = strip_trailing_slash(root);
  if (base.empty()) {
    result.error = "工作区根目录为空（OJ_JUDGE_WORKDIR 不应为空串）";
    return result;
  }

  struct stat st;
  if (::stat(base.c_str(), &st) != 0) {
    if (errno != ENOENT) {
      result.error = "无法访问工作区根目录 " + base + "（errno=" + errno_text(errno) + "）";
      return result;
    }
    // 一次只建一级：root 的父目录通常是 /tmp（已存在）。用 mkdir 而非
    // std::filesystem::create_directories 是为了只申请 0700 的权限位，且不额外
    // 引入"中间目录用什么权限"这个本题用不上的问题。
    if (::mkdir(base.c_str(), 0700) != 0 && errno != EEXIST) {
      result.error = "无法创建工作区根目录 " + base + "（errno=" + errno_text(errno) + "）";
      return result;
    }
  } else if (!S_ISDIR(st.st_mode)) {
    result.error = "工作区根目录 " + base + " 不是目录";
    return result;
  }

  result.dir = workspace_paths(base, submission_id).dir;
  if (::mkdir(result.dir.c_str(), 0700) == 0) {
    result.ok = true;
    return result;
  }

  if (errno != EEXIST) {
    result.error = "无法创建工作区 " + result.dir + "（errno=" + errno_text(errno) + "）";
    return result;
  }

  // 已存在：只核对类型，不改权限。lstat 而不是 stat —— 后者会把符号链接解析成
  // 目标，而"这里摆着一个指向别处的链接"正是必须拒绝的情形：复用它会让编译
  // 产物写进别人的目录。
  if (::lstat(result.dir.c_str(), &st) != 0) {
    result.error = "工作区 " + result.dir + " 存在但无法读取（errno=" + errno_text(errno) + "）";
    return result;
  }
  if (!S_ISDIR(st.st_mode)) {
    result.error = "工作区 " + result.dir + " 已存在但不是目录（可能是符号链接），拒绝复用";
    return result;
  }
  result.ok = true;
  result.reused = true;
  return result;
}

bool remove_workspace(const std::string& root, const std::string& dir, std::string* error) {
  const std::string base = strip_trailing_slash(root);
  const std::string target = strip_trailing_slash(dir);

  // 形状校验（头文件【删除纪律】①③）：必须是 <root>/<纯数字>。
  // 用字符串比较而不是 realpath：realpath 会把符号链接解析成真路径，
  // 于是 "/tmp/oj-judge/42 -> /etc" 通过校验后变成"删 /etc" —— 恰好是它要防的事。
  if (parent_of(target) != base) {
    if (error != nullptr) {
      *error = "拒绝删除 " + target + "：它不在工作区根目录 " + base + " 之下";
    }
    return false;
  }
  if (!is_submission_dir_name(base_of(target))) {
    if (error != nullptr) {
      *error = "拒绝删除 " + target + "：目录名不是 submissionId 的数字形式";
    }
    return false;
  }

  // 已经不存在 = 清理已经完成。清理动作必须幂等（worker 的 finally 里调用它）。
  if (::access(target.c_str(), F_OK) != 0) {
    if (errno == ENOENT) return true;
    if (error != nullptr) {
      *error = "无法访问 " + target + "（errno=" + errno_text(errno) + "）";
    }
    return false;
  }
  return remove_tree(target, error);
}

SweepReport sweep_workspace(const std::string& root) {
  SweepReport report;
  const std::string base = strip_trailing_slash(root);

  DIR* dir = ::opendir(base.c_str());
  if (dir == nullptr) {
    if (errno == ENOENT) return report;  // 首次启动没有残留，不是错误
    report.failed = 1;
    report.notes = "无法读取工作区根目录 " + base + "（errno=" + errno_text(errno) + "）";
    return report;
  }

  // 逐条处理，坏条目不中断整轮扫残：50 道题的服务不该因为一个删不掉的目录就起不来。
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;

    if (!is_submission_dir_name(name)) {
      ++report.skipped;
      report.notes += "  保留（名字不是 submissionId）：" + base + "/" + name + "\n";
      continue;
    }

    std::string error;
    if (remove_workspace(base, base + "/" + name, &error)) {
      ++report.removed;
    } else {
      ++report.failed;
      report.notes += "  删除失败：" + error + "\n";
    }
  }
  ::closedir(dir);

  // 枚举期间目录可能被别人改动（另一个进程建了新工作区），因此 notes 的行数与
  // 计数不一定严格对应；这里不为此做二次核对 —— 扫残是尽力而为的清理动作，
  // 它的正确性由"下次启动再扫一遍"兜底，不需要锁。
  return report;
}

std::string SweepReport::summary() const {
  if (removed == 0 && skipped == 0 && failed == 0) return "工作区扫残：无需清理";
  std::string text = "工作区扫残：删除残留 " + std::to_string(removed) + " 个，保留 " +
                     std::to_string(skipped) + " 个，失败 " + std::to_string(failed) + " 个";
  return text;
}

}  // namespace judge
}  // namespace oj