// =============================================================================
// 注册 / 登录 / 登出 / 我是谁 的业务层实现（include/web/auth_api.h 的配套）
// 对应任务：SPEC.md §9 M3 T3.3；契约 §6.3，验收 AC-1
//
// 【本文件里只有 4 条 SQL，全部走预处理语句占位符（§1.4 禁止字符串拼接）】
//   注册  SELECT id, role FROM users WHERE username = ?      ← 判重（1005）
//        INSERT INTO users (...) VALUES (?, ?, 'user', UTC_TIMESTAMP())
//   登录  SELECT id, username, role, password_hash FROM users WHERE username = ?
//   （登出与"我是谁"没有 SQL：登出用 session::destroy，"我是谁"用 session::find_user）
//
// 【时间一律由 MySQL 算，不在 C++ 里算】（与 session.cpp 同一口径）
//   created_at 用 UTC_TIMESTAMP()：[D-17] 存 UTC，且连接时区已被 mysql_pool 锁成 +00:00。
//   本文件里没有第二处时钟，也没有第二处 TTL 常量。
//
// 【唯一一条"防枚举"的动作：用户不存在时也跑一次 bcrypt】
//   见 auth_api.h 的说明。假哈希是一个【形状合法、必然验不过】的固定串，
//   用 cost=4 —— 它的作用只是把时间差拉回同一量级，不是模拟真实校验强度。
//   ⚠️ 用固定的假串而不是每次现算：现算的话，"用户不存在"的路径会比登录慢 60ms，
//     那本身又是一个可测的信号。
// =============================================================================
#include "web/auth_api.h"

#include <nlohmann/json.hpp>

#include <string>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"
#include "web/bcrypt.h"

namespace oj {
namespace web {
namespace {

// 一个 cost=4、形状合法的假哈希。用户不存在时拿它跑一次 verify，
// 让"查不到"与"密码错"两条路径的耗时处在同一量级（见头文件【防枚举】）。
constexpr const char* kDummyHash = "$2b$04$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.";

// 一句话的错误出口：把 DbError 格式化后同时进 app.log 与调用方的 error。
bool fail_db(std::string* error, const std::string& what, const db::DbError& db_error) {
  const std::string detail = what + "：" + db_error.format();
  common::log_error(common::LogChannel::kApp, "认证 " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

void set_failure(AuthFailure* out, common::ApiError error_value, const std::string& reason,
                 bool server_side) {
  if (out == nullptr) return;
  out->error = error_value;
  out->field.reason = reason;
  out->server_side = server_side;
}

void set_field_failure(AuthFailure* out, const FieldError& field_error) {
  if (out == nullptr) return;
  out->error = common::ApiError::kInvalidArgument;
  out->field = field_error;
  out->server_side = false;
}

std::string to_base64_size(std::size_t value) { return std::to_string(value); }

}  // namespace

// ---- 纯函数 ---------------------------------------------------------------------

std::optional<FieldError> validate_username(const std::string& username) {
  FieldError error;
  error.field = "username";
  // ⚠️ 【按字节】判长度，不是按字符：username 是 VARCHAR(32)，MySQL 按字符计 utf8mb4
  //   但存储上限是字节口径的密码学上限；本项目限定 ASCII 字符集，两者等价。
  if (username.empty()) {
    error.reason = "用户名不能为空";
    return error;
  }
  if (username.size() < kUsernameMinLength || username.size() > kUsernameMaxLength) {
    error.reason = "用户名长度需为 " + to_base64_size(kUsernameMinLength) + "–" +
                   to_base64_size(kUsernameMaxLength) + " 个字符";
    return error;
  }
  for (const char c : username) {
    const bool is_digit = c >= '0' && c <= '9';
    const bool is_upper = c >= 'A' && c <= 'Z';
    const bool is_lower = c >= 'a' && c <= 'z';
    const bool is_dash = c == '-' || c == '_';
    if (!is_digit && !is_upper && !is_lower && !is_dash) {
      error.reason = "用户名只能包含字母、数字、'-' 与 '_'";
      return error;
    }
  }
  return std::nullopt;
}

std::optional<FieldError> validate_password(const std::string& password) {
  FieldError error;
  error.field = "password";
  if (password.empty()) {
    error.reason = "密码不能为空";
    return error;
  }
  if (password.size() < kPasswordMinLength || password.size() > kPasswordMaxLength) {
    error.reason = "密码长度需为 " + to_base64_size(kPasswordMinLength) + "–" +
                   to_base64_size(kPasswordMaxLength) + " 个字符";
    return error;
  }
  bool has_non_space = false;
  for (const char c : password) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      has_non_space = true;
      break;
    }
  }
  if (!has_non_space) {
    // 只挡"全空白"这种几乎等于空口令的情况，不做复杂度要求（§5.8 没有这条，
    // 且服务端强加复杂度规则会把"密码管理器生成的 20 位随机串"也拦下来）。
    error.reason = "密码不能全是空白字符";
    return error;
  }
  return std::nullopt;
}

// ---- 注册 ------------------------------------------------------------------------

bool register_user(db::Session& db_session, const std::string& username,
                   const std::string& password, AuthUserRecord* out_user,
                   AuthFailure* out_failure, std::string* error) {
  // 生产入口：策略取自 config()。.env 仍是唯一真源（§6.3.1 的声明式取向不变）。
  RegisterPolicy policy;
  policy.allow_register = common::config().allow_register;
  return register_user(db_session, policy, username, password, out_user, out_failure, error);
}

bool register_user(db::Session& db_session, const RegisterPolicy& policy,
                   const std::string& username, const std::string& password,
                   AuthUserRecord* out_user, AuthFailure* out_failure, std::string* error) {
  // ① 注册开关（§6.3：先查 OJ_ALLOW_REGISTER，为 0 时返回 1010 且不落库）
  if (!policy.allow_register) {
    set_failure(out_failure, common::ApiError::kRegisterClosed,
                "本实例已关闭注册（OJ_ALLOW_REGISTER=0）", false);
    return false;
  }
  // ②③ 参数校验
  if (const std::optional<FieldError> username_error = validate_username(username)) {
    set_field_failure(out_failure, *username_error);
    return false;
  }
  if (const std::optional<FieldError> password_error = validate_password(password)) {
    set_field_failure(out_failure, *password_error);
    return false;
  }

  // ④ 判重。⚠️ 查一次不是"防并发"的保证 —— 两个并发注册可能都查不到，
  //   真正的保证是 uk_username + 下面把 1062 也映射成 1005（而不是 1008）。
  static const char* kLookupSql = "SELECT id FROM users WHERE username = ?";
  const db::ResultSet existing = db_session.run(kLookupSql, {db::Value::from_text(username)});
  if (existing.failed()) {
    set_failure(out_failure, common::ApiError::kInternalError, "查询用户失败", true);
    return fail_db(error, "注册时判重失败", existing.error());
  }
  if (!existing.empty()) {
    set_failure(out_failure, common::ApiError::kConflict, "用户名已注册", false);
    return false;
  }

  // bcrypt 只在四道闸门之后做（cost=10 约 60~100ms，见文件头【四道闸门】）
  std::string hash;
  try {
    hash = hash_password(password, kBcryptDefaultCost);
  } catch (const std::exception& thrown) {
    // RAND_bytes 不可用属于"熵源故障"，不是用户输入问题：记 ERROR 并回 1008，
    // 绝不退化成可预测的盐（与 session::generate_token 同一判断）。
    set_failure(out_failure, common::ApiError::kInternalError, "密码哈希失败", true);
    common::log_error(common::LogChannel::kApp,
                      std::string("注册失败：计算密码哈希时出错：") + thrown.what());
    if (error != nullptr) *error = thrown.what();
    return false;
  }

  // role 显式写死 'user'：不依赖 DDL 的 DEFAULT，更不接受请求里的 role（头文件【不做提权】）
  static const char* kInsertSql =
      "INSERT INTO users (username, password_hash, role, created_at) "
      "VALUES (?, ?, 'user', UTC_TIMESTAMP())";
  const db::ResultSet inserted =
      db_session.run(kInsertSql, {db::Value::from_text(username), db::Value::from_text(hash)});
  if (inserted.failed()) {
    // 1062 = Duplicate entry（两个并发注册撞在 uk_username 上）。这与"先查到了"是同一
    // 种用户可见的结果，必须回同一个 1005，否则并发下会偶发 1008。
    if (inserted.error().code == 1062u) {
      set_failure(out_failure, common::ApiError::kConflict, "用户名已注册", false);
      return false;
    }
    set_failure(out_failure, common::ApiError::kInternalError, "写入用户失败", true);
    return fail_db(error, "注册时插入 users 失败", inserted.error());
  }

  if (out_user != nullptr) {
    out_user->user_id = inserted.insert_id();
    out_user->username = username;
    out_user->role = "user";
  }
  common::log_info(common::LogChannel::kApp, "新用户注册：" + username);
  return true;
}

// ---- 登录 ------------------------------------------------------------------------

bool login_user(db::Session& db_session, const std::string& username, const std::string& password,
                AuthUserRecord* out_user, std::string* out_set_cookie, AuthFailure* out_failure,
                std::string* error) {
  // 【参数校验对登录【不设闸】】—— 登录失败统一是 1001，如果这里先按注册那套报
  // "用户名长度不足"，就成了枚举接口的另一半（能区分"格式非法"与"用户不存在"）。
  static const char* kSelectSql =
      "SELECT id, username, role, password_hash FROM users WHERE username = ?";
  // 只按用户名查一条（唯一键），口令在 C++ 侧用 bcrypt 比对 —— 不存在
  // `WHERE username=? AND password_hash=?` 这种写法，哈希是自相关的、没法进 SQL。
  const db::ResultSet found = db_session.run(kSelectSql, {db::Value::from_text(username)});
  if (found.failed()) {
    set_failure(out_failure, common::ApiError::kInternalError, "查询用户失败", true);
    return fail_db(error, "登录时查询 users 失败", found.error());
  }

  if (found.empty()) {
    // 用户不存在：照样跑一次 bcrypt（对着假哈希），把耗时拉回同一量级。
    // ⚠️ 这条会烧掉一次 cost=4 的计算（约 2ms），是刻意的 —— 见头文件【防枚举】。
    (void)verify_password(password, kDummyHash);
    set_failure(out_failure, common::ApiError::kInvalidArgument, "用户名或密码错误", false);
    if (out_failure != nullptr) out_failure->field.field = "password";
    return false;
  }

  const db::Row& row = found.at(0);
  const unsigned long long user_id = row.u64(0);
  const std::string stored_username = row.str(1);
  const std::string role = row.str(2);
  const std::string stored_hash = row.str(3);

  // 库里若存着形状非法的哈希（历史脏数据或手工改库），verify 返回 false ⇒ 1001。
  // 刻意不"按哈希无效就放行"也不"报 1008"：前者是认证绕过，后者把用户的输入问题
  // 变成了服务端故障提示，而真相（库里有坏行）已经在 verify 的形状校验里体现了。
  if (!is_hash_wellformed(stored_hash) || !verify_password(password, stored_hash)) {
    set_failure(out_failure, common::ApiError::kInvalidArgument, "用户名或密码错误", false);
    if (out_failure != nullptr) out_failure->field.field = "password";
    return false;
  }

  // 签发会话：回收过期行 + 插入新行在同一事务里（§6.3.2，见 session::issue）
  std::string token;
  // 会话层与本模块同在 web 命名空间（session.h 把函数直接放在 web 下，不另起命名空间）
  if (!issue(db_session, user_id, &token, nullptr, error)) return false;

  if (out_user != nullptr) {
    out_user->user_id = user_id;
    out_user->username = stored_username;
    out_user->role = role;
  }
  if (out_set_cookie != nullptr) *out_set_cookie = make_set_cookie(token);
  common::log_info(common::LogChannel::kApp, "用户登录：" + stored_username);
  return true;
}

// ---- 登出 ------------------------------------------------------------------------

bool logout_user(db::Session& db_session, const std::string& token, std::string* out_set_cookie,
                 std::string* error) {
  // 【不校验 token 形状】—— session::destroy 对任意值都成立，删不掉就是 0 行，
  // 那也是成功（登出的语义是"让这个浏览器不再是登录态"，幂等是应有性质）。
  if (!destroy(db_session, token, error)) return false;
  if (out_set_cookie != nullptr) *out_set_cookie = make_clear_cookie();
  return true;
}

// ---- 响应体 -----------------------------------------------------------------------

std::string user_payload_json(const AuthUser& user) {
  nlohmann::json payload;
  payload["userId"] = user.user_id;
  payload["username"] = user.username;
  payload["role"] = user.role;
  return payload.dump();
}

std::string user_payload_json(const AuthUserRecord& user) {
  nlohmann::json payload;
  payload["userId"] = user.user_id;
  payload["username"] = user.username;
  payload["role"] = user.role;
  return payload.dump();
}

}  // namespace web
}  // namespace oj
