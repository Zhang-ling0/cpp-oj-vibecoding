// =============================================================================
// 会话管理的实现（include/web/session.h 的配套）
// 对应任务：SPEC.md §9 M3 T3.3 / T3.10；契约 §6.3，回收策略 §6.3.2，D-69 见 §6.3.2.1
//
// 【本文件里只有 6 条 SQL，逐条对应 sessions 的一个动作】
//   purge_expired   DELETE ... WHERE expires_at < UTC_TIMESTAMP()
//   issue           上面那条 + INSERT（同一事务）
//   find_user       SELECT ... JOIN users WHERE token = ? AND expires_at > UTC_TIMESTAMP()
//   destroy         DELETE ... WHERE token = ?
//   destroy_others  DELETE ... WHERE user_id = ? AND token <> ?
// 全部走预处理语句占位符（§1.4「所有 SQL 走预处理」）：token / user_id / TTL 天数
// 都来自不可信输入或外部输入，本文件没有一处字符串拼接 SQL。
//
// 【EXPLAIN 实测（MySQL 8.0.46，2026-10-14）—— 索引不是"应该能用"，是量出来的】
//   回收语句      type=range  key=idx_expire     ← §6.3.2 要求的形态，无需新增索引
//   destroy_others type=range key=PRIMARY        （token 比较把范围收在主键上，表极小，无所谓）
//   find_user     JOIN 两侧 type=const          （token 走 PRIMARY，user_id 折叠成常量）
//
// 【时间全部由 MySQL 算，不在 C++ 里算】
//   expires_at / created_at 用 `UTC_TIMESTAMP()` 与 `UTC_TIMESTAMP() + INTERVAL ? DAY`
//   在库里算完落库。这样做的理由不是省代码，而是消除【第二个时间来源】：
//   若 C++ 侧用 chrono 拼 'YYYY-MM-DD HH:MM:SS'，就等于自己实现了一遍日历与时区，
//   一旦与连接时区（mysql_pool 锁死 +00:00）不一致，写进去的 expires_at 与
//   purge_expired 的比较基准就差了几小时 —— 症状是"会话随机提前失效/延迟失效"，
//   极难定位。让数据库算，两处必然同源。
//
// 【单条 INSERT 里同时取回 token 是没有必要的】
//   issue() 只回传 token：登录端点要写的只有 `Set-Cookie`，§6.3 的登录响应体是
//   `{userId, username, role}`，不含到期时间。回传一个自己拼的 expires_at 字符串，
//   只会多出一条"这个字符串和库里那个是不是同一个"的不可验证假设。
// =============================================================================
#include "web/session.h"

#include <openssl/rand.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"

namespace oj {
namespace web {
namespace {

// 统一的失败动作：服务端 errno/sqlstate 原文打进 app.log（排查不必再翻日志），
// 同一句话交给上层映射成 1008。error 为 nullptr 时只记日志。
bool fail(std::string* error, const std::string& what, const db::DbError& db_error) {
  const std::string detail = what + "：" + db_error.format();
  common::log_error(common::LogChannel::kApp, "会话 " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

bool fail(std::string* error, const std::string& detail) {
  common::log_error(common::LogChannel::kApp, "会话 " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

const char kHexDigits[] = "0123456789abcdef";

// §6.3.2 的回收语句，三处调用（启动 T3.9、登录 issue、独立调用）共用这一份文本。
// 单独提出来不是为了省字数，而是【这份 SQL 是本模块唯一有正确性要点的地方】
// （UTC_TIMESTAMP vs NOW），三处各写一遍时迟早有一处抄错，而抄错的后果是
// 静默误删未过期会话 —— 没有任何报错。
const char* const kPurgeExpiredSql = "DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP()";

}  // namespace

// ---- 纯函数 -------------------------------------------------------------------

std::string generate_token() {
  unsigned char raw[kTokenRandomBytes];
  // RAND_bytes 在 3.0 起要求 buffer 至少 128 位（kTokenRandomBytes = 32 字节，满足），
  // 返回 1 成功、0 失败（熵源不可用）。**不做重试**：重试只会把"熵源坏了"从一次
  // 明确失败变成一段不确定的等待，而失败点此刻没有任何状态被改动，直接抛最干净。
  if (RAND_bytes(raw, static_cast<int>(kTokenRandomBytes)) != 1) {
    common::log_error(common::LogChannel::kApp, "会话 生成 token 失败：RAND_bytes 不可用（熵源故障）");
    throw std::runtime_error("RAND_bytes 不可用，拒绝签发可预测的会话 token");
  }
  std::string hex;
  hex.reserve(kTokenHexLength);
  for (const unsigned char byte : raw) {
    hex.push_back(kHexDigits[byte >> 4]);
    hex.push_back(kHexDigits[byte & 0x0F]);
  }
  return hex;
}

bool is_token_wellformed(const std::string& token) {
  if (token.size() != kTokenHexLength) return false;
  for (const char c : token) {
    const bool is_digit = c >= '0' && c <= '9';
    const bool is_lower_hex = c >= 'a' && c <= 'f';
    if (!is_digit && !is_lower_hex) return false;
  }
  return true;
}

std::optional<std::string> parse_sid(const std::string& cookie_header) {
  const std::string name(kSessionCookieName);
  std::size_t cursor = 0;
  while (cursor < cookie_header.size()) {
    std::size_t end = cookie_header.find(';', cursor);
    if (end == std::string::npos) end = cookie_header.size();
    // 一个 cookie-pair 的范围是 [cursor, end)，两端与 '=' 两侧都可能带空白
    //（RFC 6265 允许 OWS：浏览器一般不发，但手工构造的请求会）。
    std::size_t begin = cursor;
    while (begin < end && (cookie_header[begin] == ' ' || cookie_header[begin] == '\t')) ++begin;
    std::size_t stop = end;
    while (stop > begin && (cookie_header[stop - 1] == ' ' || cookie_header[stop - 1] == '\t')) --stop;

    if (stop > begin) {
      const std::size_t eq = cookie_header.find('=', begin);
      // 没有 '=' 的片段整体跳过（畸形但不致命），且**不**把 key 当 value 返回。
      if (eq != std::string::npos && eq < stop) {
        std::size_t name_stop = eq;
        while (name_stop > begin &&
               (cookie_header[name_stop - 1] == ' ' || cookie_header[name_stop - 1] == '\t')) {
          --name_stop;
        }
        // 全等匹配：xsid= / sid_extra= 一律不算（见头文件 parse_sid 的说明）
        if (cookie_header.compare(begin, name_stop - begin, name) == 0) {
          std::size_t value_begin = eq + 1;
          while (value_begin < stop &&
                 (cookie_header[value_begin] == ' ' || cookie_header[value_begin] == '\t')) {
            ++value_begin;
          }
          std::string value = cookie_header.substr(value_begin, stop - value_begin);
          // 剥掉一层成对的双引号：Cookie 值被引号包裹是合法写法，
          // 不剥会让 Set-Cookie 写出去的值与库里存的值对不上（永远查不到会话）。
          if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
          }
          if (!value.empty()) return value;
          // 空值（sid= 或 sid=""）等价于"没有这个 Cookie"，且【就此结束】：取第一个
          // sid 就停，不继续往后找。同名 Cookie 重复出现只可能来自不同 Path 的遗留，
          // 继续往后找等于让攻击者用一个空 sid 挡在前面、或让规则随浏览器版本变化；
          // "第一个 sid 说了算"是确定的、与实现无关的规则。
          return std::nullopt;
        }
      }
    }
    cursor = end + 1;
  }
  return std::nullopt;
}

std::string make_set_cookie(const std::string& token) {
  // 属性顺序与 §6.3 的示例逐字一致（D-69）：HttpOnly → SameSite=Lax → Path=/。
  // ⚠️ 故意不加 Secure：首版明文 HTTP，加了 Cookie 永不下发。启用 HTTPS 时
  // 必须同时补 Secure 并让全站走 HTTPS —— 完整清单见 §6.3.2.1。
  return std::string(kSessionCookieName) + "=" + token + "; HttpOnly; SameSite=Lax; Path=/";
}

std::string make_clear_cookie() {
  // 前半段必须与 make_set_cookie() 逐字相同：名字、Path、HttpOnly、SameSite
  // 只要有一项不同，浏览器就当成另一个 Cookie 留着，服务端清了客户端没清。
  return std::string(kSessionCookieName) +
         "=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT";
}

// ---- DB -----------------------------------------------------------------------

bool purge_expired(db::Session& session, long* removed, std::string* error) {
  const db::ResultSet result = session.run(kPurgeExpiredSql);
  if (result.failed()) return fail(error, "回收过期会话失败", result.error());
  if (removed != nullptr) *removed = static_cast<long>(result.affected_rows());
  return true;
}

bool issue(db::Session& session, unsigned long long user_id, std::string* token_out,
           long* purged, std::string* error) {
  // token 在【事务之外】生成：RAND_bytes 是纯 CPU 运算，让它排在 BEGIN 之后
  // 等于在一次极短的写事务里挂着一次熵源调用（失败时还要回滚）。先算好再开事务，
  // 失败点在任何状态被改动之前。
  const std::string token = generate_token();

  db::Transaction transaction(session);
  if (!transaction.ok()) {
    return fail(error, "签发会话失败：开启事务失败：" + session.error().format());
  }

  // 回收与插入同一事务（§6.3.2）：分开提交时，两个并发登录可能删掉对方
  // 刚落库、还没交给浏览器的会话。
  const db::ResultSet purge = session.run(kPurgeExpiredSql);
  if (purge.failed()) return fail(error, "签发会话失败：回收过期会话", purge.error());

  const db::ResultSet inserted = session.run(
      "INSERT INTO sessions (token, user_id, expires_at, created_at)"
      " VALUES (?, ?, UTC_TIMESTAMP() + INTERVAL ? DAY, UTC_TIMESTAMP())",
      {db::Value::from_text(token), db::Value::from_u64(user_id),
       db::Value::from_i64(common::config().session_ttl_days)});
  if (inserted.failed()) return fail(error, "签发会话失败：插入 sessions", inserted.error());

  if (!transaction.commit()) {
    return fail(error, "签发会话失败：提交事务失败：" + session.error().format());
  }
  // 事务提交后才写出参：失败时 *token_out 保持原值（不变式，与 admin.cpp 同一约定）
  if (token_out != nullptr) *token_out = token;
  if (purged != nullptr) *purged = static_cast<long>(purge.affected_rows());
  return true;
}

std::optional<AuthUser> find_user(db::Session& session, const std::string& token,
                                  std::string* error) {
  // 形状检查先于任何 SQL —— 见头文件【token 形状检查必须在每次查库之前】：
  // token 列是 utf8mb4 CHAR(64)，默认排序规则大小写不敏感，
  // 放行非小写 hex 等于把 ci 排序规则变成 token 校验的一部分。
  if (!is_token_wellformed(token)) return std::nullopt;

  static const char* kSql = R"SQL(
SELECT u.id, u.username, u.role
FROM sessions s
JOIN users u ON u.id = s.user_id
WHERE s.token = ? AND s.expires_at > UTC_TIMESTAMP()
)SQL";
  const db::ResultSet result = session.run(kSql, {db::Value::from_text(token)});
  if (result.failed()) {
    fail(error, "查询会话失败", result.error());
    return std::nullopt;
  }
  if (result.empty()) return std::nullopt;   // 无此会话 / 已过期 —— 都是"未登录"

  const db::Row& row = result.at(0);
  AuthUser user;
  user.user_id = row.u64(0);
  user.username = row.str(1);
  user.role = row.str(2);
  return user;
}

bool destroy(db::Session& session, const std::string& token, std::string* error) {
  // 不判 token 形状：登出对任意值都成立，删不掉就是 0 行，那也是成功。
  const db::ResultSet result =
      session.run("DELETE FROM sessions WHERE token = ?", {db::Value::from_text(token)});
  if (result.failed()) return fail(error, "删除会话失败", result.error());
  return true;
}

bool destroy_others(db::Session& session, unsigned long long user_id,
                    const std::string& keep_token, unsigned long long* removed,
                    std::string* error) {
  // token <> ? 就是"保留当前会话"这一条硬要求（D-40 / AC-21）。去掉它，
  // 改完密码的人会立刻被踢下线，那个端点的验收点也就永远通不过。
  const db::ResultSet result = session.run(
      "DELETE FROM sessions WHERE user_id = ? AND token <> ?",
      {db::Value::from_u64(user_id), db::Value::from_text(keep_token)});
  if (result.failed()) return fail(error, "踢掉其他会话失败", result.error());
  if (removed != nullptr) *removed = result.affected_rows();
  return true;
}

// ---- 入口 ---------------------------------------------------------------------

std::optional<AuthUser> authenticate(db::Session& session, const std::string& cookie_header,
                                     std::string* error) {
  const std::optional<std::string> sid = parse_sid(cookie_header);
  if (!sid.has_value()) return std::nullopt;   // 未登录，不是错误：error 保持原值（空）
  return find_user(session, *sid, error);
}

std::optional<AuthUser> authenticate(const std::string& cookie_header, std::string* error) {
  const std::optional<std::string> sid = parse_sid(cookie_header);
  // 【先解析、后取租约】：匿名读题面是本项目的常态（[D-43]），这类请求不该在
  // [D-21] 的全局互斥量上排队 —— 串行化下多一次无谓排队就是多一个请求的延迟。
  if (!sid.has_value()) return std::nullopt;

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    fail(error, "查询会话失败：连接池未就绪，取不到租约（见 app.log）");
    return std::nullopt;
  }
  return find_user(lease.session(), *sid, error);
}

}  // namespace web
}  // namespace oj