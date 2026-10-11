// =============================================================================
// CppOJ 运行期配置（.env 文件 + 进程环境变量）
// 对应任务：SPEC.md §9 M0 T0.4「common/config.h（环境变量读取）」
// 规格来源：SPEC.md §6.7.3（.env 清单）/ §6.3.1（D-37 角色声明）/ §2.3（D-74 闸门）
//           / §5.4.2.1（D-62 兜底墙钟）/ §5.8（D-22 配额）/ §6.3.2（会话 7 天）
//
// 【定位】本模块是「改配置 + 重启」这套运维模式的【读取端】。SPEC 全篇只有这一条配置
//   生效途径：题库（D-30 改 JSON 后重启）、管理员名单（D-37 改 .env 后重启）都是如此，
//   没有运行期写接口。因此本模块【不提供任何 setter】，只有「加载 + 只读访问 + 诊断」。
//
// 【优先级】进程环境变量 > .env 文件 > 内置默认值。
//   进程环境优先是路线 B 的需要（compose 的 environment 直接进容器）；路线 A 下二者
//   通常一致。.env 相对【当前工作目录】查找，即必须从项目根目录启动（`make run` 保证）。
//
// 【D-37 约束 1：必须区分「变量未设置」与「设置为空串」】
//   OJ_ADMIN_USERNAMES：
//     未设置  → Config::admin_usernames_configured = false → T3.11 启动同步【完全跳过】，
//                role 列零变化（语义：「我没配置，别动我」）
//     显式空串 → configured = true 且名单为空 → 同步照常执行 → 零 admin（语义：
//                「我要收回所有权限」）
//   两者语义不同。若混为一谈，一次「.env 丢失 + 重启」就会静默把所有人降权，且日志里
//   看不出任何异常。故 API 用独立的 configured 标志表达，
//   【禁止用「名单为空」反推「未配置」】。
//
// 【量化链：闸门 4 → worker 2 → 兜底墙钟余量 2.9 倍 → AC-38 压测口径】
//   这四个数字互相绑定（SPEC §12 v1.6 末条：动一个必须重算另外三个），所以
//   load_config() 对下面两条做【硬校验】，不满足直接返回失败（绝不静默修正）：
//     ① judge_global_gate == 2 × judge_workers          （§2.3）
//        == worker 会让并发退化为 1（一次只判一题）；> worker×2 则 2 核机器上墙钟
//        放大倍数上升，逼近 ② 的兜底墙钟。
//     ② gate × time_limit_ms < time_limit_ms × fence_multiplier + kFenceSlackMs（§5.4.2.1）
//        否则兜底墙钟从「安全网」退化成「误杀器」—— 并发评测时 CPU 用量高但完全合法的
//        解法会被系统性判成 TLE。这正是 v1.4 修掉的 §5.4.1 那个 bug。
//
// 【启动契约】load_config() 必须在启动 HTTP 线程池与 judge 调度器【之前】调用一次
//   （§2.3：配置 → DB 就绪 → 建表校验 → 题库 → 角色同步 → judge → listen）。
//   加载完成后配置是只读的，config() 可被任意线程并发调用，不需要加锁。
//
// 【失败语义】load_config() 返回 false 时【不更新全局配置】（全局保持内置默认值），
//   调用方必须据此终止启动 —— 配置自相矛盾的服务跑起来比跑不起来危险得多。
// =============================================================================
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oj {
namespace common {

// 兜底墙钟的固定加量（SPEC §5.4.2.1 公式的后一项，与闸门无关，故不做成环境变量）
inline constexpr long kFenceSlackMs = 5000;
// 校验 ② 时使用的参考时限 = problems.time_limit_ms 的 DDL 默认值。
// 题库若改时限，闸门/系数需按同一不等式重算（§5.4.2.1 的表就是这么算的）。
inline constexpr long kRefTimeLimitMs = 3000;

enum class DiagLevel { kInfo, kWarn, kError };

struct Diagnostic {
  DiagLevel level = DiagLevel::kInfo;
  std::string key;      // 相关配置键；跨字段问题为空串
  std::string message;  // 人可读说明，含量化关系与修正建议
};

struct Config {
  // ---- MySQL（§6.7.3）----
  std::string mysql_host = "127.0.0.1";
  int mysql_port = 3306;
  std::string mysql_user = "oj";
  std::string mysql_password = "oj";
  std::string mysql_db = "oj";

  // ---- HTTP（§2.3 / §6.7.3）----
  std::string listen_host = "0.0.0.0";
  int listen_port = 8080;
  int http_threads = 8;  // cpp-httplib 线程池大小

  // ---- Judge（§2.3 / §5.4.2.1 / §6.7.3）----
  int judge_workers = 2;       // [D-74] 2 核实测机
  int judge_global_gate = 4;   // [D-74] 必须 == 2 × judge_workers
  int judge_user_gate = 2;     // 单用户同时评测数上限（§1.4）
  int fence_multiplier = 10;   // 兜底墙钟系数，闸门 > 10 时必须同步调高（§5.4.2.1）
  std::string judge_workdir = "/tmp/oj-judge";  // §5.1 工作区，启动时扫残

  // ---- 策略 [D-37]----
  bool allow_register = true;  // OJ_ALLOW_REGISTER=0 → 注册端点返回 1010
  // 关键：区分「未设置」与「显式空串」，见本文件顶部 D-37 约束 1
  bool admin_usernames_configured = false;
  std::vector<std::string> admin_usernames;  // 已 trim、去重、保序

  // ---- 配额 [D-22]（§5.8）----
  int rate_window_seconds = 60;         // 内存滑动窗口长度
  int rate_max_submits_per_window = 10; // 超限 → 1006
  std::size_t max_code_bytes = 64 * 1024;

  // ---- 会话（§6.3.2）----
  int session_ttl_days = 7;

  // ---- 路径与日志（§2.1-A / §5.1）----
  std::string problems_dir = "./problems";
  std::string static_dir = "./static";
  std::string log_dir = "./logs";
  int log_retention_days = 7;

  // 兜底墙钟阈值 = time_limit_ms × fence_multiplier + kFenceSlackMs（§5.4.2.1）
  long fence_wall_ms(long time_limit_ms) const {
    return time_limit_ms * fence_multiplier + kFenceSlackMs;
  }
  // 闸门下合法解法的最坏墙钟（放大倍数 = 闸门）
  long fence_budget_ms(long time_limit_ms) const {
    return time_limit_ms * judge_global_gate;
  }
};

struct LoadResult {
  bool ok = false;
  std::string env_path;
  bool env_file_loaded = false;                 // .env 文件是否读到
  std::vector<std::string> env_file_keys;      // .env 中定义了的键（按字典序）
  std::vector<Diagnostic> diagnostics;

  std::size_t warning_count() const;
  std::size_t error_count() const;
};

// 加载 .env + 进程环境变量并做交叉校验。必须在线程池启动前调用一次。
// 失败（ok=false）时不更新全局配置。
LoadResult load_config(const std::string& env_path = ".env");

bool config_loaded();
// 未加载时返回内置默认值（全部字段可安全读取），加载后为只读快照。
const Config& config();

// 仅供单元测试：清空已加载状态与全局配置
void reset_config();

// 键级原始值查询（启动日志回显 / 诊断用）
std::optional<std::string> lookup_raw(const std::string& key);
std::vector<std::string> defined_keys();  // 排序后的全部键

// 一行一条的诊断文本，直接喂给 logger
std::string format_diagnostics(const LoadResult& result);
// 生效配置回显（密码掩码），启动日志用
std::string dump_config();
// 掩码：空串显示 (空)，否则 ***（长度 N）
std::string mask_secret(const std::string& secret);

}  // namespace common
}  // namespace oj