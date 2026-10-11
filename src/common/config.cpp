// =============================================================================
// CppOJ 配置模块实现 —— 契约与设计理由见 include/common/config.h 顶部注释
// 对应任务：SPEC.md §9 M0 T0.4
//
// 【实现要点】
//   1. 三层来源合并：内置默认 → .env 文件 → 进程环境（后者覆盖前者）
//   2. 「未设置」与「空串」全程用 optional / map::find 区分，【绝不用「值是否为空」判断
//      是否配置过】（D-37 约束 1）
//   3. 类型/范围不合法的单个键：WARN + 回落默认值（默认值是已实测的取值，比带着
//      半截坏值启动安全）；跨字段矛盾的：ERROR + 拒绝启动（见头文件量化链 ①②）
//   4. 未知键（拼写错误，如 OJ_ADMIN_USERNAME）WARN 点名 —— 静默忽略是配置类 bug 的
//      主要来源，AC-18/AC-19 都依赖「配置真的被读到了」
// =============================================================================
#include "common/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

extern char** environ;

namespace oj {
namespace common {
namespace {

// 本模块认识的全部配置键。用于：未知键检测（拼写错误防护）+ dump 回显顺序。
const std::vector<std::string>& known_keys() {
  static const std::vector<std::string> keys = {
      "MYSQL_HOST",   "MYSQL_PORT",   "MYSQL_USER",   "MYSQL_PASSWORD",
      "MYSQL_DB",     "OJ_LISTEN",    "OJ_HTTP_THREADS",
      "OJ_JUDGE_WORKERS",   "OJ_JUDGE_GATE",      "OJ_JUDGE_USER_GATE",
      "OJ_FENCE_MULTIPLIER", "OJ_JUDGE_WORKDIR", "OJ_PROBLEMS_DIR",
      "OJ_STATIC_DIR", "OJ_LOG_DIR",  "OJ_LOG_RETENTION_DAYS",
      "OJ_SESSION_TTL_DAYS", "OJ_MAX_CODE_BYTES", "OJ_RATE_WINDOW_SECONDS",
      "OJ_RATE_MAX_SUBMITS",  "OJ_ALLOW_REGISTER", "OJ_ADMIN_USERNAMES",
  };
  return keys;
}

bool is_known_key(const std::string& key) {
  const auto& keys = known_keys();
  return std::find(keys.begin(), keys.end(), key) != keys.end();
}

std::string trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// 加载过程中的可变上下文
struct Ctx {
  std::map<std::string, std::string> kv;  // 合并后的最终值（文件 + 进程环境）
  std::vector<std::string> file_keys;    // .env 中定义的键
  LoadResult* res = nullptr;
};

void diag(Ctx& c, DiagLevel level, std::string key, std::string message) {
  c.res->diagnostics.push_back(Diagnostic{level, std::move(key), std::move(message)});
}

// 解析单行 .env 的值部分：支持引号包裹、`export ` 前缀、行内注释。
// 返回 false 表示该行格式有问题（已记 WARN）。
bool parse_env_value(const std::string& raw_value, std::string& out) {
  std::string v = raw_value;

  if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
    const char quote = v.front();
    std::size_t i = 1;
    std::string inner;
    while (i < v.size() && v[i] != quote) {
      // 仅支持 \" 形式的转义，避免过度实现
      if (v[i] == '\\' && i + 1 < v.size() &&
          (v[i + 1] == '"' || v[i + 1] == '\'' || v[i + 1] == '\\')) {
        inner.push_back(v[i + 1]);
        i += 2;
        continue;
      }
      inner.push_back(v[i]);
      ++i;
    }
    if (i >= v.size()) return false;  // 引号未闭合
    out = inner;
    // 引号后只允许空白或注释
    std::string tail = trim(v.substr(i + 1));
    if (!tail.empty() && tail.front() != '#') return false;
    return true;
  }

  // 行内注释：仅在【空白之后】的 # 才算注释，否则 # 会吞掉密码里的合法字符
  std::size_t cut = v.size();
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '#' && i > 0 && std::isspace(static_cast<unsigned char>(v[i - 1]))) {
      cut = i;
      break;
    }
  }
  out = trim(v.substr(0, cut));
  return true;
}

void read_env_file(const std::string& path, Ctx& c) {
  std::ifstream in(path);
  if (!in) {
    // 缺 .env 不是致命错误：内置默认值即可跑起来（路线 B 的 compose environment
    // 也会绕过本文件）。但必须 WARN —— 它同时是 D-37 里「名单未设置」的常见成因。
    diag(c, DiagLevel::kWarn, "", "未找到配置文件 " + path +
                                     "，本次全部使用内置默认值；若期望的 .env 生效请确认"
                                     "启动时的工作目录是项目根目录");
    return;
  }
  c.res->env_file_loaded = true;

  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::string s = trim(line);
    if (s.empty() || s[0] == '#') continue;

    if (s.rfind("export ", 0) == 0) s = trim(s.substr(7));

    const std::size_t eq = s.find('=');
    if (eq == std::string::npos) {
      diag(c, DiagLevel::kWarn, "", path + ":" + std::to_string(lineno) +
                                       " 行缺少 '='，已忽略");
      continue;
    }

    std::string key = trim(s.substr(0, eq));
    if (key.empty()) {
      diag(c, DiagLevel::kWarn, "", path + ":" + std::to_string(lineno) +
                                       " 行键名为空，已忽略");
      continue;
    }

    std::string value;
    if (!parse_env_value(s.substr(eq + 1), value)) {
      diag(c, DiagLevel::kWarn, key, path + ":" + std::to_string(lineno) + " 行值格式异常"
                                    "（引号未闭合或引号后有多余内容），已按原样取用");
      value = trim(s.substr(eq + 1));
    }

    if (c.kv.count(key) != 0) {
      diag(c, DiagLevel::kWarn, key, path + " 中重复定义，以最后一次为准");
    }
    c.kv[key] = value;
    c.file_keys.push_back(key);
  }
}

void overlay_process_env(Ctx& c) {
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    const std::string entry(*e);
    const std::size_t eq = entry.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = entry.substr(0, eq);
    if (key.empty()) continue;

    auto it = c.kv.find(key);
    if (it != c.kv.end()) {
      // .env 里写了但没 export，进程环境才是最终生效值（路线 B 的注入方式）
      if (it->second != entry.substr(eq + 1)) {
        diag(c, DiagLevel::kInfo, key, "进程环境变量覆盖了 .env 中的取值");
      }
      it->second = entry.substr(eq + 1);
    } else if (is_known_key(key) || key.rfind("OJ_", 0) == 0) {
      // .env 缺失时的纯环境变量部署（路线 B 的 compose environment），或 .env 里
      // 漏写、只靠环境注入的键。只吸收已知键与 OJ_* 前缀，避免把 MYSQL_HOME 之类
      // 系统变量塞进配置表。
      c.kv[key] = entry.substr(eq + 1);
    }
  }
}

// ---- 键级读取 ---------------------------------------------------------------

// 【D-37 约束 1 的落点】未设置返回 nullopt，显式空串返回"有值的空串" —— 两者必须
// 在类型上就分得开，下游不得用「值是否为空」代替「是否配置过」。
std::optional<std::string> raw_of(Ctx& c, const std::string& key) {
  auto it = c.kv.find(key);
  if (it == c.kv.end()) return std::nullopt;
  return it->second;
}

std::string get_str(Ctx& c, const std::string& key, const std::string& fallback) {
  auto v = raw_of(c, key);
  return v.has_value() ? *v : fallback;
}

bool parse_int_strict(const std::string& s, long long& out) {
  if (s.empty()) return false;
  std::size_t i = 0;
  if (s[0] == '+' || s[0] == '-') i = 1;
  if (i >= s.size()) return false;
  for (std::size_t j = i; j < s.size(); ++j) {
    if (!std::isdigit(static_cast<unsigned char>(s[j]))) return false;
  }
  try {
    std::size_t pos = 0;
    out = std::stoll(s, &pos);
    return pos == s.size();
  } catch (...) {
    return false;
  }
}

int get_int(Ctx& c, const std::string& key, int fallback, long long lo,
            long long hi) {
  auto v = raw_of(c, key);
  if (!v.has_value()) return fallback;

  long long n = 0;
  if (!parse_int_strict(trim(*v), n)) {
    diag(c, DiagLevel::kWarn, key,
         "取值 \"" + *v + "\" 不是整数，已回落到默认值 " + std::to_string(fallback));
    return fallback;
  }
  if (n < lo || n > hi) {
    diag(c, DiagLevel::kWarn, key,
         "取值 " + std::to_string(n) + " 超出允许范围 [" + std::to_string(lo) +
             ", " + std::to_string(hi) + "]，已回落到默认值 " + std::to_string(fallback));
    return fallback;
  }
  return static_cast<int>(n);
}

bool get_bool(Ctx& c, const std::string& key, bool fallback) {
  auto v = raw_of(c, key);
  if (!v.has_value()) return fallback;
  const std::string s = to_lower(trim(*v));
  if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "开") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off" || s == "关") return false;
  diag(c, DiagLevel::kWarn, key,
       "取值 \"" + *v +
           "\" 不是布尔值（可用 1/0、true/false、yes/no、on/off），已回落到默认值 " +
           (fallback ? "1" : "0"));
  return fallback;
}

// 逗号分隔列表：trim → 去空 → 去重（保序）
std::vector<std::string> split_list(Ctx& c, const std::string& key,
                                    const std::string& s) {
  std::vector<std::string> out;
  std::string item;
  std::istringstream in(s);
  while (std::getline(in, item, ',')) {
    const std::string t = trim(item);
    if (t.empty()) continue;
    if (std::find(out.begin(), out.end(), t) != out.end()) {
      diag(c, DiagLevel::kWarn, key, "名单中 \"" + t + "\" 重复出现，已去重");
      continue;
    }
    out.push_back(t);
  }
  return out;
}

// ---- 组装 -------------------------------------------------------------------

Config build(Ctx& c) {
  Config cfg;

  cfg.mysql_host = get_str(c, "MYSQL_HOST", cfg.mysql_host);
  cfg.mysql_port = get_int(c, "MYSQL_PORT", cfg.mysql_port, 1, 65535);
  cfg.mysql_user = get_str(c, "MYSQL_USER", cfg.mysql_user);
  cfg.mysql_password = get_str(c, "MYSQL_PASSWORD", cfg.mysql_password);
  cfg.mysql_db = get_str(c, "MYSQL_DB", cfg.mysql_db);

  // OJ_LISTEN 形如 "0.0.0.0:8080"；也容忍裸端口 "8080" 与 IPv6 字面量 "[::]:8080"
  auto listen = raw_of(c, "OJ_LISTEN");
  if (listen.has_value()) {
    const std::string s = trim(*listen);
    if (!s.empty()) {
      std::string host = s;
      std::string port;
      if (s.front() == '[') {  // [::]:8080
        const std::size_t rb = s.find(']');
        if (rb != std::string::npos && rb + 2 <= s.size() && s[rb + 1] == ':') {
          host = s.substr(1, rb - 1);
          port = s.substr(rb + 2);
        }
      } else {
        const std::size_t colon = s.rfind(':');
        if (colon != std::string::npos && s.find(':') == colon) {  // 仅一个冒号
          host = trim(s.substr(0, colon));
          port = trim(s.substr(colon + 1));
        } else if (colon == std::string::npos) {
          host.clear();
          port = s;
        }
      }
      long long p = 0;
      if (port.empty()) {
        diag(c, DiagLevel::kWarn, "OJ_LISTEN",
             "未能解析出端口，已回落到默认 " + cfg.listen_host + ":" +
                 std::to_string(cfg.listen_port));
      } else if (!parse_int_strict(port, p) || p < 1 || p > 65535) {
        diag(c, DiagLevel::kWarn, "OJ_LISTEN",
             "端口 \"" + port + "\" 非法，已回落到默认 " + std::to_string(cfg.listen_port));
      } else {
        cfg.listen_port = static_cast<int>(p);
        if (!host.empty()) cfg.listen_host = host;
      }
    }
  }

  cfg.http_threads = get_int(c, "OJ_HTTP_THREADS", cfg.http_threads, 1, 256);

  cfg.judge_workers = get_int(c, "OJ_JUDGE_WORKERS", cfg.judge_workers, 1, 256);
  cfg.judge_global_gate = get_int(c, "OJ_JUDGE_GATE", cfg.judge_global_gate, 1, 1024);
  cfg.judge_user_gate = get_int(c, "OJ_JUDGE_USER_GATE", cfg.judge_user_gate, 1, 1024);
  cfg.fence_multiplier = get_int(c, "OJ_FENCE_MULTIPLIER", cfg.fence_multiplier, 1, 1000);
  cfg.judge_workdir = get_str(c, "OJ_JUDGE_WORKDIR", cfg.judge_workdir);

  cfg.allow_register = get_bool(c, "OJ_ALLOW_REGISTER", cfg.allow_register);

  // D-37 约束 1：configured 只看「键是否存在」，与值是否为空串无关
  auto admins = raw_of(c, "OJ_ADMIN_USERNAMES");
  if (admins.has_value()) {
    cfg.admin_usernames_configured = true;
    cfg.admin_usernames = split_list(c, "OJ_ADMIN_USERNAMES", *admins);
    for (const std::string& name : cfg.admin_usernames) {
      if (name.size() < 3 || name.size() > 32) {
        diag(c, DiagLevel::kWarn, "OJ_ADMIN_USERNAMES",
             "用户名 \"" + name + "\" 长度 " + std::to_string(name.size()) +
                 " 不在 3–32 内，它不会匹配到任何 users 行（同步仍会执行）");
      }
    }
    if (cfg.admin_usernames.empty()) {
      diag(c, DiagLevel::kWarn, "OJ_ADMIN_USERNAMES",
           "已设置但名单为空 → 启动同步将把所有管理员【降为普通用户】（显式空串的语义）");
    }
  }

  cfg.rate_window_seconds =
      get_int(c, "OJ_RATE_WINDOW_SECONDS", cfg.rate_window_seconds, 1, 86400);
  cfg.rate_max_submits_per_window =
      get_int(c, "OJ_RATE_MAX_SUBMITS", cfg.rate_max_submits_per_window, 1, 100000);
  cfg.max_code_bytes = static_cast<std::size_t>(
      get_int(c, "OJ_MAX_CODE_BYTES", static_cast<int>(cfg.max_code_bytes), 1024,
              64 * 1024 * 1024));

  cfg.session_ttl_days = get_int(c, "OJ_SESSION_TTL_DAYS", cfg.session_ttl_days, 1, 3650);

  cfg.problems_dir = get_str(c, "OJ_PROBLEMS_DIR", cfg.problems_dir);
  cfg.static_dir = get_str(c, "OJ_STATIC_DIR", cfg.static_dir);
  cfg.log_dir = get_str(c, "OJ_LOG_DIR", cfg.log_dir);
  cfg.log_retention_days =
      get_int(c, "OJ_LOG_RETENTION_DAYS", cfg.log_retention_days, 1, 3650);

  return cfg;
}

// ---- 交叉校验（对应头文件「量化链」①②）--------------------------------------

void cross_validate(const Config& cfg, Ctx& c) {
  // 未知键检测：拼错的键会被静默忽略 → 必须点名。
  // 口径：OJ_ 前缀来自任意来源都点名；MYSQL_ 前缀只对 .env 里写的点名 ——
  // 宿主机/镜像里常有 MYSQL_HOME、MYSQL_TCP_PORT 之类与本项目无关的系统变量，
  // 对它们报警只会制造噪音，让人学会无视 WARN。
  const std::set<std::string> from_file(c.file_keys.begin(), c.file_keys.end());
  for (const auto& kv : c.kv) {
    const bool ours = kv.first.rfind("OJ_", 0) == 0 ||
                      (kv.first.rfind("MYSQL_", 0) == 0 && from_file.count(kv.first) != 0);
    if (ours && !is_known_key(kv.first)) {
      diag(c, DiagLevel::kWarn, kv.first,
           "未知配置项，不会被读取（检查是否拼写错误，如 OJ_ADMIN_USERNAMES）");
    }
  }

  // ① 闸门 = 2 × worker（SPEC §2.3）
  if (cfg.judge_global_gate != 2 * cfg.judge_workers) {
    std::ostringstream msg;
    msg << "OJ_JUDGE_GATE(" << cfg.judge_global_gate << ") 必须等于 2 × OJ_JUDGE_WORKERS("
        << cfg.judge_workers << ") = " << 2 * cfg.judge_workers
        << "（SPEC §2.3）。等于 worker 数会让并发退化为 1；大于 2× 则 2 核机器上墙钟"
           "放大倍数上升，逼近下面的兜底墙钟约束";
    diag(c, DiagLevel::kError, "OJ_JUDGE_GATE", msg.str());
  }

  // 单用户闸门不可能大于全局闸门（否则单用户就能吃满全部并发额度）
  if (cfg.judge_user_gate > cfg.judge_global_gate) {
    std::ostringstream msg;
    msg << "OJ_JUDGE_USER_GATE(" << cfg.judge_user_gate
        << ") 不得大于 OJ_JUDGE_GATE(" << cfg.judge_global_gate << ")";
    diag(c, DiagLevel::kError, "OJ_JUDGE_USER_GATE", msg.str());
  }

  // ② 闸门 × 时限 < 兜底墙钟（SPEC §5.4.2.1），否则合法解法会被系统性误判 TLE
  const long budget = cfg.fence_budget_ms(kRefTimeLimitMs);
  const long fence = cfg.fence_wall_ms(kRefTimeLimitMs);
  if (budget >= fence) {
    // 建议的最小系数：使 gate × tl < tl × m + slack 成立的最小整数 m
    long long need = (cfg.judge_global_gate * kRefTimeLimitMs - kFenceSlackMs) /
                         kRefTimeLimitMs +
                     1;
    if (need < 1) need = 1;
    while (!(cfg.judge_global_gate * kRefTimeLimitMs <
             kRefTimeLimitMs * need + kFenceSlackMs)) {
      ++need;
    }
    std::ostringstream msg;
    // 两个可执行的修法，各自给出确切的目标值：
    //   甲：闸门降到 max_gate 以内； 乙：系数提到 need 以上
    const long long max_gate =
        (kRefTimeLimitMs * cfg.fence_multiplier + kFenceSlackMs - 1) / kRefTimeLimitMs;
    msg << "兜底墙钟与闸门矛盾（SPEC §5.4.2.1）：闸门 " << cfg.judge_global_gate
        << " × 时限 " << kRefTimeLimitMs << "ms = " << budget << "ms ≥ 兜底阈值 "
        << kRefTimeLimitMs << "×" << cfg.fence_multiplier << "+" << kFenceSlackMs << " = "
        << fence << "ms。此时兜底墙钟会从安全网退化成误杀器 —— 并发评测时 CPU 用量高但"
                   "完全合法的解法会被系统性判成 TLE。修法：把 OJ_FENCE_MULTIPLIER 调到 ≥ "
        << need << "（当前 " << cfg.fence_multiplier
        << "），或把 OJ_JUDGE_GATE 降到 ≤ " << max_gate << "（当前 "
        << cfg.judge_global_gate << "，同时 OJ_JUDGE_WORKERS ≤ " << max_gate / 2
        << " 以维持闸门 = 2 × workers）";
    diag(c, DiagLevel::kError, "OJ_FENCE_MULTIPLIER", msg.str());
  }
}

// ---- 全局状态（加载后只读）-------------------------------------------------

Config g_config;
bool g_loaded = false;
std::map<std::string, std::string> g_kv;

const char* level_name(DiagLevel l) {
  switch (l) {
    case DiagLevel::kInfo:
      return "INFO ";
    case DiagLevel::kWarn:
      return "WARN ";
    case DiagLevel::kError:
      return "ERROR";
  }
  return "?????";
}

}  // namespace

std::size_t LoadResult::warning_count() const {
  std::size_t n = 0;
  for (const auto& d : diagnostics) {
    if (d.level == DiagLevel::kWarn) ++n;
  }
  return n;
}

std::size_t LoadResult::error_count() const {
  std::size_t n = 0;
  for (const auto& d : diagnostics) {
    if (d.level == DiagLevel::kError) ++n;
  }
  return n;
}

LoadResult load_config(const std::string& env_path) {
  LoadResult res;
  res.env_path = env_path;

  Ctx c;
  c.res = &res;
  if (!env_path.empty()) read_env_file(env_path, c);
  overlay_process_env(c);

  const Config cfg = build(c);
  cross_validate(cfg, c);

  res.ok = res.error_count() == 0;
  if (!res.ok) return res;  // 不发布半份配置：调用方必须终止启动

  std::sort(c.file_keys.begin(), c.file_keys.end());
  c.file_keys.erase(std::unique(c.file_keys.begin(), c.file_keys.end()),
                    c.file_keys.end());
  res.env_file_keys = std::move(c.file_keys);

  g_config = cfg;
  g_kv = std::move(c.kv);
  g_loaded = true;
  return res;
}

bool config_loaded() { return g_loaded; }

const Config& config() { return g_config; }

void reset_config() {
  g_config = Config();
  g_loaded = false;
  g_kv.clear();
}

std::optional<std::string> lookup_raw(const std::string& key) {
  auto it = g_kv.find(key);
  if (it == g_kv.end()) return std::nullopt;
  return it->second;
}

std::vector<std::string> defined_keys() {
  std::vector<std::string> keys;
  keys.reserve(g_kv.size());
  for (const auto& kv : g_kv) keys.push_back(kv.first);
  return keys;  // std::map 已按字典序
}

std::string format_diagnostics(const LoadResult& result) {
  std::ostringstream out;
  out << "配置来源 " << result.env_path
      << (result.env_file_loaded ? "（已读取，定义 " +
                                       std::to_string(result.env_file_keys.size()) +
                                       " 个键）"
                                 : "（未读取）")
      << "；诊断 " << result.diagnostics.size() << " 条（ERROR " << result.error_count()
      << " / WARN " << result.warning_count() << "）";
  for (const auto& d : result.diagnostics) {
    out << "\n  [" << level_name(d.level) << "] "
        << (d.key.empty() ? "-" : d.key) << ": " << d.message;
  }
  return out.str();
}

std::string mask_secret(const std::string& secret) {
  if (secret.empty()) return "(空)";
  return "***（长度 " + std::to_string(secret.size()) + "）";
}

std::string dump_config() {
  const Config& c = g_config;
  // 余量倍数按一位小数输出（35000/12000 = 2.9×，与 SPEC §5.4.2.1 的表格同口径）
  const long budget = c.fence_budget_ms(kRefTimeLimitMs);
  const long fence = c.fence_wall_ms(kRefTimeLimitMs);
  const long margin_x10 = budget > 0 ? fence * 10 / budget : 0;

  std::ostringstream out;
  out << "生效配置"
      << (g_loaded ? "" : "（尚未调用 load_config，以下为内置默认值）") << ":\n"
      << "  MySQL      : " << c.mysql_user << "@" << c.mysql_host << ":"
      << c.mysql_port << "/" << c.mysql_db << "  密码 " << mask_secret(c.mysql_password)
      << "\n"
      << "  HTTP       : " << c.listen_host << ":" << c.listen_port << "，线程池 "
      << c.http_threads << "\n"
      << "  Judge      : workers=" << c.judge_workers << " 全局闸门="
      << c.judge_global_gate << " 单用户闸门=" << c.judge_user_gate << "，工作区 "
      << c.judge_workdir << "\n"
      << "  兜底墙钟   : 时限×" << c.fence_multiplier << "+" << kFenceSlackMs
      << "ms → 闸门 " << c.judge_global_gate << " 下最坏墙钟 " << budget << "ms < 阈值 "
      << fence << "ms（余量 " << margin_x10 / 10 << "." << margin_x10 % 10
      << "×，以参考时限 " << kRefTimeLimitMs << "ms 校验）\n"
      << "  注册       : " << (c.allow_register ? "开放" : "已关闭（1010）") << "\n"
      << "  管理员名单 : ";
  if (!c.admin_usernames_configured) {
    out << "【变量未设置】→ 启动同步完全跳过，role 列零变化";
  } else if (c.admin_usernames.empty()) {
    out << "【显式空串】→ 启动同步执行，结果为零 admin（全员降权）";
  } else {
    out << "已配置 " << c.admin_usernames.size() << " 人：";
    for (std::size_t i = 0; i < c.admin_usernames.size(); ++i) {
      out << (i ? ", " : "") << c.admin_usernames[i];
    }
    out << "（其余用户将被降为 user；不存在的名字会在同步时 WARN 点名）";
  }
  out << "\n"
      << "  配额       : 单用户每 " << c.rate_window_seconds << "s 最多 "
      << c.rate_max_submits_per_window << " 次提交；代码上限 " << c.max_code_bytes
      << " 字节\n"
      << "  会话       : 有效期 " << c.session_ttl_days << " 天\n"
      << "  路径       : 题库 " << c.problems_dir << "，静态 " << c.static_dir
      << "，日志 " << c.log_dir << "（保留 " << c.log_retention_days << " 天）";
  return out.str();
}

}  // namespace common
}  // namespace oj