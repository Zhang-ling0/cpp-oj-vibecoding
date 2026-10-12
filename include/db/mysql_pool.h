// =============================================================================
// CppOJ MySQL 连接池 —— 进程内【唯一】的 DB 访问入口
// 对应任务：SPEC.md §9 M3 T3.1「db/mysql_pool.h + 互斥量串行化；所有 DAO 用预处理语句」
// 规格来源：[D-21] httplib 线程池 + 互斥量串行化 DB / §2.3 线程模型与锁顺序
//           / §2.1-A 第 1 条（启动幂等建表，本模块的 exec_script）
//           / §6.3 错误码 1008（DB 异常一律 500）/ [D-17] 时间存 UTC
//           / §1.4 性能与并发（P95 < 200ms 的列表查询）/ §3.2 DDL 全为 InnoDB
//
// 【为什么叫「池」但默认只有一条连接】
//   [D-21] 与 §2.3 都写死了一条：DB 访问由【一把进程内全局互斥量】串行化。
//   因此本模块的有效并发恒为 1，size > 1 不会提高吞吐 —— 多出来的连接只会永远空着。
//   保留 size 参数不是为了并发，而是为了让 acquire() 写成"从空闲表取 / 没有就等"这套
//   【池的标准语义】，而不是把一个裸 MYSQL* 扔给 DAO：后者会让 DAO 直接负责
//   连接生命周期，而 T2.11 要求"一个事务里 UPDATE submissions + INSERT 明细行"
//   ——连接的生命周期必须由池兜底，否则每个 DAO 都要自己写一遍 begin/commit/rollback
//   与异常回滚，那是错误最容易藏身的地方。
//
// 【D-21 的全局互斥量 = 两层锁，顺序不可颠倒】
//   db_mutex()  进程级唯一一把。acquire() 全程持有，归还时最后释放。DAO 的每一条语句
//               都在它里面执行 —— 这就是"串行化"的实现本身，不是附加的优化。
//   mu_ / cv_   池内簿记（空闲连接表、每条连接的状态、统计）。只被持有极短时间。
//   归还顺序   ①回滚未提交事务 → ②置空闲并 notify → ③释放 db_mutex_。
//               【②必须在③之前】：反过来的话，等待者拿到 db_mutex_ 时可能还没人归还，
//               就必须再等 cv_，而归还者又要等 db_mutex_ 才能发通知 —— 死锁。
//               现在的顺序让"拿到 db_mutex_"蕴含"至少有一条连接刚被归还"，
//               cv_ 因此在当前实现下不会真的阻塞；保留它是为了让「池被关闭」与
//               「连接被丢弃」两条路径有确定行为，而不是靠一条不变量兜底。
//
// 【绝不持有 DB 锁等待 judge】（§2.3 锁顺序）
//   本模块不含任何回调/等待点：DAO 拿到租约后可以执行任意多条语句，但**不应**在
//   租约内去等 judge worker（那会让整库服务停摆）。需要"提交入库 → 判题"的两段式
//   写法（T3.7）是：本次租约内写完 pending 并提交，释放租约，再入队。
//
// 【超时四档：这是本模块最重要的一段，全部服务于"串行化 ⇒ 一条慢语句拖垮全站"】
//   串行化把「最坏延迟」从"最慢的那条查询"放大成"所有请求之和"。因此每条连接都必须
//   带超时，且超时的代价必须是【一条请求失败】而不是【进程永久卡死】：
//     connect_timeout  5s  起不来时快速失败（路线 A 的 make run 会拉 mysqld）
//     read_timeout    15s  最长合法操作是 judge 写回事务（毫秒级）或 §6.5 的聚合
//                          （P95 < 200ms）。15s 是它的 75 倍余量 —— 不会误杀，
//                          但能把一次楔死限制在 15s 而不是永久
//     write_timeout   15s  单次提交代码 ≤ 64KB（§5.8），量级很小，纯兜底
//     innodb_lock_wait  5s  行锁等待。MySQL 默认 50s：在这套串行化模型下等于
//                          「一条锁等待把全站拖停 50s」，故显式压到 5s
//   另有 max_execution_time（默认 10s）只对【只读 SELECT】生效，给跑飞的聚合兜底；
//   它不覆盖锁等待（那是 innodb_lock_wait_timeout 的职责），二者不要互相顶替。
//
// 【为什么不自动重试失败的语句】——本模块唯一一处"看起来该做但故意不做"的事
//   断连后自动重连 + 自动重试，是两件听起来都对、叠在一起就错的事：
//     重连可以（重连发生在【空闲时】，此时不可能有未提交事务，重建连接无损）；
//     重试不行（INSERT/UPDATE 在"服务端已提交、客户端收到响应前"断连时处于未知态，
//               重试 = 写两次。T2.11 的写回若被重试，submissions 会出现两条用例明细）。
//   所以：本模块只在【租约开始时】ping + 按需重连；语句失败一律如实上抛，
//   由调用方按 §6.3 映射成 1008 / 把 submission 置 failed（§5.7 的既有路径）。
//
// 【语句级细节：时间必须锁死成 UTC】
//   §D-17 约定【存 UTC】。DATETIME 列没有时区，若会话 time_zone 跟随服务器
//   （Ubuntu 默认 Asia/Shanghai），写 NOW() 存进去的就是本地时间，事后无从分辨。
//   故连接建立后一律 `SET SESSION time_zone = '+00:00'`（偏移写法不依赖时区表，
//   空白的 MySQL 也能用），DAO 侧只许用 NOW() 不许用 CURDATE()/CONVERT_TZ。
//
// 【连接建立后必须验证的三件事】（响亮地失败，理由同 §6.3.1 / config 的未知键 WARN）
//   ① sql_mode 含 STRICT_TRANS_TABLES —— 否则超长字符串被静默截断：
//      submissions.message 是 VARCHAR(255)，被截断后前端展示的是半句话且无任何报错。
//   ② sql_mode 含 NO_ENGINE_SUBSTITUTION —— 否则 DDL 写错 ENGINE 时 InnoDB 静默退化成
//      MyISAM（丢掉全部外键与事务，而 §3.2 的级联删除正是硬删账号的实现基础[D-57]）。
//   ③ 连接字符集为 utf8mb4 —— 题面、代码、编译输出全是中文/多字节文本。
//   任何一项不满足都让 init() 失败并给出可执行的修复命令，而不是 WARN 后继续跑。
//
// 【预处理语句是强制的，不是建议】（T3.1「禁止字符串拼接 SQL」）
//   带参数一律走 mysql_stmt_* 通道，参数以二进制协议单独下发，SQL 文本里只有 `?`。
//   两道本地护栏（都在【发 SQL 之前】拦下，因此不会留下半条语句的副作用）：
//     ① 占位符个数与参数个数不符 → 直接失败。（少传一个参数比 SQL 语法错误危险得多：
//        MySQL 会照常执行，只是条件里少了一段判断。）
//     ② exec() 只接受【无参数、不返回结果集】的语句，SQL 文本里出现 `?` 直接拒绝。
//   ⚠️ 本模块【无法】自动识别调用方是否自己把值拼进了 SQL 字符串 —— 那是 DAO 的纪律。
//   能做的是把"拼字符串"这条路变得比"传参"更难受：本文件里所有 API 的 SQL 参数
//   都是 std::string 而非 char*，且没有任何接受"带值 SQL"的便捷重载。
//
// 【结果集一律以文本取回】
//   所有列按原始字节取成 std::string（NULL 单独标记），DAO 自行按 DDL 类型解析。
//   代价：省掉一整套按字段类型分派的结果缓冲；收益：`statement_md` / `code` /
//   `compiler_output` 这些 MEDIUMTEXT 与所有数值列走同一条路径，不会出现
//   "文本列忘了扩容"这类只在长内容时才暴露的 bug。BIGINT UNSIGNED 取回是
//   20 字符的十进制串（实测），不会溢出。
//   注意 MEDIUMTEXT 允许内嵌 '\0'，长度以 length 字段为准，不要用 strlen 语义。
//
// 【超时/重连的观测口径】PoolStats 全部可读，进程退出时由 shutdown() 打进 app.log：
//   排队时长、语句数、错误码分布、重连次数、ping 失败、prepare 次数与缓存命中率。
//   其中 max_wait_ns 是 D-21 串行化是否成为瓶颈的唯一直接证据（SPEC §8 R-6）。
// =============================================================================
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oj {
namespace db {

// 前置声明：连接的全部内部状态（MYSQL*、预处理语句缓存、存活标记、统计）都在 .cpp 里。
struct Connection;

// ---- 错误 ---------------------------------------------------------------------

// 一次失败的全部信息。code == 0 且 message 为空 ⟺ 成功（ok()）。
// sql 保留占位符原文（`?`），【绝不】记录参数值 —— 用户代码与密码都走这条通道。
struct DbError {
  unsigned int code = 0;    // MySQL errno；本地校验失败为 0
  std::string sqlstate;     // 5 字符 SQLSTATE，仅服务端错误有
  std::string message;
  std::string sql;

  bool ok() const { return code == 0 && message.empty(); }
  bool server_side() const { return code != 0; }
  explicit operator bool() const { return !ok(); }
  // "服务端错误 code=1062 sqlstate=23000 msg=Duplicate entry ... [sql=INSERT ...]"
  std::string format() const;
  static DbError local(std::string message);
};

// ---- 参数 ---------------------------------------------------------------------

// 预处理语句的绑定值。没有"默认构造的字符串等于 NULL"这种隐式转换：
// 空串与 NULL 是两种不同的值（§3.2 里 hint_md 可为 NULL，statement_md 不可），
// 由调用方用 null() / from_text("") 明确区分。
class Value {
 public:
  enum class Kind { kNull, kBool, kI64, kU64, kDouble, kText };

  static Value null();
  static Value from_bool(bool v);
  static Value from_i64(long long v);
  static Value from_u64(unsigned long long v);
  static Value from_double(double v);
  static Value from_text(std::string v);
  static Value from_text(const char* v);

  Kind kind() const { return kind_; }
  bool is_null() const { return kind_ == Kind::kNull; }

  // 载荷只读访问器，供本模块的绑定实现与单测使用。构造仍一律走 from_*/null()，
  // 目的是让"空串 ≠ NULL"这件事在调用点上就显式（见本类的注释）。
  bool as_bool() const { return bool_; }
  long long as_i64() const { return i64_; }
  unsigned long long as_u64() const { return u64_; }
  double as_double() const { return double_; }
  const std::string& as_text() const { return text_; }

 private:
  friend class Session;
  Kind kind_ = Kind::kNull;
  bool bool_ = false;
  long long i64_ = 0;
  unsigned long long u64_ = 0;
  double double_ = 0.0;
  std::string text_;
};

// ---- 结果 ---------------------------------------------------------------------

// 一行。所有列都按字节取回；NULL 由 is_null 单独标记（text 为空串）。
class Row {
 public:
  std::size_t size() const { return cells_.size(); }
  bool is_null(std::size_t col) const;
  // NULL 时返回空串。要区分"列不存在"用 is_null()，不要靠 str() 是否为空。
  const std::string& str(std::size_t col) const;
  std::string str_or(std::size_t col, std::string fallback) const;
  // 以下按 DDL 类型解析；无法解析（NULL / 非数字 / 有余数）时返回 fallback。
  long long i64(std::size_t col, long long fallback = 0) const;
  unsigned long long u64(std::size_t col, unsigned long long fallback = 0) const;
  double real(std::size_t col, double fallback = 0.0) const;
  bool boolean(std::size_t col, bool fallback = false) const;

 private:
  friend class Session;
  struct Cell {
    std::string text;
    bool is_null = false;
  };
  explicit Row(std::vector<Cell> cells) : cells_(std::move(cells)) {}
  std::vector<Cell> cells_;
};

// 一条语句的完整结果（行已全部取回内存 —— 见【结果集一律以文本取回】及其理由）。
// 【为什么必须整批取回而不是逐行游标】：连接是共享的（size=1 + 全局互斥量），
// 游标必须活到 DAO 读完为止；期间该连接上不能再发任何语句，否则 MySQL 报
// "Commands out of sync"，而这正是把租约借出去最容易犯的错。整批取回让语句
// 在 run() 返回时就已彻底结束，连接立刻干净。
class ResultSet {
 public:
  bool ok() const { return error_.ok(); }
  bool failed() const { return !ok(); }
  const DbError& error() const { return error_; }

  const std::vector<std::string>& columns() const { return columns_; }
  std::size_t column_count() const { return columns_.size(); }
  const std::vector<Row>& rows() const { return rows_; }
  std::size_t size() const { return rows_.size(); }
  bool empty() const { return rows_.empty(); }
  const Row& at(std::size_t i) const { return rows_.at(i); }
  // 首列/指定列等于给定值的第一行；没有则 nullptr。免去 DAO 里到处写循环。
  const Row* find(std::size_t col, const std::string& value) const;

  unsigned long long affected_rows() const { return affected_rows_; }
  unsigned long long insert_id() const { return insert_id_; }

 private:
  friend class Session;
  DbError error_;
  std::vector<std::string> columns_;
  std::vector<Row> rows_;
  unsigned long long affected_rows_ = 0;
  unsigned long long insert_id_ = 0;
};

// ---- 连接（租约期内的一切 DB 操作）----------------------------------------------

// 只能由 Pool::acquire() 产生。**生命周期长于租约**：池为每条连接常驻一个 Session，
// 租约只是把它借出去。这么设计的理由是"漏用"这一类错误的失败方式：
//   若 Session 随租约一起销毁，DAO 里一个跨租约存活的 Session* 就是 use-after-free
//   —— release 构建下它往往"还能用"，改不掉一条数据也看不出来，直到某天崩在别处；
//   现在它会确定地返回"会话不可用"并被记 ERROR。
//   同时它【不可能】造成数据竞争：同一时刻一条连接至多有一个租约，租约归还时
//   conn_ 被置空，池级互斥量又保证不会有两个租约指向同一条连接。
class Session {
 public:
  Session() = default;

  bool valid() const { return conn_ != nullptr; }
  const DbError& error() const { return error_; }

  // 无参数、不返回结果集的语句：DDL / SET / START TRANSACTION / COMMIT / ROLLBACK。
  // SQL 里出现 `?` 直接拒绝（这是"禁止字符串拼接"的第二道护栏）。
  bool exec(const std::string& sql);

  // 参数化语句。这是 DAO 的唯一入口：带参数一律走这里。
  // 失败时返回的 ResultSet 里 ok() 为 false，error() 有完整信息。
  ResultSet run(const std::string& sql, const std::vector<Value>& params = {});

  // 多语句脚本（db/init/01-schema.sql）。按 ; 切分，识别 -- 行注释、/* */ 块注释、
  // 单引号/双引号/反引号里的分号。返回成功执行的条数；任一条失败即停并返回已执行条数。
  // 用途是 §2.1-A 第 1 条的"启动时幂等建表"——MySQL 默认禁用 multi-statement，
  // 整份 DDL 不能作为一条语句发过去。
  std::size_t exec_script(const std::string& script, std::vector<std::string>* failed = nullptr);

  // ---- 事务（§2.2 / T2.11）----------------------------------------------------
  bool begin();
  bool commit();
  bool rollback();
  bool in_transaction() const;

  // ---- 诊断 ------------------------------------------------------------------
  bool ping();                                    // mysql_ping；失败即视为需要重连
  unsigned long long connection_id() const;       // SHOW PROCESSLIST / SHOW ENGINE INNODB STATUS 用
  // ⚠️ 这三个只应在【持有租约期间】调用：它们返回的引用/值来自池内的 Connection，
  //   而 Pool::shutdown() 会销毁全部 Connection。调用方若把 host() 的引用留到
  //   shutdown 之后再读，就是悬空引用。它们的存在只是为了把连接信息打进错误日志。
  const std::string& host() const;
  unsigned int port() const;
  const std::string& database() const;

 private:
  friend class Pool;
  friend class Lease;
  explicit Session(Connection* conn) : conn_(conn) {}
  void attach(Connection* conn) { conn_ = conn; }
  // 租约结束时由 Lease 调用：未提交则回滚，并清掉残留状态。
  void detach();
  void set_error(DbError error);

  Connection* conn_ = nullptr;
  DbError error_;
};

// 事务守卫：析构时若未提交则回滚。§T2.11 的三段写（UPDATE 汇总列 + INSERT 用例行
// + INSERT 诊断行）必须同事务，用它可保证任何一条失败都不会留下"半个提交"。
class Transaction {
 public:
  explicit Transaction(Session& session);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  bool active() const { return active_; }
  bool ok() const { return started_; }
  // 提交失败时不会自动回滚（提交点之后的语义由调用方决定），但会置 active_=false，
  // 析构时不再重复回滚 —— 避免把"提交失败"伪装成"已回滚"这种更难查的状态。
  bool commit();
  void rollback();

 private:
  Session* session_ = nullptr;
  bool started_ = false;
  bool active_ = false;
};

// ---- 租约 ---------------------------------------------------------------------

class Pool;  // 前置声明：租约持有池指针（归还连接），而池的接口返回租约

// 持有 [D-21] 全局互斥量的凭证。**租约内可以执行任意多条语句，但不应在租约内等待
// judge**（§2.3 锁顺序）—— 那会让整库服务停摆。
// 不可拷贝，可移动。
class Lease {
 public:
  Lease() = default;
  Lease(Lease&& other) noexcept;
  Lease& operator=(Lease&& other) noexcept;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  ~Lease();

  bool valid() const { return session_ != nullptr; }
  explicit operator bool() const { return valid(); }

  // 无效租约（池未就绪 / 已关闭）返回一个【惰性失败】的会话：任何语句都返回错误，
  // 而不是崩在空指针上。这不是为了让忘记检查的代码"看起来能跑"，而是为了让
  // 忘记检查的后果变成一条可观测的 ERROR 日志 + 1008 响应。
  Session& session() const;
  Session* operator->() const { return &session(); }
  Session& operator*() const { return session(); }

  // 为本次租约等待全局互斥量花掉的时长（D-21 串行化是否成为瓶颈的直接证据，§8 R-6）。
  std::chrono::nanoseconds waited() const { return waited_; }

 private:
  friend class Pool;
  Lease(Pool* pool, Session* session, std::unique_lock<std::mutex> db_lock,
        std::chrono::nanoseconds waited)
      : pool_(pool), session_(session), db_lock_(std::move(db_lock)), waited_(waited) {}
  // 归还连接 + 释放全局互斥量。~Lease 与 operator= 共用：
  // 后者若想"先清干净自己再接管新的"，正确写法是调它而不是显式调 ~Lease() ——
  // 对一个 nontrivial 对象显式调用析构函数再复用其存储是不合法的（UB），
  // 哪怕它眼下能跑通。
  void release();

  Pool* pool_ = nullptr;
  // 不拥有 Session —— Session 由池常驻持有（见上面 Session 的注释）
  Session* session_ = nullptr;
  std::unique_lock<std::mutex> db_lock_;
  std::chrono::nanoseconds waited_{0};
};

// ---- 连接池 -------------------------------------------------------------------

struct PoolOptions {
  std::string host = "127.0.0.1";
  unsigned int port = 3306;
  std::string user = "oj";
  std::string password = "oj";
  std::string database = "oj";

  // 见本文件顶部【为什么叫「池」但默认只有一条连接】。默认 1 = 单条连接 + 串行化，
  // 与 [D-21] 一致。> 1 不提高吞吐（D-21 的全局互斥量仍是瓶颈），仅为兼容池语义。
  std::size_t size = 1;

  unsigned int connect_timeout_s = 5;
  unsigned int read_timeout_s = 15;
  unsigned int write_timeout_s = 15;
  // MySQL 默认 50s，在串行化模型下等于"一条锁等待停站 50s"，故压到 5s。
  unsigned int lock_wait_timeout_s = 5;
  // 只读 SELECT 的兜底；0 = 不设置（服务端默认同样是不限）。不覆盖锁等待。
  unsigned int max_execution_time_ms = 10000;

  // 空闲超过该毫秒数后，下次取用先 ping 一次再决定是否重连。
  // 太短 = 每次取用都多一次往返；太长 = 连接静默死掉后要等更久才被发现。
  // 30s 的依据：路由 B 的 mysqld 默认 wait_timeout 8h、宿主机 mysqld 同量级，
  // 真实的断连原因是重启/被 KILL，那类情况由"语句失败即标记可疑"这条更快。
  unsigned int idle_ping_ms = 30000;

  // 每条连接缓存的预处理语句条数上限。DAO 的 SQL 是固定十几个集合，
  // 64 足够把每次查询的"多一次 PREPARE 往返 + 服务端解析"全省掉；
  // 超出按 FIFO 淘汰（不是 LRU：规模太小，LRU 多出的链表迭代器没有可观测收益）。
  std::size_t stmt_cache_capacity = 64;
};

struct PoolStats {
  long acquires = 0;            // acquire() 成功次数
  long acquire_failures = 0;    // 池不可用（未就绪/已关闭/连接无法重建）
  long active_leases = 0;       // 当前在途租约数；shutdown 卡住时用它定位是谁没归还
  long lease_seconds_micros = 0;// 所有租约持有时长之和（串行化下 ≈ DB 忙碌总时长）
  std::chrono::nanoseconds max_wait{0};  // 单次等待全局互斥量的最大值

  long statements = 0;          // 成功执行的语句数（含缓存命中）
  long statement_failures = 0;
  long rows_returned = 0;
  long transactions = 0;        // begin() 次数
  long rollbacks = 0;           // 显式 + 租约结束时兜底

  long prepares = 0;            // 真正的 mysql_stmt_prepare 次数
  long stmt_cache_hits = 0;
  long stmt_cache_evictions = 0;
  // 服务端句柄失效后的重备次数（1243/1615）。
  // ⚠️ 【本机实测恒为 0】：MySQL 8.0.46 对 ALTER / TRUNCATE / DROP+CREATE / FLUSH TABLES
  //   四种表结构变更一律【透明重备】，从不把错误码抛给客户端（已逐条验证）。
  //   保留这条计数与对应的重试分支，是因为该行为随服务端版本与 optimizer_switch 变化，
  //   不能靠"我这里不会发生"来保证。哪天它开始计数，就说明服务端行为变了。
  long stmt_reprepares = 0;

  long reconnects = 0;          // 重建了 TCP 连接
  long pings = 0;
  long ping_failures = 0;
  long last_error_code = 0;     // 最近一次服务端错误的 errno
};

class Pool {
 public:
  Pool();
  ~Pool();
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  // 生产代码只用这一个（[D-21] 的互斥量是进程级的，多个实例之间同样串行）。
  static Pool& instance();

  // 建立全部连接并逐条校验 sql_mode / 字符集。任一条失败即整体失败（不留半池）。
  // problems 非空时填入人可读的失败原因（启动时直接打进 app.log）。
  bool init(const PoolOptions& options, std::vector<std::string>* problems = nullptr);
  // 用 common::config() 的 MYSQL_* 字段（§6.7.3）。必须在 load_config() 之后调用。
  bool init_from_config(std::vector<std::string>* problems = nullptr);

  // 关闭全部连接。会等待正在进行的租约结束（它先拿全局互斥量）。
  // ⚠️ 因此【不能在持有租约时调用】——哪怕是同一个线程：全局互斥量正被你自己攥着，
  //   shutdown 会一直等下去。这是刻意的（宁可等，也不关闭一条正在执行语句的连接），
  //   正常的调用位置只有进程退出路径。等待开始时会打一条 INFO 便于定位。
  void shutdown();

  bool ready() const;

  // 取一条连接。阻塞直到 [D-21] 的全局互斥量可用（之后通常立即返回）。
  // 池不可用时返回无效租约（valid()==false），不抛异常、不崩溃。
  // ⚠️ 同一线程持有租约时再调本函数会被【主动拒绝】并记 ERROR，而不是无限期阻塞 ——
  //   那条路是自死锁。正确写法是【在一次租约内跑完多条语句】。
  Lease acquire();

  PoolStats stats() const;
  // 启动回显用的一行。注意它【不是"把密码掩码后打出来"】：options_.password
  // 压根没进这条串（describe_locked 里只取 host/port/database/user 与各项超时）。
  // 之所以写明这一点，是因为"掩码"这个词会让人以为该函数仍持有密码的副本。
  std::string describe() const;
  // 返回【副本】而非引用：options_ 只在 init()/init_from_config() 里写，
  // 返回引用会让"调用方拿着一个引用、init() 同时在改它"成为可能的数据竞争。
  // 代价是几十字节拷贝，对只在启动/诊断路径调用的接口完全可接受。
  PoolOptions options() const;

  // 单测专用：把 stats 清零（不改变连接状态）
  void reset_stats();

 private:
  friend class Lease;
  friend class Session;

  void give_back(Connection* conn, std::chrono::nanoseconds held);
  bool ensure_alive(Connection* conn);
  void close_all();
  // 调用方必须已持有 mu_。单独拆出来是因为 init() 在持锁状态下要打启动日志，
  // 而 describe() 会再加一次锁 —— std::mutex 不可重入。
  std::string describe_locked() const;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Connection>> connections_;
  // 与 connections_ 一一对应：每条连接常驻一个 Session（见 Session 的生命周期注释）
  std::vector<std::unique_ptr<Session>> sessions_;
  std::vector<Connection*> free_;
  PoolOptions options_;
  PoolStats stats_;
  // 当前在途租约的持有线程（D-21 下至多一个）。用于自死锁防护，见 acquire()。
  std::thread::id lease_owner_;
  bool closed_ = true;
  bool ready_ = false;
};

// [D-21] 的进程级全局互斥量。测试里构造多个 Pool 实例时它们仍共用这一把 ——
// "全局"指的是进程，不是一个对象。
std::mutex& db_mutex();

// 把多语句脚本按 ; 拆开，用的是 exec_script() 内部那一套规则（注释 / 引号 /
// 反引号里的分号都不算分隔符），且【注释在此已被剥掉】。
// 单独暴露的理由：调用方常常要在执行之前先看一眼"这份脚本会被拆成什么"——
// 启动时的建表核对（数表名、核对数量）就是这种需求。若调用方自己再写一遍
// 切分/去注释逻辑，两份实现迟早在边界写法上漂移，而漂移的方向通常是
// "多认了一个 CREATE TABLE"这种静默失准。
// 解析失败（未闭合的引号 / 块注释）时返回空并把原因写进 error。
std::vector<std::string> split_statements(const std::string& script, DbError* error);

}  // namespace db
}  // namespace oj