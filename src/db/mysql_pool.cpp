// =============================================================================
// CppOJ MySQL 连接池实现 —— 契约与设计理由见 include/db/mysql_pool.h 顶部注释
// 对应任务：SPEC.md §9 M3 T3.1
//
// 【实现要点】
//   1. 锁只有两层，顺序恒为 db_mutex() → mu_（mu_ 永远是内层）。持锁期间除了重连
//      不做别的阻塞；重连发生在【已经拿到 db_mutex_ 之后】，此时全库没有别的语句在跑，
//      换来的是"重连期间不可能存在未提交事务"这个强不变式。
//   2. 结果集整批取回内存（见头文件【为什么必须整批取回】），取完立刻
//      mysql_stmt_free_result —— 连接在任何时刻都是"无游标、无未读结果"的状态。
//   3. 语句失败一律【不重试】，唯一的例外是服务端句柄失效（1243/1615）：那时服务端
//      【没有执行过】该语句，重备后重试安全。见头文件【为什么不自动重试失败的语句】。
//   4. 统计量分两处放：连接级用 atomic（由租约持有者写，无需再加锁），
//      池级用 mu_ 保护（acquire/give_back 时写）。Pool::stats() 在 mu_ 下把两者相加，
//      因此"HTTP 线程跑语句时主线程读 stats"不存在数据竞争。
//   5. 单例用"故意泄漏"的函数内 static new（与 log.cpp 同一处理）：连接池与全局
//      互斥量的生命周期由 init()/shutdown() 显式控制，不依赖静态析构顺序。
// =============================================================================
#include "db/mysql_pool.h"

#include <mysql/mysql.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/log.h"

namespace oj {
namespace db {
namespace {

using Clock = std::chrono::steady_clock;

// ---- MySQL 错误码（本模块只关心这几类，其余一律如实上抛）------------------------
constexpr unsigned int kErUnknownStmtHandler = 1243;  // DDL 变更后旧句柄失效（实测）
constexpr unsigned int kErNeedReprepare = 1615;       // 同上，另一条路径
constexpr unsigned int kCrConnectionError = 2003;     // 连不上
constexpr unsigned int kCrServerGone = 2006;          // 服务端走了
constexpr unsigned int kCrServerLost = 2013;          // 查询过程中断连
constexpr unsigned int kCrServerLostExtended = 2055;  // 8.0 的对应码

// 连接级错误：发生后把连接标记为"可疑"，下次取用直接重连。
// 为什么不在语句失败处立刻重连：此时连接上可能还挂着一个未提交事务
// （T2.11 的三段写中途断连），重连等于替服务端隐式回滚，而调用方并不知情 ——
// 于是"提交失败"被伪装成"提交成功但没有数据"。
bool is_connection_lost(unsigned int code) {
  return code == kCrConnectionError || code == kCrServerGone ||
         code == kCrServerLost || code == kCrServerLostExtended;
}

// 表结构变化导致的服务端句柄失效：服务端【没有执行】这条语句，重备后重试安全。
bool is_stmt_stale(unsigned int code) {
  return code == kErUnknownStmtHandler || code == kErNeedReprepare;
}

std::string describe_target(const PoolOptions& o) {
  return o.host + ":" + std::to_string(o.port) + "/" + o.database + "（user=" +
         (o.user.empty() ? std::string("(空)") : o.user) + "）";
}

DbError server_error(unsigned int code, const char* sqlstate, const char* message,
                     const std::string& sql) {
  DbError e;
  e.code = code;
  e.sqlstate = (sqlstate != nullptr) ? sqlstate : "";
  e.message = (message != nullptr) ? message : "";
  e.sql = sql;
  return e;
}

// ---- DDL 脚本切分（exec_script 用）-----------------------------------------------
// 只需正确处理三件事：注释、字符串/标识符里的分号、结尾分号。
// 切错的后果不是"少切一点"，而是 §3.2 的 DDL 被拆成两条半截 SQL 而建表失败 ——
// 因此对"未闭合的引号/块注释"显式报错，而不是猜。
std::string trim_blank(const std::string& s) {
  auto space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && space(s[b])) ++b;
  while (e > b && space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// 匿名命名空间内实现的切分器，供 split_statements() 与 Session::exec_script() 共用。
// 刻意只写一份：启动时的建表核对也要按同一套规则数语句，两份实现的漂移是静默的。
namespace {
bool split_script_impl(const std::string& script, std::vector<std::string>* out,
                       std::string* error) {
  std::string current;
  enum class State {
    kNormal, kLineComment, kBlockComment, kSingleQuote, kDoubleQuote, kBacktick
  };
  State state = State::kNormal;
  for (std::size_t i = 0; i < script.size(); ++i) {
    const char ch = script[i];
    const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';
    switch (state) {
      case State::kNormal:
        if (ch == '-' && next == '-') {
          state = State::kLineComment;
          ++i;
        } else if (ch == '/' && next == '*') {
          state = State::kBlockComment;
          ++i;
        } else if (ch == '\'') {
          state = State::kSingleQuote;
          current.push_back(ch);
        } else if (ch == '"') {
          state = State::kDoubleQuote;
          current.push_back(ch);
        } else if (ch == '`') {
          state = State::kBacktick;
          current.push_back(ch);
        } else if (ch == ';') {
          const std::string trimmed = trim_blank(current);
          if (!trimmed.empty()) out->push_back(trimmed);
          current.clear();
        } else {
          current.push_back(ch);
        }
        break;
      case State::kLineComment:
        if (ch == '\n') {
          state = State::kNormal;
          current.push_back('\n');
        }
        break;
      case State::kBlockComment:
        if (ch == '*' && next == '/') {
          state = State::kNormal;
          ++i;
        } else if (ch == '\n') {
          current.push_back('\n');
        }
        break;
      case State::kSingleQuote:
      case State::kDoubleQuote:
      case State::kBacktick:
        current.push_back(ch);
        // 反斜杠转义：Ubuntu 的 MySQL 默认 sql_mode 里没有 NO_BACKSLASH_ESCAPES，
        // 故 \' 是字面量单引号，转义规则必须跟着走，否则 'it\'s' 会被提前闭合。
        if (ch == '\\' && next != '\0') {
          current.push_back(next);
          ++i;
        } else if ((state == State::kSingleQuote && ch == '\'') ||
                   (state == State::kDoubleQuote && ch == '"') ||
                   (state == State::kBacktick && ch == '`')) {
          state = State::kNormal;
        }
        break;
    }
  }
  if (state == State::kBlockComment) {
    if (error != nullptr) *error = "脚本以未闭合的 /* 块注释结尾";
    return false;
  }
  if (state == State::kSingleQuote || state == State::kDoubleQuote ||
      state == State::kBacktick) {
    if (error != nullptr) *error = "脚本以未闭合的引号结尾";
    return false;
  }
  const std::string trimmed = trim_blank(current);
  if (!trimmed.empty()) out->push_back(trimmed);
  return true;
}
}  // namespace

// ---- 建立一条物理连接 -------------------------------------------------------------
// options → connect → 会话级设置 → 三项校验（sql_mode / 字符集 / 时区）。
// 任何一步失败都返回 false 并把原因写进 error（含 host:port/db，密码不出现在这里）。
bool open_connection(const PoolOptions& options, MYSQL** out, std::string* error) {
  *out = nullptr;
  MYSQL* m = mysql_init(nullptr);
  if (m == nullptr) {
    *error = "mysql_init 返回空（内存不足？）";
    return false;
  }

  // 超时必须在 connect 之前设：connect 阶段不认 read/write timeout。
  unsigned int connect_timeout = options.connect_timeout_s;
  unsigned int read_timeout = options.read_timeout_s;
  unsigned int write_timeout = options.write_timeout_s;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
  mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
  mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
  // §3.2 全表 utf8mb4；客户端默认 latin1，不显式指定会把中文写成 '?'。
  const char* charset = "utf8mb4";
  mysql_options(m, MYSQL_SET_CHARSET_NAME, charset);

  if (mysql_real_connect(m, options.host.c_str(), options.user.c_str(),
                         options.password.c_str(), options.database.c_str(),
                         options.port, nullptr, 0) == nullptr) {
    *error = "连接失败 code=" + std::to_string(mysql_errno(m)) + " msg=" +
             mysql_error(m) + " → " + describe_target(options);
    mysql_close(m);
    return false;
  }

  auto fail = [&](const std::string& what, unsigned int code, const char* msg) {
    *error = what + " code=" + std::to_string(code) + " msg=" + msg + " → " +
             describe_target(options);
    mysql_close(m);
    return false;
  };

  // ---- 会话级设置（[D-17] 存 UTC / 头文件【超时四档】）----
  if (mysql_query(m, "SET SESSION time_zone = '+00:00'") != 0) {
    return fail("设置会话时区失败", mysql_errno(m), mysql_error(m));
  }
  const std::string lock_waits = "SET SESSION innodb_lock_wait_timeout = " +
                                 std::to_string(options.lock_wait_timeout_s);
  if (mysql_query(m, lock_waits.c_str()) != 0) {
    return fail("设置 innodb_lock_wait_timeout 失败", mysql_errno(m), mysql_error(m));
  }
  if (options.max_execution_time_ms > 0) {
    const std::string met =
        "SET SESSION max_execution_time = " +
        std::to_string(options.max_execution_time_ms);
    if (mysql_query(m, met.c_str()) != 0) {
      return fail("设置 max_execution_time 失败", mysql_errno(m), mysql_error(m));
    }
  }

  // ---- 三项校验：静默截断 / 静默退化成 MyISAM / 字符集不是 utf8mb4 / 时区不是 UTC ----
  if (mysql_query(m,
                  "SELECT @@session.sql_mode, @@session.time_zone, "
                  "@@session.character_set_client") != 0) {
    return fail("读取会话变量失败", mysql_errno(m), mysql_error(m));
  }
  MYSQL_RES* res = mysql_store_result(m);
  if (res == nullptr) {
    return fail("读取会话变量没有返回结果集", mysql_errno(m), mysql_error(m));
  }
  const MYSQL_ROW row = mysql_fetch_row(res);
  const std::string sql_mode = (row != nullptr && row[0] != nullptr) ? row[0] : "";
  const std::string time_zone = (row != nullptr && row[1] != nullptr) ? row[1] : "";
  const std::string charset_now = (row != nullptr && row[2] != nullptr) ? row[2] : "";
  mysql_free_result(res);

  if (sql_mode.find("STRICT_TRANS_TABLES") == std::string::npos) {
    mysql_close(m);
    *error =
        "会话 sql_mode 不含 STRICT_TRANS_TABLES：超长字符串会被【静默截断】"
        "（submissions.message 是 VARCHAR(255)，截断后前端拿到半句话且没有任何报错）。"
        "修复：在 my.cnf 的 [mysqld] 段补 sql_mode 后重启 mysqld。当前 sql_mode=" +
        sql_mode + " → " + describe_target(options);
    return false;
  }
  if (sql_mode.find("NO_ENGINE_SUBSTITUTION") == std::string::npos) {
    mysql_close(m);
    *error =
        "会话 sql_mode 不含 NO_ENGINE_SUBSTITUTION：§3.2 的 DDL 若写错 ENGINE 会"
        "【静默退化成 MyISAM】，丢掉全部外键与事务 —— 而硬删账号依赖 CASCADE[D-57]。"
        "当前 sql_mode=" +
        sql_mode + " → " + describe_target(options);
    return false;
  }
  if (charset_now != "utf8mb4") {
    mysql_close(m);
    *error = "连接字符集不是 utf8mb4（实际 " + charset_now +
             "）：题面/代码/编译输出全是中文，latin1 下会写成 '?'。" +
             describe_target(options);
    return false;
  }
  if (time_zone != "+00:00") {
    mysql_close(m);
    *error = "会话时区不是 +00:00（实际 " + time_zone + "）：[D-17] 约定存 UTC。" +
             describe_target(options);
    return false;
  }

  *out = m;
  return true;
}

// 失败时把 errno 填进 error —— 调用方据此判断是"SQL 写错了"还是"连接已经死了"。
// 这两种必须分开：后者要标记连接可疑并在下一次取用时重建，前者只该上抛给 DAO。
bool prepare_stmt(MYSQL* m, const std::string& sql, MYSQL_STMT** out, DbError* error) {
  *out = nullptr;
  MYSQL_STMT* stmt = mysql_stmt_init(m);
  if (stmt == nullptr) {
    *error = DbError::local("mysql_stmt_init 返回空（内存不足？）");
    return false;
  }
  if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) !=
      0) {
    *error = server_error(mysql_stmt_errno(stmt), mysql_stmt_sqlstate(stmt),
                          mysql_stmt_error(stmt), sql);
    error->message = "预处理失败：" + error->message;
    mysql_stmt_close(stmt);
    return false;
  }
  *out = stmt;
  return true;
}

}  // namespace

// =============================================================================
// Connection —— 一条物理连接的内部状态（头文件里只前置声明）
// =============================================================================
struct Connection {
  MYSQL* mysql = nullptr;
  PoolOptions options;

  // 预处理语句缓存：SQL 文本 → 服务端句柄。FIFO 淘汰（见 PoolOptions 注释）。
  // 只有租约持有者会碰这两个容器，而任一时刻一条连接至多有一个持有者。
  std::unordered_map<std::string, MYSQL_STMT*> stmt_cache;
  std::deque<std::string> stmt_order;

  // 上次语句因连接级错误失败 —— 下次取用直接重连（不再 ping：ping 几乎必失败，
  // 多一次往返没有意义）。
  bool suspect = false;
  bool in_transaction = false;
  Clock::time_point idle_since = Clock::now();

  // 与本连接一一对应的常驻 Session（由 Pool 在 init 时创建）。
  // 不在 Session 里存 Connection* 反过来，是因为 Connection 的定义在 Session 之后 ——
  // 用 Session* 就只需要前置声明（与头文件里 struct Connection; 的处理一致）。
  Session* session = nullptr;

  // 连接级统计：用 atomic 是为了让 Pool::stats() 能在 mu_ 下安全读取，
  // 而写入方（租约持有者）不需要再拿一把锁。mu_ 仍是内层锁，故不会产生锁序倒置。
  std::atomic<long> statements{0};
  std::atomic<long> statement_failures{0};
  std::atomic<long> rows_returned{0};
  std::atomic<long> transactions{0};
  std::atomic<long> rollbacks{0};
  std::atomic<long> prepares{0};
  std::atomic<long> stmt_cache_hits{0};
  std::atomic<long> stmt_cache_evictions{0};
  std::atomic<long> stmt_reprepares{0};
  std::atomic<long> reconnects{0};
  std::atomic<long> pings{0};
  std::atomic<long> ping_failures{0};
  std::atomic<long> last_error_code{0};

  void destroy() {
    for (auto& entry : stmt_cache) {
      if (entry.second != nullptr) mysql_stmt_close(entry.second);
    }
    stmt_cache.clear();
    stmt_order.clear();
    if (mysql != nullptr) {
      mysql_close(mysql);
      mysql = nullptr;
    }
  }

  // 兜底：init() 建到第 N 条连接失败时会直接 clear() 掉前面那几条，
  // 那种路径不会（也不该）逐个显式调 destroy()。没有析构函数的话，
  // 那些 MYSQL* 句柄与语句缓存就跟着泄漏了 —— 而且只在"第 2 条起连接失败"
  // 时才发生，正常路径永远测不到。
  ~Connection() { destroy(); }
};

std::vector<std::string> split_statements(const std::string& script, DbError* error) {
  std::vector<std::string> statements;
  std::string parse_error;
  if (!split_script_impl(script, &statements, &parse_error)) {
    if (error != nullptr) *error = DbError::local("多语句脚本解析失败：" + parse_error);
    return {};
  }
  return statements;
}

// =============================================================================
// DbError / Value / Row / ResultSet
// =============================================================================
std::string DbError::format() const {
  if (ok()) return "ok";
  std::string s = server_side()
                      ? "服务端错误 code=" + std::to_string(code) +
                            " sqlstate=" + (sqlstate.empty() ? "-" : sqlstate)
                      : "本地错误";
  s += " msg=" + message;
  if (!sql.empty()) s += " [sql=" + sql + "]";
  return s;
}

DbError DbError::local(std::string message) {
  DbError e;
  e.message = std::move(message);
  return e;
}

Value Value::null() { return Value(); }

Value Value::from_bool(bool v) {
  Value x;
  x.kind_ = Kind::kBool;
  x.bool_ = v;
  return x;
}

Value Value::from_i64(long long v) {
  Value x;
  x.kind_ = Kind::kI64;
  x.i64_ = v;
  return x;
}

Value Value::from_u64(unsigned long long v) {
  Value x;
  x.kind_ = Kind::kU64;
  x.u64_ = v;
  return x;
}

Value Value::from_double(double v) {
  Value x;
  x.kind_ = Kind::kDouble;
  x.double_ = v;
  return x;
}

Value Value::from_text(std::string v) {
  Value x;
  x.kind_ = Kind::kText;
  x.text_ = std::move(v);
  return x;
}

Value Value::from_text(const char* v) {
  return from_text(std::string(v != nullptr ? v : ""));
}

bool Row::is_null(std::size_t col) const { return cells_.at(col).is_null; }

const std::string& Row::str(std::size_t col) const { return cells_.at(col).text; }

std::string Row::str_or(std::size_t col, std::string fallback) const {
  const Cell& c = cells_.at(col);
  return c.is_null ? std::move(fallback) : c.text;
}

namespace {
// 全量解析：必须吃完整串才算数。"12abc" 静默变成 12 比报错难查得多。
template <typename T, typename Parse>
T parse_full(const std::string& text, T fallback, Parse parse) {
  if (text.empty()) return fallback;
  const char* begin = text.c_str();
  char* end = nullptr;
  errno = 0;
  const T value = parse(begin, &end);
  if (end != begin + text.size() || errno == ERANGE) return fallback;
  return value;
}
}  // namespace

long long Row::i64(std::size_t col, long long fallback) const {
  return parse_full(str(col), fallback,
                    [](const char* b, char** e) { return std::strtoll(b, e, 10); });
}

unsigned long long Row::u64(std::size_t col, unsigned long long fallback) const {
  return parse_full(str(col), fallback,
                    [](const char* b, char** e) { return std::strtoull(b, e, 10); });
}

double Row::real(std::size_t col, double fallback) const {
  return parse_full(str(col), fallback,
                    [](const char* b, char** e) { return std::strtod(b, e); });
}

bool Row::boolean(std::size_t col, bool fallback) const {
  const Cell& c = cells_.at(col);
  if (c.is_null) return fallback;
  if (c.text == "1" || c.text == "true" || c.text == "TRUE") return true;
  if (c.text == "0" || c.text == "false" || c.text == "FALSE") return false;
  return fallback;
}

const Row* ResultSet::find(std::size_t col, const std::string& value) const {
  for (const Row& row : rows_) {
    if (row.is_null(col)) continue;
    if (row.str(col) == value) return &row;
  }
  return nullptr;
}

// =============================================================================
// Session
// =============================================================================
void Session::set_error(DbError error) { error_ = std::move(error); }

namespace {
Session& inert_session() {
  // 故意泄漏的单例：避免静态析构顺序问题（与 log.cpp 同一处理）
  static Session* inert = new Session();
  return *inert;
}
}  // namespace

bool Session::exec(const std::string& sql) {
  error_ = DbError();
  if (conn_ == nullptr || conn_->mysql == nullptr) {
    set_error(DbError::local("会话不可用：连接池未就绪，或租约已归还"));
    return false;
  }
  // 第二道护栏：exec() 只服务于无参数语句。带 ? 的必须走 run()，
  // 否则"顺手把值拼进 SQL 再 exec"会成为绕过预处理的后门。
  if (sql.find('?') != std::string::npos) {
    set_error(DbError::local(
        "exec() 不接受含 '?' 的语句：带参数的语句必须走 run(sql, params) 走预处理通道"));
    return false;
  }

  if (mysql_real_query(conn_->mysql, sql.c_str(),
                       static_cast<unsigned long>(sql.size())) != 0) {
    const unsigned int code = mysql_errno(conn_->mysql);
    error_ = server_error(code, mysql_sqlstate(conn_->mysql), mysql_error(conn_->mysql),
                          sql);
    conn_->last_error_code.store(static_cast<long>(code));
    conn_->statement_failures.fetch_add(1);
    if (is_connection_lost(code)) conn_->suspect = true;
    return false;
  }
  // exec() 收到有结果集的语句 = 用错了 API。若不消费掉，连接会一直卡在"还有未读结果"
  // 状态，下一位使用者报 Commands out of sync —— 那是极难查的错，故这里明确拒绝。
  if (mysql_field_count(conn_->mysql) != 0) {
    MYSQL_RES* res = mysql_store_result(conn_->mysql);
    if (res != nullptr) mysql_free_result(res);
    set_error(DbError::local(
        "exec() 收到了会返回结果集的语句（可能是 SELECT）。请改用 run()，"
        "否则结果会被丢弃、连接卡在未读状态"));
    return false;
  }
  conn_->statements.fetch_add(1);
  return true;
}

ResultSet Session::run(const std::string& sql, const std::vector<Value>& params) {
  ResultSet result;
  error_ = DbError();
  if (conn_ == nullptr || conn_->mysql == nullptr) {
    result.error_ = DbError::local("会话不可用：连接池未就绪，或租约已归还");
    set_error(result.error_);
    return result;
  }

  // ---- 取（必要时备）预处理句柄 ----
  MYSQL_STMT* stmt = nullptr;
  auto cache_it = conn_->stmt_cache.find(sql);
  if (cache_it != conn_->stmt_cache.end()) {
    stmt = cache_it->second;
    conn_->stmt_cache_hits.fetch_add(1);
  } else {
    DbError prepare_error;
    if (!prepare_stmt(conn_->mysql, sql, &stmt, &prepare_error)) {
      result.error_ = prepare_error;
      set_error(result.error_);
      conn_->last_error_code.store(static_cast<long>(prepare_error.code));
      conn_->statement_failures.fetch_add(1);
      // ⚠️ 连接级错误在【prepare 阶段】也会发生：断连后第一条"没被缓存过"的语句
      //    会先死在 prepare 上。不标记的话连接会被当成健康的归还，
      //    后面每条语句都失败一次 —— 直到某条恰好命中缓存才走 execute。
      if (is_connection_lost(prepare_error.code)) conn_->suspect = true;
      return result;
    }
    conn_->prepares.fetch_add(1);
    // 容量满 → FIFO 淘汰最老的一条（头文件：为什么是 FIFO 不是 LRU）
    while (conn_->stmt_cache.size() >= conn_->options.stmt_cache_capacity &&
           !conn_->stmt_order.empty()) {
      const std::string oldest = conn_->stmt_order.front();
      conn_->stmt_order.pop_front();
      const auto it = conn_->stmt_cache.find(oldest);
      if (it != conn_->stmt_cache.end()) {
        if (it->second != nullptr) mysql_stmt_close(it->second);
        conn_->stmt_cache.erase(it);
        conn_->stmt_cache_evictions.fetch_add(1);
      }
    }
    conn_->stmt_cache.emplace(sql, stmt);
    conn_->stmt_order.push_back(sql);
  }

  // ---- 第一道护栏：占位符个数必须与参数个数一致（在【发 SQL 之前】拦下）----
  // 少传参数比语法错误危险得多：MySQL 会照常执行，只是 WHERE 少了一段判断，
  // 返回一批"看起来正常"而语义已错的结果。
  const unsigned long expected = mysql_stmt_param_count(stmt);
  if (expected != params.size()) {
    result.error_ = DbError::local("占位符个数(" + std::to_string(expected) +
                                   ")与参数个数(" + std::to_string(params.size()) +
                                   ")不一致");
    result.error_.sql = sql;
    set_error(result.error_);
    conn_->statement_failures.fetch_add(1);
    return result;
  }

  // ---- 绑定参数 ----
  // 标量存进预先 reserve 好的临时数组：MYSQL_BIND::buffer 要的是裸指针，
  // 直接指向 accessor 返回的临时对象会拿到已析构的内存。
  const std::size_t n = params.size();
  struct Flag {
    bool value = false;
  };
  std::vector<MYSQL_BIND> binds(n);
  std::vector<Flag> null_flags(n);
  std::vector<long long> int_scratch;
  std::vector<unsigned long long> uint_scratch;
  std::vector<double> double_scratch;
  int_scratch.reserve(n);
  uint_scratch.reserve(n);
  double_scratch.reserve(n);
  std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * n);
  for (std::size_t i = 0; i < n; ++i) {
    MYSQL_BIND& bind = binds[i];
    switch (params[i].kind()) {
      case Value::Kind::kNull:
        bind.buffer_type = MYSQL_TYPE_NULL;
        null_flags[i].value = true;
        bind.is_null = &null_flags[i].value;
        break;
      case Value::Kind::kBool:
        int_scratch.push_back(params[i].as_bool() ? 1 : 0);
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.buffer = &int_scratch.back();
        break;
      case Value::Kind::kI64:
        int_scratch.push_back(params[i].as_i64());
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.buffer = &int_scratch.back();
        break;
      case Value::Kind::kU64:
        uint_scratch.push_back(params[i].as_u64());
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.is_unsigned = 1;
        bind.buffer = &uint_scratch.back();
        break;
      case Value::Kind::kDouble:
        double_scratch.push_back(params[i].as_double());
        bind.buffer_type = MYSQL_TYPE_DOUBLE;
        bind.buffer = &double_scratch.back();
        break;
      case Value::Kind::kText:
        // 空串按长度 0 绑定：MySQL 侧同样区分 '' 与 NULL，不在这里偷换
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = const_cast<char*>(params[i].as_text().data());
        bind.buffer_length = static_cast<unsigned long>(params[i].as_text().size());
        break;
    }
  }
  if (n != 0 && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
    const unsigned int code = mysql_stmt_errno(stmt);
    result.error_ =
        server_error(code, mysql_stmt_sqlstate(stmt), mysql_stmt_error(stmt), sql);
    set_error(result.error_);
    conn_->last_error_code.store(static_cast<long>(code));
    conn_->statement_failures.fetch_add(1);
    return result;
  }

  // ---- 执行（含【唯一】的安全重试：服务端句柄失效后重备一次）----
  int rc = mysql_stmt_execute(stmt);
  if (rc != 0 && is_stmt_stale(static_cast<unsigned int>(mysql_stmt_errno(stmt)))) {
    // 服务端【没有执行】这条语句（句柄已不存在），因此重备后重试是安全的。
    mysql_stmt_close(stmt);
    conn_->stmt_cache.erase(sql);
    for (auto it = conn_->stmt_order.begin(); it != conn_->stmt_order.end(); ++it) {
      if (*it == sql) {
        conn_->stmt_order.erase(it);
        break;
      }
    }
    DbError prepare_error;
    if (!prepare_stmt(conn_->mysql, sql, &stmt, &prepare_error)) {
      prepare_error.message = "重备失败：" + prepare_error.message;
      result.error_ = prepare_error;
      set_error(result.error_);
      conn_->statement_failures.fetch_add(1);
      if (is_connection_lost(prepare_error.code)) conn_->suspect = true;
      return result;
    }
    conn_->prepares.fetch_add(1);
    conn_->stmt_reprepares.fetch_add(1);
    conn_->stmt_cache.emplace(sql, stmt);
    conn_->stmt_order.push_back(sql);
    rc = (n != 0 && mysql_stmt_bind_param(stmt, binds.data()) != 0)
             ? 1
             : mysql_stmt_execute(stmt);
  }
  if (rc != 0) {
    const unsigned int code = mysql_stmt_errno(stmt);
    result.error_ =
        server_error(code, mysql_stmt_sqlstate(stmt), mysql_stmt_error(stmt), sql);
    set_error(result.error_);
    conn_->last_error_code.store(static_cast<long>(code));
    conn_->statement_failures.fetch_add(1);
    if (is_connection_lost(code)) {
      conn_->suspect = true;
      // 事务中途断连：连接已不可用，后续任何语句都会失败。置位让下次取用直接重连。
      conn_->in_transaction = false;
    }
    return result;
  }

  result.affected_rows_ = mysql_stmt_affected_rows(stmt);
  result.insert_id_ = mysql_stmt_insert_id(stmt);
  conn_->statements.fetch_add(1);

  // ---- 元信息：列名（无结果集时 metadata 为空）----
  MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
  if (meta == nullptr) {
    // 无结果集语句（INSERT/UPDATE/DDL…）：store_result 仍要调一次，
    // 否则连接上会残留未消费的状态。
    mysql_stmt_store_result(stmt);
    mysql_stmt_free_result(stmt);
    return result;
  }
  const unsigned int field_count = mysql_num_fields(meta);
  MYSQL_FIELD* fields = mysql_fetch_fields(meta);
  result.columns_.reserve(field_count);
  for (unsigned int i = 0; i < field_count; ++i) {
    result.columns_.emplace_back(fields[i].name != nullptr ? fields[i].name : "");
  }
  mysql_free_result(meta);

  if (mysql_stmt_store_result(stmt) != 0) {
    const unsigned int code = mysql_stmt_errno(stmt);
    result.error_ =
        server_error(code, mysql_stmt_sqlstate(stmt), mysql_stmt_error(stmt), sql);
    set_error(result.error_);
    conn_->statement_failures.fetch_add(1);
    mysql_stmt_free_result(stmt);
    return result;
  }

  // ---- 逐行取回并整批缓冲 ----
  // 256 字节的初始 buffer 是刻意选的小：绝大多数列（id/verdict/status）远小于它，
  // 而 statement_md / code / compiler_output 一定会走【截断后按精确长度重取】那条路 ——
  // 那条路必须被日常负载反复走到才算被测到。
  struct Slot {
    bool flag = false;
  };
  std::vector<std::string> buffers(field_count, std::string(256, '\0'));
  std::vector<unsigned long> lengths(field_count, 0);
  std::vector<Slot> nulls(field_count);
  std::vector<Slot> truncated(field_count);
  std::vector<MYSQL_BIND> result_binds(field_count);
  std::memset(result_binds.data(), 0, sizeof(MYSQL_BIND) * result_binds.size());
  for (unsigned int i = 0; i < field_count; ++i) {
    result_binds[i].buffer_type = MYSQL_TYPE_STRING;
    result_binds[i].buffer = buffers[i].data();
    result_binds[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
    result_binds[i].length = &lengths[i];
    result_binds[i].is_null = &nulls[i].flag;
    result_binds[i].error = &truncated[i].flag;  // 该列被 buffer_length 截断
  }
  if (mysql_stmt_bind_result(stmt, result_binds.data()) != 0) {
    const unsigned int code = mysql_stmt_errno(stmt);
    result.error_ =
        server_error(code, mysql_stmt_sqlstate(stmt), mysql_stmt_error(stmt), sql);
    set_error(result.error_);
    conn_->statement_failures.fetch_add(1);
    mysql_stmt_free_result(stmt);
    return result;
  }

  bool fetch_failed = false;
  for (;;) {
    const int fetch_rc = mysql_stmt_fetch(stmt);
    if (fetch_rc == 1 || fetch_rc == MYSQL_NO_DATA) break;  // 1 与 100 都表示读完
    if (fetch_rc != 0 && fetch_rc != MYSQL_DATA_TRUNCATED) {
      const unsigned int code = mysql_stmt_errno(stmt);
      result.error_ =
          server_error(code, mysql_stmt_sqlstate(stmt), mysql_stmt_error(stmt), sql);
      set_error(result.error_);
      conn_->last_error_code.store(static_cast<long>(code));
      conn_->statement_failures.fetch_add(1);
      if (is_connection_lost(code)) conn_->suspect = true;
      result.rows_.clear();
      fetch_failed = true;
      break;
    }

    std::vector<Row::Cell> cells;
    cells.reserve(field_count);
    for (unsigned int i = 0; i < field_count; ++i) {
      Row::Cell cell;
      if (nulls[i].flag) {
        cell.is_null = true;
      } else if (truncated[i].flag || lengths[i] > buffers[i].size()) {
        // 被 buffer_length 截断：以 length 为准重新取完整列。
        // §3.2 的 statement_md / code / compiler_output 都是 MEDIUMTEXT，
        // 代码上限 64KB（§5.8）—— 这条路径是常态，不是边角。
        std::string exact(lengths[i], '\0');
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = exact.data();
        bind.buffer_length = static_cast<unsigned long>(exact.size());
        unsigned long got = 0;
        bind.length = &got;
        if (mysql_stmt_fetch_column(stmt, &bind, i, 0) != 0) {
          result.error_ = DbError::local("取回第 " + std::to_string(i) +
                                         " 列失败：" + mysql_stmt_error(stmt));
          set_error(result.error_);
          conn_->statement_failures.fetch_add(1);
          result.rows_.clear();
          fetch_failed = true;
          break;
        }
        exact.resize(got);
        cell.text = std::move(exact);
      } else {
        // 【不要】buffers[i].resize(lengths[i])：resize 可能触发重新分配，
        // 而 result_binds[i].buffer 还指着这块内存 —— 第二行开始就会读到已释放的地址。
        // （缩小的情形在 libstdc++ 上恰好不会 realloc，但"恰好"不是契约。）
        // 按已知长度拷贝一份即可，buffer 的所有权始终留在 buffers 里。
        cell.text.assign(buffers[i].data(), lengths[i]);
      }
      cells.push_back(std::move(cell));
    }
    if (fetch_failed) break;
    result.rows_.push_back(Row(std::move(cells)));
  }

  // 无论成败都要消费干净，否则连接卡在未读状态
  mysql_stmt_free_result(stmt);
  conn_->rows_returned.fetch_add(static_cast<long>(result.rows_.size()));
  return result;
}

std::size_t Session::exec_script(const std::string& script,
                                 std::vector<std::string>* failed) {
  std::vector<std::string> statements;
  std::string split_error;
  if (!split_script_impl(script, &statements, &split_error)) {
    set_error(DbError::local("DDL 脚本解析失败：" + split_error));
    if (failed != nullptr) failed->push_back(split_error);
    return 0;
  }
  std::size_t done = 0;
  for (const std::string& sql : statements) {
    if (!exec(sql)) {
      if (failed != nullptr) failed->push_back(error_.format());
      break;  // 幂等建表遇错必须停下：后面的表大概率依赖前面的（外键）
    }
    ++done;
  }
  return done;
}

bool Session::begin() {
  if (conn_ != nullptr && conn_->in_transaction) {
    set_error(DbError::local("begin() 时已经处于事务中"));
    return false;
  }
  if (!exec("START TRANSACTION")) return false;
  conn_->in_transaction = true;
  conn_->transactions.fetch_add(1);
  return true;
}

bool Session::commit() {
  if (conn_ == nullptr || !conn_->in_transaction) {
    set_error(DbError::local("commit() 时没有处于事务中"));
    return false;
  }
  conn_->in_transaction = false;
  if (!exec("COMMIT")) {
    // 提交失败：事务【可能已经生效】。因此不自动回滚 —— 把"提交失败"伪装成
    // "已回滚"比报错更难查（§T2.11 依赖调用方据此把整条 submission 置 failed）。
    return false;
  }
  return true;
}

bool Session::rollback() {
  if (conn_ == nullptr || !conn_->in_transaction) return true;  // 无事务可回滚，幂等
  conn_->in_transaction = false;
  const bool ok = exec("ROLLBACK");
  if (conn_ != nullptr) conn_->rollbacks.fetch_add(1);
  return ok;
}

bool Session::in_transaction() const {
  return conn_ != nullptr && conn_->in_transaction;
}

bool Session::ping() {
  error_ = DbError();
  if (conn_ == nullptr || conn_->mysql == nullptr) {
    set_error(DbError::local("会话不可用"));
    return false;
  }
  conn_->pings.fetch_add(1);
  if (mysql_ping(conn_->mysql) == 0) return true;
  conn_->ping_failures.fetch_add(1);
  const unsigned int code = mysql_errno(conn_->mysql);
  error_ = server_error(code, mysql_sqlstate(conn_->mysql), mysql_error(conn_->mysql),
                        "PING");
  conn_->suspect = true;
  return false;
}

unsigned long long Session::connection_id() const {
  if (conn_ == nullptr || conn_->mysql == nullptr) return 0;
  const ResultSet rs = const_cast<Session*>(this)->run("SELECT CONNECTION_ID() AS id");
  if (rs.failed() || rs.empty()) return 0;
  return rs.at(0).u64(0);
}

const std::string& Session::host() const {
  static const std::string kUnknown;
  return conn_ != nullptr ? conn_->options.host : kUnknown;
}

unsigned int Session::port() const {
  return conn_ != nullptr ? conn_->options.port : 0;
}

const std::string& Session::database() const {
  static const std::string kUnknown;
  return conn_ != nullptr ? conn_->options.database : kUnknown;
}

void Session::detach() {
  if (conn_ == nullptr) return;
  if (conn_->in_transaction) {
    // 租约到期仍有未提交事务 —— 这是调用方的 bug（正常路径由 Transaction 守卫回滚）。
    // 但【必须】兜底回滚：连接是共享的，带着未提交事务归还，会让下一个使用者
    // 在同一个事务里看到不该看到的数据；§5.7 的启动恢复逻辑也依赖这个不变量。
    DbError saved = error_;
    exec("ROLLBACK");
    conn_->rollbacks.fetch_add(1);
    error_ = saved;
    common::log_error(common::LogChannel::kApp,
                      "租约结束时发现未提交事务，已兜底回滚（调用方应使用 Transaction 守卫）");
  }
}

// =============================================================================
// Transaction
// =============================================================================
Transaction::Transaction(Session& session) : session_(&session) {
  started_ = session.begin();
  active_ = started_;
}

Transaction::~Transaction() {
  if (active_ && session_ != nullptr) session_->rollback();
}

bool Transaction::commit() {
  if (!active_) return false;
  const bool ok = session_->commit();
  active_ = false;  // 提交失败也不回滚（见头文件注释）
  return ok;
}

void Transaction::rollback() {
  if (!active_) return;
  active_ = false;
  session_->rollback();
}

// =============================================================================
// Lease
// =============================================================================
Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_),
      session_(other.session_),
      db_lock_(std::move(other.db_lock_)),
      waited_(other.waited_) {
  other.pool_ = nullptr;
  other.session_ = nullptr;
}

Lease& Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    release();  // 不显式调 ~Lease()：那是对 nontrivial 对象复用已析构存储的 UB
    pool_ = other.pool_;
    session_ = other.session_;
    db_lock_ = std::move(other.db_lock_);
    waited_ = other.waited_;
    other.pool_ = nullptr;
    other.session_ = nullptr;
  }
  return *this;
}

void Lease::release() {
  if (session_ == nullptr || pool_ == nullptr) return;
  Connection* conn = session_->conn_;
  const Clock::time_point start = Clock::now();
  // ① 回滚未提交事务并清状态（此时仍持有全局互斥量与这条连接）
  session_->detach();
  // ② 归还连接：置空闲 +（必要时）重建 + 通知等待者。
  //    必须早于 ③ 释放全局互斥量 —— 见头文件【D-21 的全局互斥量 = 两层锁】里
  //    对归还顺序的论证。重建也在这一步做，而不是等下一次取用：此刻仍持有
  //    全局互斥量，"重建期间不可能存在未提交事务"这个不变式才成立。
  pool_->give_back(conn, Clock::now() - start);
  session_->conn_ = nullptr;
  // ③ 释放 [D-21] 的全局互斥量
  db_lock_.unlock();
}

Lease::~Lease() { release(); }

Session& Lease::session() const {
  if (session_ != nullptr) return *session_;
  // 惰性失败会话：让"忘记检查 valid()"的后果变成一条错误 + 一条 ERROR 日志，
  // 而不是空指针崩溃。它不意味着可以忽略检查 —— 租约无效时每条语句都会失败。
  return inert_session();
}

// =============================================================================
// Pool
// =============================================================================
std::mutex& db_mutex() {
  // 故意泄漏：见文件头【实现要点】5。进程级唯一，与 Pool 实例数无关。
  static std::mutex* instance = new std::mutex();
  return *instance;
}

Pool::Pool() = default;

Pool::~Pool() { shutdown(); }

Pool& Pool::instance() {
  // 同样故意泄漏：生命周期由 init()/shutdown() 显式管理
  static Pool* singleton = new Pool();
  return *singleton;
}

bool Pool::init(const PoolOptions& options, std::vector<std::string>* problems) {
  auto add = [&](const std::string& message) {
    if (problems != nullptr) problems->push_back(message);
  };

  // ---- 先校验参数：不合法就不要浪费一次连接尝试 ----
  std::string invalid;
  if (options.size == 0) invalid = "pool size 必须 ≥ 1";
  else if (options.host.empty()) invalid = "host 不能为空";
  else if (options.port == 0 || options.port > 65535) invalid = "port 必须在 1..65535";
  else if (options.user.empty()) invalid = "user 不能为空";
  else if (options.database.empty()) invalid = "database 不能为空";
  else if (options.connect_timeout_s == 0) invalid = "connect_timeout_s 必须 ≥ 1";
  else if (options.read_timeout_s == 0) invalid = "read_timeout_s 必须 ≥ 1";
  else if (options.write_timeout_s == 0) invalid = "write_timeout_s 必须 ≥ 1";
  else if (options.lock_wait_timeout_s == 0) invalid = "lock_wait_timeout_s 必须 ≥ 1";
  else if (options.stmt_cache_capacity == 0) invalid = "stmt_cache_capacity 必须 ≥ 1";
  if (!invalid.empty()) {
    add("连接池参数不合法：" + invalid);
    common::log_error(common::LogChannel::kApp, "连接池参数不合法：" + invalid);
    return false;
  }

  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    add("mysql_library_init 失败");
    common::log_error(common::LogChannel::kApp, "MySQL 客户端库初始化失败");
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  close_all();  // 重复 init 时先清干净，避免遗留连接
  options_ = options;

  // ---- 建立全部连接；任一条失败即整体失败（不留半池）----
  for (std::size_t i = 0; i < options.size; ++i) {
    auto conn = std::unique_ptr<Connection>(new Connection());
    conn->options = options;
    std::string error;
    MYSQL* raw = nullptr;
    if (!open_connection(options, &raw, &error)) {
      add("第 " + std::to_string(i + 1) + "/" + std::to_string(options.size) +
          " 条连接建立失败：" + error);
      common::log_error(common::LogChannel::kApp, "MySQL " + error);
      connections_.clear();
      sessions_.clear();
      free_.clear();
      ready_ = false;
      closed_ = true;
      mysql_library_end();
      return false;
    }
    conn->mysql = raw;
    conn->idle_since = Clock::now();
    auto session = std::unique_ptr<Session>(new Session(conn.get()));
    conn->session = session.get();
    free_.push_back(conn.get());
    sessions_.push_back(std::move(session));
    connections_.push_back(std::move(conn));
  }

  stats_ = PoolStats();
  ready_ = true;
  closed_ = false;
  // 注意用 describe_locked()：此刻仍持有 mu_，而 describe() 会再加一次锁 ——
  // std::mutex 不可重入，那会是一把自己等自己的死锁（且只在 init 成功时触发，
  // 失败路径提前 return 了，所以它很容易躲过"只测失败路径"的用例）。
  common::log_info(common::LogChannel::kApp, "MySQL 连接池就绪 " + describe_locked());
  return true;
}

bool Pool::init_from_config(std::vector<std::string>* problems) {
  const common::Config& cfg = common::config();
  PoolOptions options;
  options.host = cfg.mysql_host;
  options.port = (cfg.mysql_port > 0) ? static_cast<unsigned int>(cfg.mysql_port) : 3306u;
  options.user = cfg.mysql_user;
  options.password = cfg.mysql_password;
  options.database = cfg.mysql_db;
  return init(options, problems);
}

void Pool::close_all() {
  // 调用方必须已持有 mu_ 或保证无并发（init / shutdown 两条路径都是如此）
  for (auto& conn : connections_) {
    if (conn != nullptr) conn->destroy();
  }
  connections_.clear();
  sessions_.clear();  // Session 必须在 Connection 之后销毁
  free_.clear();
}

void Pool::shutdown() {
  // 先摘下"就绪"标志，让并发的 acquire() 立刻返回无效租约，
  // 再去抢全局互斥量等在途租约结束 —— 否则会关闭一条正在执行语句的连接。
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_ && connections_.empty()) {
      ready_ = false;
      return;
    }
    ready_ = false;
    closed_ = true;
  }
  long pending = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending = stats_.active_leases;
  }
  if (pending > 0) {
    // 会一直等到它们归还。提前说一句，是为了"卡住"这件事可被诊断：
    // 不带这句的话，看到的就是一个没有任何输出的 shutdown()。
    common::log_info(common::LogChannel::kApp,
                     "MySQL 连接池关闭中：等待 " + std::to_string(pending) +
                         " 个在途租约归还");
  }
  {
    std::unique_lock<std::mutex> db_lock(db_mutex());
    std::lock_guard<std::mutex> lock(mu_);
    cv_.notify_all();
    close_all();
    stats_.active_leases = 0;
  }
  common::log_info(common::LogChannel::kApp,
                  "MySQL 连接池已关闭 " + describe());
  mysql_library_end();
}

bool Pool::ready() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ready_ && !closed_;
}

void Pool::give_back(Connection* conn, std::chrono::nanoseconds held) {
  bool need_rebuild = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stats_.active_leases > 0) stats_.active_leases -= 1;
    lease_owner_ = std::thread::id{};
    stats_.lease_seconds_micros +=
        std::chrono::duration_cast<std::chrono::microseconds>(held).count();
    if (conn != nullptr) {
      if (conn->mysql != nullptr && !conn->suspect && !conn->in_transaction) {
        conn->idle_since = Clock::now();
      } else {
        need_rebuild = true;
      }
      // 无论是否需要重建都先归还：重建失败时下一次取用会再试一次
      // （数据库可能正在重启，重试比重试失败即放弃更符合运维预期）。
      free_.push_back(conn);
    }
    cv_.notify_all();
  }

  if (conn == nullptr || !need_rebuild) return;
  // 簿记已出锁，连接重建的网络 I/O 不占用 mu_。
  // 此刻仍持有 [D-21] 的全局互斥量（由 ~Lease 在下一步才释放），
  // 因此不存在"另一条线程拿到这条半重建的连接"的可能。
  conn->destroy();
  MYSQL* raw = nullptr;
  std::string error;
  if (open_connection(conn->options, &raw, &error)) {
    conn->mysql = raw;
    conn->reconnects.fetch_add(1);
    common::log_warn(common::LogChannel::kApp,
                     "MySQL 连接已重建 → " + describe_target(conn->options));
  } else {
    conn->mysql = nullptr;
    common::log_error(common::LogChannel::kApp,
                      "MySQL 连接重建失败（连接已归还，下一次取用会再试）：" + error);
  }
}

Lease Pool::acquire() {
  // 自死锁防护：同一线程在持有租约时又取租约 = 自己等自己，且没有任何输出。
  //   这是 [D-21] 串行化 + RAII 组合下最容易写出来的错（DAO 里嵌套一层 lambda、
  //   顺手多取一次租约都是它的来源），而它的表现是"服务莫名卡死、无日志、无堆栈"——
  //   属于最难查的一类故障。因此这里【主动失败并记 ERROR】，而不是安静地等下去。
  // ⚠️ 必须放在【抢全局互斥量之前】：放在之后等于死锁已经生效，检查永远走不到。
  //   不会误报：lease_owner_ 只可能是【此刻正攥着全局互斥量的那个线程】——
  //   别的线程读到它与自己不等，就会正常地去排队等锁。
  {
    std::lock_guard<std::mutex> guard(mu_);
    if (ready_ && !closed_ && lease_owner_ == std::this_thread::get_id()) {
      common::log_error(common::LogChannel::kApp,
                        "同一线程在持有租约时又调用 acquire()：这会与 [D-21] 的全局"
                        "互斥量自死锁（服务会无日志地卡死）。已拒绝本次 acquire —— "
                        "请在【同一次租约内】执行完多条语句，而不是反复取租约");
      stats_.acquire_failures += 1;
      return Lease();
    }
  }

  const Clock::time_point start = Clock::now();
  // ---- [D-21] 的进程级全局互斥量，全程持有到租约析构 ----
  std::unique_lock<std::mutex> db_lock(db_mutex());
  const std::chrono::nanoseconds waited =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);

  Connection* conn = nullptr;
  {
    std::unique_lock<std::mutex> lock(mu_);
    // 归还顺序保证了"拿到 db_mutex_ ⇒ 至少刚有一条连接被归还"，因此这里通常不会阻塞；
    // 保留 wait 是为了让「池被关闭」「连接被丢弃」两条路径有确定行为，而不是靠不变量兜底。
    cv_.wait(lock, [this] { return closed_ || !ready_ || !free_.empty(); });
    if (closed_ || !ready_ || free_.empty()) {
      stats_.acquire_failures += 1;
      return Lease();
    }
    conn = free_.back();
    free_.pop_back();
    stats_.acquires += 1;
    stats_.active_leases += 1;
    lease_owner_ = std::this_thread::get_id();
    if (waited > stats_.max_wait) stats_.max_wait = waited;
  }

  // ---- 必要时 ping / 重连（此时已持有全局互斥量，全库没有别的语句在跑）----
  if (conn == nullptr || !ensure_alive(conn)) {
    std::lock_guard<std::mutex> lock(mu_);
    if (conn != nullptr) free_.push_back(conn);
    stats_.acquire_failures += 1;
    cv_.notify_all();
    common::log_error(common::LogChannel::kApp,
                      "MySQL 连接不可用且重建失败，本次 acquire 返回无效租约");
    return Lease();
  }
  Session* session = conn->session;
  session->attach(conn);
  return Lease(this, session, std::move(db_lock), waited);
}

bool Pool::ensure_alive(Connection* conn) {
  if (conn == nullptr) return false;
  if (conn->mysql == nullptr) {
    // 上一次重建失败留下的空连接：重连一次（§Mysql C API：失败后必须重新 init）
    MYSQL* raw = nullptr;
    std::string error;
    if (!open_connection(conn->options, &raw, &error)) {
      common::log_error(common::LogChannel::kApp, "MySQL 连接重建失败：" + error);
      return false;
    }
    conn->mysql = raw;
    conn->suspect = false;
    conn->idle_since = Clock::now();
    conn->reconnects.fetch_add(1);
    return true;
  }
  if (conn->suspect) {
    // 连接级错误已经发生过：ping 几乎必失败，直接重连（省一次往返）
    conn->destroy();
    return ensure_alive(conn);
  }
  const std::chrono::milliseconds idle = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - conn->idle_since);
  if (conn->options.idle_ping_ms == 0 ||
      idle < std::chrono::milliseconds(conn->options.idle_ping_ms)) {
    return true;
  }
  conn->pings.fetch_add(1);
  if (mysql_ping(conn->mysql) == 0) {
    conn->idle_since = Clock::now();
    return true;
  }
  conn->ping_failures.fetch_add(1);
  conn->destroy();
  return ensure_alive(conn);
}

PoolStats Pool::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  PoolStats total = stats_;
  for (const auto& conn : connections_) {
    if (conn == nullptr) continue;
    total.statements += conn->statements.load();
    total.statement_failures += conn->statement_failures.load();
    total.rows_returned += conn->rows_returned.load();
    total.transactions += conn->transactions.load();
    total.rollbacks += conn->rollbacks.load();
    total.prepares += conn->prepares.load();
    total.stmt_cache_hits += conn->stmt_cache_hits.load();
    total.stmt_cache_evictions += conn->stmt_cache_evictions.load();
    total.stmt_reprepares += conn->stmt_reprepares.load();
    total.reconnects += conn->reconnects.load();
    total.pings += conn->pings.load();
    total.ping_failures += conn->ping_failures.load();
    if (conn->last_error_code.load() != 0) {
      total.last_error_code = conn->last_error_code.load();
    }
  }
  return total;
}

PoolOptions Pool::options() const {
  std::lock_guard<std::mutex> lock(mu_);
  return options_;
}

std::string Pool::describe() const {
  std::lock_guard<std::mutex> lock(mu_);
  return describe_locked();
}

std::string Pool::describe_locked() const {
  std::string s = describe_target(options_);
  s += " size=" + std::to_string(options_.size);
  s += " timeout(connect/read/write)=" + std::to_string(options_.connect_timeout_s) + "/" +
       std::to_string(options_.read_timeout_s) + "/" +
       std::to_string(options_.write_timeout_s) + "s";
  s += " lock_wait=" + std::to_string(options_.lock_wait_timeout_s) + "s";
  s += " max_exec=" + std::to_string(options_.max_execution_time_ms) + "ms";
  s += " stmt_cache=" + std::to_string(options_.stmt_cache_capacity);
  return s;
}

void Pool::reset_stats() {
  std::lock_guard<std::mutex> lock(mu_);
  stats_ = PoolStats();
  for (auto& conn : connections_) {
    if (conn == nullptr) continue;
    conn->statements.store(0);
    conn->statement_failures.store(0);
    conn->rows_returned.store(0);
    conn->transactions.store(0);
    conn->rollbacks.store(0);
    conn->prepares.store(0);
    conn->stmt_cache_hits.store(0);
    conn->stmt_cache_evictions.store(0);
    conn->stmt_reprepares.store(0);
    conn->reconnects.store(0);
    conn->pings.store(0);
    conn->ping_failures.store(0);
    conn->last_error_code.store(0);
  }
}

}  // namespace db
}  // namespace oj