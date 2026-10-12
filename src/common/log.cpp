// =============================================================================
// CppOJ 日志模块实现 —— 契约与设计理由见 include/common/log.h 顶部注释
// 对应任务：SPEC.md §9 M0 T0.4（双文件按天切割）+ §9 M2 T2.10（judge 日志内容）
//
// 【实现要点】
//   1. 状态是一个【故意泄漏】的单例（State& state() 里的 new）：日志可能被静态对象的
//      析构函数调用，若状态是普通全局对象，析构顺序不定 → 写已析构的 ofstream 是 UB。
//      泄漏 + 显式 shutdown_logging() 把这个顺序问题彻底消掉。
//   2. 写入、切割、保留期清理、统计【全部在同一个互斥量下】完成：切割会关文件换文件，
//      若与写入并发就会出现"写进刚被关掉的流"。低频写入，不值得为它做无锁。
//   3. 切割判据是"写入时的本地日期 ≠ 该文件上次写入时的本地日期"，即【惰性切割】：
//      进程整晚没写入就不会在零点产生文件，零点后第一条日志才切。
//   4. 保留期清理只认 "<当前文件名>.<YYYY-MM-DD>" 这一种形状且日期必须合法，
//      不做 glob —— 日志目录在路线 B 下是 volume 挂载点，可能有别的东西躺在里面。
//   5. 每条日志写完立即 flush：judge worker 会被 SIGKILL（§5.4.2 两条 kill 路径），
//      那正是最需要留下现场的时刻，缓冲会把最有价值的那几行丢掉。
// =============================================================================
#include "common/log.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

#include "common/config.h"

namespace oj {
namespace common {
namespace {

// 两个通道的固定文件名。[D-34] 当前文件恒为此名，跨日才改名。
constexpr std::array<const char*, kLogChannelCount> kBaseName{{"app.log", "judge.log"}};
// 前缀里的通道标签（原样，不补齐 —— 补齐只对固定宽度的等级标签有意义）
constexpr std::array<const char*, kLogChannelCount> kChannelTag{{"app", "judge"}};
// 等级标签，宽度补齐到 5（"DEBUG"/"INFO "/"WARN "/"ERROR"）
const char* level_tag(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO ";
    case LogLevel::kWarn:  return "WARN ";
    case LogLevel::kError: return "ERROR";
  }
  return "?????";
}

// ---- 时间 ---------------------------------------------------------------------
// 时钟覆盖值：> 0 时所有格式化都用它（单测用），<= 0 用真实时钟
std::int64_t g_clock_override = 0;

std::int64_t now_epoch_seconds() {
  if (g_clock_override > 0) return g_clock_override;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int now_millis() {
  if (g_clock_override > 0) return 0;
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int>(ts.tv_nsec / 1000000);
}

std::tm local_tm(std::int64_t epoch_seconds) {
  std::time_t raw = static_cast<std::time_t>(epoch_seconds);
  std::tm out{};
  ::localtime_r(&raw, &out);
  return out;
}

std::string format_date(std::int64_t epoch_seconds) {
  const std::tm tm_value = local_tm(epoch_seconds);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm_value);
  return buf;
}

std::string format_timestamp(std::int64_t epoch_seconds, int millis) {
  const std::tm tm_value = local_tm(epoch_seconds);
  char buf[24];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm_value);
  char out[32];
  std::snprintf(out, sizeof out, "%s.%03d", buf, millis);
  return out;
}

// 天序号：Howard Hinnant 的 days_from_civil。只用 y/m/d，与时区与夏令时无关，
// 保留期比较必须用【日历天】而不是秒差 —— 跨夏令时后者会少一天或多一天。
long long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

long long date_to_index(const std::string& yyyymmdd) {
  if (yyyymmdd.size() != 10) return 0;
  if (yyyymmdd[4] != '-' || yyyymmdd[7] != '-') return 0;
  for (std::size_t i = 0; i < yyyymmdd.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (yyyymmdd[i] < '0' || yyyymmdd[i] > '9') return 0;
  }
  const int y = std::atoi(yyyymmdd.substr(0, 4).c_str());
  const unsigned m = static_cast<unsigned>(std::atoi(yyyymmdd.substr(5, 2).c_str()));
  const unsigned d = static_cast<unsigned>(std::atoi(yyyymmdd.substr(8, 2).c_str()));
  if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return days_from_civil(y, m, d);
}

long today_index() {
  const std::tm tm_value = local_tm(now_epoch_seconds());
  return days_from_civil(tm_value.tm_year + 1900,
                         static_cast<unsigned>(tm_value.tm_mon + 1),
                         static_cast<unsigned>(tm_value.tm_mday));
}

long thread_id() {
  // 每线程缓存一次 gettid：日志不是热路径，但 syscall 仍没必要每行都做
  static thread_local const long tid = static_cast<long>(::syscall(SYS_gettid));
  return tid;
}

// ---- 文本规范化 ----------------------------------------------------------------
constexpr std::size_t kTruncateNoticeReserve = 48;

void append_truncated(std::string* out, std::size_t original_bytes) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "...[已截断：原文 %zu 字节，超过单条上限]",
                original_bytes);
  out->append(buf);
}

// 按字节截断可能把一个多字节字符劈成两半，终端里表现为乱码。
// 这里把尾部残缺的那个 UTF-8 序列整个去掉 —— 日志是给人读的，
// 宁可少几个字节也不能留下半个字符。（原始内容是二进制时无法判定完整性，
// 此时不动它：原样保留比"删掉一串看不懂的东西"更利于定位问题。）
void drop_partial_utf8_tail(std::string* s) {
  if (s->empty()) return;
  // 尾部连续的续字节（10xxxxxx）数量，最多为 3
  std::size_t continuations = 0;
  std::size_t lead_pos = s->size();
  while (lead_pos > 0 &&
         (static_cast<unsigned char>((*s)[lead_pos - 1]) & 0xC0) == 0x80) {
    --lead_pos;
    if (++continuations > 3) return;  // 非法序列，不做处理
  }
  if (continuations > 0) {
    if (lead_pos == 0) return;  // 整串都是续字节：原文是二进制，不动
    --lead_pos;                 // 退回前导字节
  } else {
    --lead_pos;                 // 尾部没有续字节：候选就是最后一个字节本身
  }
  const unsigned char lead = static_cast<unsigned char>((*s)[lead_pos]);
  int needed = 0;  // 该字符除前导字节外应有的续字节数
  if ((lead & 0xE0) == 0xC0) {
    needed = 1;
  } else if ((lead & 0xF0) == 0xE0) {
    needed = 2;
  } else if ((lead & 0xF8) == 0xF0) {
    needed = 3;
  } else {
    return;  // ASCII 或非法前导字节 → 无从判断，不动
  }
  if (continuations >= static_cast<std::size_t>(needed)) return;  // 序列完整
  s->resize(lead_pos);
}

// 单行：'\n'/'\r'/'\t' 转成可见字面量，其余控制字符（含 DEL）替换成 '·'，
// UTF-8 多字节序列的字节值均 >= 0x80，原样透传（不做编码处理，避免误伤中文）
std::string escape_inline(const std::string& in, std::size_t max_bytes) {
  std::string out;
  out.reserve(std::min(in.size(), max_bytes) + kTruncateNoticeReserve);
  for (const char raw : in) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      default: break;
    }
    if (c < 0x20 || c == 0x7f) {
      out += "·";  // UTF-8 编码的 U+00B7，占 2 字节，控制台正常显示
      continue;
    }
    out += raw;
  }
  if (out.size() > max_bytes) {
    out.resize(max_bytes);
    drop_partial_utf8_tail(&out);
    append_truncated(&out, in.size());
  }
  return out;
}

// 块：保留真实换行与制表符（代码缩进就是 tab，转义掉就没法读了），
// 其余控制字符替换；总长超限时按行截断
std::string sanitize_block(const std::string& in, std::size_t max_bytes) {
  std::string out;
  out.reserve(std::min(in.size(), max_bytes) + kTruncateNoticeReserve);
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char raw = in[i];
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c == '\r') {  // CRLF/CR 统一成 LF
      if (i + 1 < in.size() && in[i + 1] == '\n') continue;
      out += '\n';
      continue;
    }
    if (c == '\n' || c == '\t') {
      out += raw;
      continue;
    }
    if (c < 0x20 || c == 0x7f) {
      out += "·";
      continue;
    }
    out += raw;
    if (out.size() > max_bytes) {
      out.resize(max_bytes);
      out.append("\n");
      drop_partial_utf8_tail(&out);
      append_truncated(&out, in.size());
      return out;
    }
  }
  return out;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : text) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else if (c != '\r') {
      current += c;
    }
  }
  lines.push_back(current);
  return lines;
}

// ---- 通道状态 ------------------------------------------------------------------
struct Channel {
  std::ofstream out;
  std::string path;
  std::string date;  // 当前文件覆盖的本地日期；与写入时日期不同即触发切割
  LogLevel min_level = LogLevel::kInfo;
  long written = 0;
  long filtered = 0;
  long rotations = 0;
  long deleted = 0;
  long failures = 0;
  bool opened = false;
  bool stderr_notice_done = false;  // 失败降级的提示每个通道只打一次
};

struct State {
  std::mutex mu;
  std::string dir;
  int retention_days = 7;
  // 唯一在【不加锁】的路径上被读的字段（log_* 先按它裁剪正文，再进锁写盘），
  // 因此用 relaxed 原子而不是裸 int：init_logging 改它时其它线程可能正在读。
  std::atomic<int> max_entry_bytes{64 * 1024};
  bool console_mirror = false;
  bool initialized = false;
  Channel channels[kLogChannelCount];
};

State& state() {
  // 故意泄漏：见文件头【实现要点】1
  static State* instance = new State();
  return *instance;
}

std::size_t channel_index(LogChannel channel) {
  const std::size_t index = static_cast<std::size_t>(channel);
  return index < kLogChannelCount ? index : 0;
}

// 无锁读单条上限（见 State::max_entry_bytes 的说明）
std::size_t entry_byte_limit() {
  return static_cast<std::size_t>(
      state().max_entry_bytes.load(std::memory_order_relaxed));
}

// ---- 保留期清理 ----------------------------------------------------------------
// 只删形如 "<base>.log.<YYYY-MM-DD>" 且日期合法、且早于 cutoff 的文件。
// 严格按文件名判定而不是 glob：路线 B 下日志目录是 volume 挂载点
// （SPEC §2.1-B），里面可能有本模块没写过的其它东西。
std::size_t sweep_retention(std::size_t index, int keep_days) {
  Channel& channel = state().channels[index];
  std::error_code ec;
  std::filesystem::directory_iterator it(state().dir, ec);
  if (ec) return 0;

  const long today = today_index();
  // 保留 keep_days 个【日期】的文件 = 今天 + 前 keep_days-1 天，
  // 因此 cutoff = 今天 - (keep_days-1)，早于它的一律删（keep_days<=0 时只留今天）
  const long cutoff = today - (keep_days > 0 ? keep_days - 1 : 0);
  const std::string prefix = std::string(kBaseName[index]) + ".";
  std::size_t removed = 0;
  for (const auto& entry_it : it) {
    const std::string name = entry_it.path().filename().string();
    if (name.size() <= prefix.size()) continue;
    if (name.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string suffix = name.substr(prefix.size());
    const long file_index = date_to_index(suffix);
    if (file_index == 0) continue;  // 后缀不是合法日期 → 不是本模块的切割文件
    if (file_index >= cutoff) continue;
    std::error_code rm_ec;
    std::filesystem::remove(entry_it.path(), rm_ec);
    if (rm_ec) continue;
    ++removed;
  }
  channel.deleted += static_cast<long>(removed);
  return removed;
}

bool open_current(Channel& channel, const std::string& path, const std::string& date,
                  bool append_mode) {
  channel.out.close();
  channel.path = path;
  channel.date = date;
  const std::ios_base::openmode mode =
      std::ios::out | (append_mode ? std::ios::app : std::ios::trunc);
  channel.out.open(path, mode);
  channel.opened = channel.out.is_open();
  return channel.opened;
}

// 惰性切割：写入时发现日期变了才切。切换失败（改名冲突、目录只读）时
// 【追加】回原文件而不是新建，代价只是这一天的内容跨了两天，不会丢数据。
void rotate_if_needed(std::size_t index) {
  Channel& channel = state().channels[index];
  const std::string today = format_date(now_epoch_seconds());
  if (channel.date == today) return;
  if (!channel.opened) return;

  channel.out.flush();
  channel.out.close();
  channel.opened = false;

  bool preserved = false;
  if (!channel.path.empty() && std::filesystem::exists(channel.path)) {
    std::string target = channel.path + "." + channel.date;
    for (int suffix = 1; suffix < 1000 && std::filesystem::exists(target); ++suffix) {
      target = channel.path + "." + channel.date + "." + std::to_string(suffix);
    }
    std::error_code ec;
    std::filesystem::rename(channel.path, target, ec);
    if (ec) {
      ++channel.failures;  // 保留原文件，追加回去
      preserved = true;
    }
    ++channel.rotations;
  }

  if (!open_current(channel, channel.path, today, preserved)) {
    ++channel.failures;
    return;
  }
  sweep_retention(index, state().retention_days);
}

// ---- 落盘 ----------------------------------------------------------------------
// 前缀已由调用方算好（含时间戳），这里只负责"写到哪去"。
void write_line(std::size_t index, const std::string& prefix, const std::string& body,
                bool mirror) {
  State& s = state();
  Channel& channel = s.channels[index];
  if (!s.initialized || !channel.opened) {
    // 未初始化或已关闭：退化为 stderr，每个通道只提示一次（见本文件【实现要点】）
    if (!channel.stderr_notice_done) {
      channel.stderr_notice_done = true;
      std::fprintf(stderr, "[oj-log] 通道 %s 尚未成功打开，日志只写到 stderr：%s\n",
                   kChannelTag[index], channel.path.c_str());
    }
    std::fputs((prefix + body + "\n").c_str(), stderr);
    return;
  }

  rotate_if_needed(index);
  if (!channel.opened) {  // 本次切割失败
    std::fputs((prefix + body + "\n").c_str(), stderr);
    return;
  }

  channel.out << prefix << body << '\n';
  // 每条即 flush：worker 被 SIGKILL 时现场不能丢（见本文件【实现要点】5）
  channel.out.flush();
  if (!channel.out) ++channel.failures;
  ++channel.written;
  if (mirror) std::fputs((prefix + body + "\n").c_str(), stderr);
}

std::string build_prefix(LogChannel channel, LogLevel level) {
  return format_timestamp(now_epoch_seconds(), now_millis()) + " " +
         level_tag(level) + " [tid=" + std::to_string(thread_id()) + "] [" +
         kChannelTag[channel_index(channel)] + "] ";
}

void entry(LogChannel channel, LogLevel level, const std::string& body) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  const std::size_t index = channel_index(channel);
  if (level < s.channels[index].min_level) {
    ++s.channels[index].filtered;
    return;
  }
  write_line(index, build_prefix(channel, level), body,
             s.console_mirror && channel == LogChannel::kApp);
}

}  // namespace

// ---- 公共接口 ------------------------------------------------------------------
const char* to_string(LogLevel level) { return level_tag(level); }
const char* to_string(LogChannel channel) { return kChannelTag[channel_index(channel)]; }

LogInitReport init_logging(const LogOptions& options) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);

  LogInitReport report;
  report.dir = options.dir;
  if (report.dir.empty()) {
    report.problems.push_back("日志目录为空串，已回退到 ./logs");
    report.dir = "./logs";
  }

  for (auto& c : s.channels) {
    if (c.opened) c.out.close();
    c = Channel();
    c.opened = false;
  }

  std::error_code ec;
  std::filesystem::create_directories(report.dir, ec);
  if (ec) {
    report.problems.push_back("无法创建日志目录 " + report.dir + "：" + ec.message());
  }

  s.dir = report.dir;
  s.retention_days = options.retention_days;
  s.max_entry_bytes.store(options.max_entry_bytes > 0 ? options.max_entry_bytes
                                                      : 64 * 1024,
                          std::memory_order_relaxed);
  s.console_mirror = options.console_mirror;
  s.initialized = false;  // 逐个通道打开，全部成功才置 true

  const std::string today = format_date(now_epoch_seconds());
  bool all_ok = true;
  for (int i = 0; i < kLogChannelCount; ++i) {
    const LogChannel channel = static_cast<LogChannel>(i);
    Channel& c = s.channels[i];
    c.min_level = channel == LogChannel::kJudge ? options.judge_min_level
                                                : options.app_min_level;
    const std::string path = s.dir + "/" + kBaseName[i];
    // init 走截断：进程重启后的当日文件重新开始，覆盖日志在磁盘上只保留一份
    if (open_current(c, path, today, /*append_mode=*/false)) {
      report.opened_files.push_back(path);
    } else {
      all_ok = false;
      report.problems.push_back("无法打开日志文件 " + path);
    }
  }

  s.initialized = all_ok;
  report.ok = all_ok;

  // 保留期清理：init 时清一次（清掉上次进程留下的超期文件），每次切割时再清
  for (int i = 0; i < kLogChannelCount; ++i) {
    report.deleted_rotated_files += sweep_retention(static_cast<std::size_t>(i),
                                                    s.retention_days);
  }
  return report;
}

LogInitReport init_logging() {
  LogOptions options;
  const Config& cfg = config();  // 未 load_config 时是内置默认值（./logs、7 天）
  options.dir = cfg.log_dir;
  options.retention_days = cfg.log_retention_days;
  return init_logging(options);
}

bool log_initialized() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.initialized;
}

void shutdown_logging() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  for (auto& c : s.channels) {
    if (c.opened) {
      c.out.flush();
      c.out.close();
      c.opened = false;
    }
  }
  s.initialized = false;
}

void log_write(LogChannel channel, LogLevel level, const std::string& message) {
  const std::string body = escape_inline(message, entry_byte_limit());
  entry(channel, level, body);
}

void log_debug(LogChannel channel, const std::string& message) {
  log_write(channel, LogLevel::kDebug, message);
}
void log_info(LogChannel channel, const std::string& message) {
  log_write(channel, LogLevel::kInfo, message);
}
void log_warn(LogChannel channel, const std::string& message) {
  log_write(channel, LogLevel::kWarn, message);
}
void log_error(LogChannel channel, const std::string& message) {
  log_write(channel, LogLevel::kError, message);
}

void log_kv(LogChannel channel, LogLevel level, const std::string& event,
            const LogFields& fields) {
  const std::size_t limit = entry_byte_limit();
  std::string body = escape_inline(event, limit);
  for (const auto& field : fields) {
    std::string value = escape_inline(field.second, limit);
    const bool needs_quote = value.find(' ') != std::string::npos ||
                             value.find('"') != std::string::npos ||
                             value.empty();
    if (needs_quote) {
      std::string quoted;
      quoted.reserve(value.size() + 2);
      quoted += '"';
      for (const char c : value) {
        if (c == '"') quoted += '\\';
        quoted += c;
      }
      quoted += '"';
      value = quoted;
    }
    body += " " + field.first + "=" + value;
    if (body.size() > limit) break;
  }
  entry(channel, level, body);
}

void log_text(LogChannel channel, LogLevel level, const std::string& text) {
  const std::size_t limit = entry_byte_limit();
  for (const std::string& raw_line : split_lines(text)) {
    entry(channel, level, escape_inline(raw_line, limit));
  }
}

void log_block(LogChannel channel, LogLevel level, const std::string& title,
               const std::string& body) {
  const std::size_t limit = entry_byte_limit();
  const std::string clean = sanitize_block(body, limit);
  const std::vector<std::string> lines = split_lines(clean);

  // 头部记的是【实际写出去】的行数与字节数（截断后仍是真实值），
  // 否则排查者会拿正文行数与头部对不上，以为块被截断了
  char header[128];
  std::snprintf(header, sizeof header, "===== %s（%zu 行 / %zu 字节）=====",
                title.c_str(), lines.size(), clean.size());
  entry(channel, level, header);
  for (const std::string& line : lines) {
    entry(channel, level, line.empty() ? std::string() : ("    " + line));
  }
  entry(channel, level, "===== " + title + " 结束 =====");
}

void flush_log(LogChannel channel) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  Channel& c = s.channels[channel_index(channel)];
  if (c.opened) c.out.flush();
}

void set_min_level(LogChannel channel, LogLevel level) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  s.channels[channel_index(channel)].min_level = level;
}

LogLevel min_level(LogChannel channel) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.channels[channel_index(channel)].min_level;
}

void set_console_mirror(bool enabled) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  s.console_mirror = enabled;
}

LogStats log_stats() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  LogStats stats;
  for (int i = 0; i < kLogChannelCount; ++i) {
    const Channel& c = s.channels[i];
    stats.written[i] = c.written;
    stats.filtered[i] = c.filtered;
    stats.rotations[i] = c.rotations;
    stats.deleted[i] = c.deleted;
    stats.failures[i] = c.failures;
  }
  return stats;
}

std::string log_path(LogChannel channel) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.channels[channel_index(channel)].path;
}

std::string log_current_date(LogChannel channel) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.channels[channel_index(channel)].date;
}

void log_test_reset() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mu);
  for (auto& c : s.channels) {
    if (c.opened) c.out.close();
    c = Channel();
  }
  s.dir.clear();
  s.retention_days = 7;
  s.max_entry_bytes.store(64 * 1024, std::memory_order_relaxed);
  s.console_mirror = false;
  s.initialized = false;
  g_clock_override = 0;
}

void log_test_set_time(std::int64_t epoch_seconds) { g_clock_override = epoch_seconds; }

std::string log_today() { return format_date(now_epoch_seconds()); }

}  // namespace common
}  // namespace oj
