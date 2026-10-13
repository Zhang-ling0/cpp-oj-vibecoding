// =============================================================================
// CppOJ 工作区生命周期 —— §5.2「每次提交在 /tmp/oj-judge/<submissionId>/ 下生成」
//                           §5.2「产物清理：无论成功失败，worker 结束前 rm -rf」
//                           §5.2「服务启动时先扫描 /tmp/oj-judge/* 清空残留」
// 对应任务：SPEC.md §9 M2 T2.9「工作目录生命周期：创建/清理/启动时扫残」
//
// 【本模块只管目录，不管编译】
//   user.cpp / user.o / tester.cpp / solution_decl.h / tester_util.h / run 的
//   【文件名】由 judge/compiler.h 声明（那里的常量是真源），本模块只负责把它们
//   拼成路径、创建目录、删除目录。不重复定义任何文件名常量 —— 两处各写一份
//   就多出一份必须手工同步的副本，而漂移了没人会发现（"编译时找不到 user.cpp"）。
//   也不 spawn 任何进程：本模块不编译、不 fork，因此不受"编译闸门"与 §0.0 的
//   内存约束影响（它只做 mkdir / 递归 unlink，全过程不申请大块虚拟内存）。
//
// 【三条不可协商的删除纪律】
//   删除是不可逆的，而被删的路径来自"目录枚举"这种外部输入。因此：
//     ① 只删【本模块自己命名形状】的条目：<root>/<纯数字>。
//        别的名字（人工放进去的文件、别的程序写的临时物）一律【只点名不删】——
//        §5.2 的原话是"扫描 /tmp/oj-judge/* 清空残留"，而"残留"指的是我们自己
//        留下的；对形状不符的条目动手，删掉的可能是别人正在用的东西，且不可恢复。
//     ② 递归删除【不跟随符号链接】（nftw + FTW_PHYS + unlink）。
//        若工作区里出现一个指向 /etc 的符号链接，跟随式删除会把 /etc 当成工作区
//        的子树清空。这是"递归删目录"这一类代码最经典的事故形状，而它平时测不出来。
//     ③ remove_workspace 拒绝删除 root 自身，也拒绝 root 之外的任何路径
//        （判据：父目录必须恰好是 root，且末段必须是纯数字 —— 这同时把 ① 固定住了）。
//
// 【权限用 0700 而不是 0755】
//   §1.4 的安全需求是"用户代码不可读文件系统（除自身工作目录）"。工作区里放着
//   用户提交的源码与编译产物，0700 让同机的其他本地用户连 ls 都做不到 ——
//   成本为零（不改变本用户自己的访问），收益是把"本机多用户"这条本来就不设防的
//   旁路堵上。注意它【不替代】§5.4 的 setrlimit：那是拦用户代码去读别人的文件，
//   0700 是反过来保护用户自己的代码不被别人读。
//
// 【调用时机：创建/删除是【每次提交】一次，扫残是【每次启动】一次】
//   扫残必须在 judge 调度器启动【之前】跑（§2.3 启动顺序第 6~7 步之间）。
//   若它在 worker 运行期间执行，就会把"正在编译的那个目录"连根删掉 —— 表现为
//   评测随机的 JE/编译失败，且极难复现。这个前置条件由 main.cpp 的启动顺序保证，
//   不用模块内部的锁去兜：锁只能防住"两个调用点同时进"，防不住"调用时机不对"。
//
// 【不做的清理】
//   · 不删"根目录"本身（删了也无妨，但下一次启动还得重建，而 §5.2 没要求）
//   · 不限制工作区数量或总占用：§1.4 的磁盘约束由 §5.4 的 RLIMIT_FSIZE 对
//     【单个 fork 子进程】生效（1MB），而工作区是一次性的、编译完就删，
//     稳态占用是"并发评测数 × 单次提交大小"，无需再加一层配额
//   · 不清理"编译中途被 kill -9 时留下的 .o / run"以外的东西：那个目录整体
//     就是一次提交的产物，删目录即可，逐文件挑拣只会引入"哪些算残留"的判断题
// =============================================================================
#pragma once

#include <cstddef>
#include <string>

namespace oj {
namespace judge {

// 一次提交在工作区里的全部文件路径。字段名与 §5.2 的清单一一对应。
struct WorkspacePaths {
  std::string dir;             // <root>/<submissionId>
  std::string user_source;     // user.cpp          —— 用户代码原文，由调用点写入
  std::string user_object;     // user.o            —— 阶段一产物（compiler.h 写）
  std::string tester_source;   // tester.cpp        —— 阶段二参与编译（生成器写）
  std::string solution_decl;   // solution_decl.h
  std::string tester_util;     // tester_util.h
  std::string runner_binary;   // run               —— 阶段二产物（compiler.h 写）
};

// 工作区根目录：config().judge_workdir（OJ_JUDGE_WORKDIR，默认 /tmp/oj-judge）。
// 取不到配置时退回 judge/compiler.h 的 kWorkspaceRoot —— 未加载配置就调用本模块
// 的场合只可能是单测，而单测应显式传 root（见下面几个函数的参数）。
std::string workspace_root();

// 拼出一次提交的全部路径。纯函数：不碰文件系统，因此不需要真实目录即可单测。
WorkspacePaths workspace_paths(const std::string& root, unsigned long long submission_id);

struct CreateResult {
  bool ok = false;
  std::string dir;
  std::string error;   // 人可读：期望什么、实际什么（含 errno 文本）
  // 目录本来就存在（同 id 重入，或上次没删干净）。为 true 时 ok 仍为 true ——
  // 复用是正当的，因为目录名由 submissionId 决定，同一个 id 不可能属于两次提交。
  // 但调用点应当知道：里面可能残留上一次的 .o / run（阶段一失败时会被覆盖，
  // 而"复用"这件事本身需要在日志里可见，否则出现"user.o 是旧的"无从排查）。
  bool reused = false;
};

// 创建 <root>/<submissionId>（含 root，逐级只在缺 root 时建一级）。
// 同名存在且是真目录 → ok=true, reused=true；同名存在但不是目录（含符号链接）→ 失败，
// 绝不复用：那意味着有人在这个路径上摆了一个链接，编译产物会写进别的地方。
CreateResult create_workspace(const std::string& root, unsigned long long submission_id);

// 删除一个工作区（递归）。root 参数用于安全校验，见本文件【三条不可协商的删除纪律】。
//   · 已经不存在 → 返回 true（清理动作必须是幂等的：worker 的 finally 里调用它，
//     而"上一轮已经删过"与"这轮没建过"都不该算失败）
//   · 路径形状不符 / 不在 root 之下 → 返回 false 并给出原因（绝不删除）
bool remove_workspace(const std::string& root, const std::string& dir,
                      std::string* error = nullptr);

struct SweepReport {
  std::size_t removed = 0;    // 已删除的残留目录数
  std::size_t skipped = 0;    // 形状不符、只点名不删的条目数
  std::size_t failed = 0;     // 形状符合但删除失败的条目数
  std::string notes;          // 逐条原因（跳过/失败各一行），直接进 app.log

  std::string summary() const;
};

// 启动扫残（§5.2 末句）：清掉上次进程被 kill -9 留下的工作区。
// root 不存在时返回【全零的空报告】—— 首次启动没有残留可清，不是错误，也不需要建目录：
// 真正要建目录的是 create_workspace（第一次提交时再建）。
// ⚠️ 只能在 judge 调度器启动之前调用一次（见本文件【调用时机】）。
SweepReport sweep_workspace(const std::string& root);

}  // namespace judge
}  // namespace oj