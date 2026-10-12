// =============================================================================
// CppOJ 日志模块 —— 应用日志与 judge 日志【双文件】、按天切割、保留 N 天
// 对应任务：SPEC.md §9 M0 T0.4「log.h（双文件按天切割 logger）」+ §9 M2 T2.10
// 规格来源：SPEC.md [D-34] / §1.4 可观测 / §2.1-A（第 269 行）/ §2.1-B（第 316 行）
//           / §4.3 判题第 5 步（"按 judge 日志修正 JSON"）/ NF-5
//
// 【定位】本模块是 SPEC 里唯一一处"日志格式即接口"的地方：
//   NF-5 与 §4.3 第 5 步都把 judge 日志当作【排查入口】而非流水账 ——
//   "全绿则录入完成；否则按 judge 日志 /var/log/oj/judge.log 修正 JSON"。
//   因此下面每条格式约定都是契约，不是实现细节：
//     · 每个物理行以 `<时间戳> <等级> [tid=N] [通道] ` 开头 → 行级 grep/tail 成立
//     · 生成代码 / 编译器输出走缩进块 → 不破坏上面那条行不变量（见下【行不变量】）
//     · 两个通道各写各的文件 → 用户代码与编译产物不混进应用日志（T2.10）
//
// 【为什么是两个文件而不是一个】[D-34]
//   app.log  的读者是"运维/开发本人"：启动回显、请求与队列事件、DB 与题库同步结论。
//   judge.log 的读者是"录题或排查评测的人"：生成的 3 个 harness 文件、两条 g++ 命令、
//             子进程退出原因（SIGXCPU / SIGKILL / SIGSEGV…）。
//   两类内容的读取时机、体量、敏感度都不同：judge.log 体积大几个数量级且含用户代码，
//   合成一个文件后，想看"服务有没有起来"必须先翻过几万行评测日志。
//
// 【按天切割的实现口径】
//   当前文件恒为固定名 `<dir>/app.log` / `<dir>/judge.log`；跨过本地零点后的【第一条】
//   写入才触发切割：先把当前文件改名为 `<当前名>.<它覆盖的那一天>`，再新建空的当前文件。
//   为什么不用「app.2026-10-11.log」这种按天建文件、靠软链接指当前：
//     ① tail -F / docker 的 volume 挂载（§2.1-B 挂 `/var/log/oj/{app,judge}.log`）
//        都要求"当前文件路径恒定"，改名式切割不用软链接就能满足；
//     ② 改名是同目录内的 rename，原子且不占用第二个句柄；
//     ③ 切割点与"内容属于哪一天"严格自洽 —— 文件名里的日期就是它最后写入的那一天。
//   目标名已存在（同名日期被写过两次，例如进程在同一天重启两次触发重复切割）时，
//   追加 `.1` / `.2` 后缀，【绝不覆盖】已有历史。
//
// 【保留 N 天】OJ_LOG_RETENTION_DAYS，默认 7（§1.4）
//   在 init_logging() 与【每次切割时】各扫一次目录，删除文件名后缀日期早于
//   「今天 - N 天」的切割文件。判据严格取自文件名（必须是 `<base>.log.YYYY-MM-DD`
//   且日期合法），因此永远不会误删同目录下的其它文件 —— 不做 `*.log*` 的 glob 匹配。
//   N = 7 表示磁盘上保留 7 个日期的文件（今天 + 前 6 天），第 8 天的文件被删。
//   N ≤ 0 表示只保留当天一个文件。
//
// 【等级与默认阈值】为什么两个通道的默认阈值不同
//   app   = INFO：启动回显/请求/队列事件在 INFO 就够，DEBUG 只在排查时打开。
//   judge = DEBUG：**NF-5 明确要求 judge 日志含"生成代码与编译命令"**，而这两样是
//           T2.10 的必选项；若 judge 默认 INFO，T2.10 只能靠把生成代码记成 INFO 来
//           绕过等级过滤，日志会被"重要信息"淹没。因此 judge 通道默认全开，
//           体积换可查性是本项目明确接受的代价（单机自用，§1.2）。
//   阈值不设环境变量：本项目的配置哲学是"改 .env + 重启"，而阈值调整属于临时排查手段，
//   用 set_min_level() 即可，**新增环境变量会让 §6.7.3 的键清单与 config 单测失配**。
//
// 【行不变量】每个物理行都必须自带前缀
//   行的形状固定为 `<时间戳> <等级> [tid=N] [<通道>] <正文>`，等级补齐到 5 字符
//   （DEBUG/INFO /WARN /ERROR）便于按列 grep。
//   log_write()/log_info()… 收到含 '\n' 的消息时，转义成字面量 "\\n" 而不是真的换行，
//   因为一行没有时间戳的日志在按天切割后会失去归属（读的人无法判断它属于哪一秒、哪个线程）。
//   需要真多行时用两个专用入口：
//     log_text()  多行【文本】逐行独立加前缀 —— 启动回显 config::dump_config() 用它；
//     log_block() 多行【代码】整块缩进 4 空格，首尾各一行边界标记 —— 生成的
//                  tester.cpp / 编译器原始输出用它（T2.10 / §4.3 第 5 步）。
//
// 【绝不抛异常、绝不静默丢消息】
//   日志函数是 noexcept：调用点遍布 DAO、judge worker 回收路径与信号相关分支，
//   在那里抛异常等于把"记一条日志"放大成"终止进程"。
//   写文件失败（磁盘满/目录被删/权限变更）时降级到 stderr，并且【每个通道只提示一次】——
//   每次都提示会把 stderr 刷爆，而提示本身又会走同一条失败路径。
//   未调用 init_logging() 时同样降级到 stderr 且【不创建任何文件】：
//   这样单测与"配置都没读到"的启动早期失败都有一条确定的行为，不会意外在 CWD 落文件。
//
// 【线程】11 个并发写者：主线程 1 + HTTP 线程池 8（§2.3）+ judge worker 2 + 调度器 1。
//   写入统一走一把进程内互斥量。**不追求无锁**：写入频率是"提交量"级别
//   （§1.4 配额：单用户每 60s 最多 10 次提交），一次 write 的开销远小于一次评测；
//   而无锁日志的真正难点是切割时的文件切换顺序，收益与风险完全不成比例。
//
// 【启动契约】（§2.3 的启动顺序里，日志要早于一切会失败的东西）
//   load_config() 成功之后、HTTP 线程池与 judge 调度器启动之前调用一次
//   init_logging()；随后把 format_diagnostics() 与 dump_config() 两段回显进 app.log。
//   为什么放在 load_config 之后：log_dir / retention_days 来自配置。
//   为什么仍要求"配置读不到时也能记"：load_config 失败时进程马上要退出，
//   此时用的是内置默认值（./logs、7 天），那条 ERROR 不该只活在 stderr。
//
// 【安全边界】用户代码与编译器原始输出只进 judge 通道。
//   app.log 里出现的必须是【元信息】（submissionId / verdict / 耗时 / 队列位置），
//   不要把密码写进来 —— config::dump_config() 已对 MYSQL_PASSWORD 做掩码，
//   本模块不做第二套脱敏（第二套必然与第一套漂移）。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oj {
namespace common {

// 等级按严重度递增；比较即过滤依据（>= min_level 才落盘）
enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

// 两个通道：[D-34] 双文件。整数下标直接用于 stats()/path() 等数组接口
enum class LogChannel { kApp = 0, kJudge = 1 };
inline constexpr int kLogChannelCount = 2;

const char* to_string(LogLevel level);
const char* to_string(LogChannel channel);

struct LogOptions {
  std::string dir = "./logs";           // 路线 A ./logs，路线 B /var/log/oj
  int retention_days = 7;               // §1.4 保留 7 天
  LogLevel app_min_level = LogLevel::kInfo;
  LogLevel judge_min_level = LogLevel::kDebug;  // NF-5 要求 judge 默认含生成代码
  // 把 app 通道的每一行同时抄到 stderr。默认关：Makefile 的 run 已重定向，
  // 开发期临时前台排查时用 set_console_mirror(true) 打开。
  bool console_mirror = false;
  // 单条日志的字节上限。默认 64KB —— 与 §5.8 的 max_code_bytes 对齐：
  // "能进平台的代码都小于这个数"，因此 judge 日志里的用户代码永远不会被截断，
  // 而失控的输出（用户 cout 刷屏）会被截断而不是把磁盘写满。
  int max_entry_bytes = 64 * 1024;
};

struct LogInitReport {
  bool ok = false;                      // 两个通道是否都成功打开
  std::string dir;                      // 实际使用的目录
  std::vector<std::string> opened_files;
  std::vector<std::string> problems;    // 人可读，main 直接打到 stderr
  std::size_t deleted_rotated_files = 0;  // 保留期清理掉的旧切割文件数
};

// 用内置默认值 + config() 的 log_dir/retention_days 初始化（必须先 load_config）
LogInitReport init_logging();
LogInitReport init_logging(const LogOptions& options);

bool log_initialized();
// 关闭两个文件。幂等；不依赖静态对象析构顺序（状态是故意泄漏的单例），
// 因此既可由 main 显式调用，也可由 atexit 调用。
void shutdown_logging();

// ---- 单行 ---------------------------------------------------------------------
// 一行一行记。消息里的 '\n'/'\r' 会被转义，保证行不变量（见本文件【行不变量】）。
void log_write(LogChannel channel, LogLevel level, const std::string& message);
void log_debug(LogChannel channel, const std::string& message);
void log_info(LogChannel channel, const std::string& message);
void log_warn(LogChannel channel, const std::string& message);
void log_error(LogChannel channel, const std::string& message);

// ---- 结构化字段 ---------------------------------------------------------------
// 渲染为 `<event> key=value key="value with space"`，字段顺序 = 传入顺序（不排序，
// 调用方按"身份在前、度量在后"排布更易读）。值同样走单行转义。
using LogFields = std::vector<std::pair<std::string, std::string>>;
void log_kv(LogChannel channel, LogLevel level, const std::string& event,
            const LogFields& fields);

// ---- 多行 ---------------------------------------------------------------------
// 逐行独立加前缀。启动回显（config::dump_config / format_diagnostics）用它，
// 空行保留（前缀单独成行，便于人读时的段落分隔）。
void log_text(LogChannel channel, LogLevel level, const std::string& text);
// 首尾加边界标记，中间整块缩进 4 空格，不加时间戳（生成代码、编译器原始输出用它）。
void log_block(LogChannel channel, LogLevel level, const std::string& title,
               const std::string& body);

void flush_log(LogChannel channel);

// ---- 运行时调整（不改配置、不需重启）-----------------------------------------
void set_min_level(LogChannel channel, LogLevel level);
LogLevel min_level(LogChannel channel);
void set_console_mirror(bool enabled);

struct LogStats {
  long written[kLogChannelCount] = {0, 0};     // 实际落盘的条目数
  long filtered[kLogChannelCount] = {0, 0};    // 被等级阈值挡下
  long rotations[kLogChannelCount] = {0, 0};   // 改名切割次数
  long deleted[kLogChannelCount] = {0, 0};     // 保留期删除的切割文件数
  long failures[kLogChannelCount] = {0, 0};    // 写入失败次数（已降级 stderr）
};
LogStats log_stats();
std::string log_path(LogChannel channel);
// 当前文件覆盖的本地日期 YYYY-MM-DD（未初始化返回空串）
std::string log_current_date(LogChannel channel);

// ---- 单测专用（与 config::reset_config() 同一性质）----------------------------
void log_test_reset();
// <= 0 恢复真实时钟；用于在不真的等一天的情况下验证切割与保留期清理
void log_test_set_time(std::int64_t epoch_seconds);
// 当前（可能被覆盖的）本地日期 YYYY-MM-DD
std::string log_today();

}  // namespace common
}  // namespace oj
