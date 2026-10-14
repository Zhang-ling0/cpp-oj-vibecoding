// =============================================================================
// 注册 / 登录 / 登出 / 我是谁 —— 四个认证端点的业务层
// 对应任务：SPEC.md §9 M3 T3.3（认证：bcrypt + 64 hex token 会话 + HttpOnly Cookie）
// 规格来源：§6.3 端点清单与错误码表 / §5.8（用户名 3–32、密码 6–64）
//           / [D-24]（bcrypt）/ [D-23]（用户名密码 + Session Cookie）
//           / [D-37] + §6.3.1（角色只由启动同步决定）/ §3.2 users DDL
//           / §10.1 AC-1（重复用户名 1005、错误密码 1001）
//
// 【范围：这一层只回答"算什么、要不要落库、发不发会话"】
//   不含 HTTP 路由与响应包络（T3.5 + common/response.h，尚未开工）、不含 Cookie 解析
//   （web/session.h 已就绪）。因此四个入口都吃【调用方递进来的 db::Session&】，
//   接线时不必改本文件；会话 token 的生成与 Cookie 头的拼装也复用 session.h 的
//   generate_token / issue / make_set_cookie，不在这里另造一套。
//
// 【注册的四道闸门，顺序不能换】（§6.3 端点表 + T3.3）
//   ① OJ_ALLOW_REGISTER=0 → 1010，【且不落库】：§6.3 写的是"先查 OJ_ALLOW_REGISTER"，
//      所以它必须排在参数校验之前 —— 否则关掉注册后仍能通过 1001 的长度校验反推出
//      "注册功能存在但关了"，而这一层本该连库都不碰。
//   ② 用户名校验（1001 + data.field=username）
//   ③ 密码校验（1001 + data.field=password）
//   ④ 用户名唯一（1005）
//   bcrypt 只在【四道都过】之后才做：cost=10 在本机约 60~100ms（§6.2.1 要点 3），
//   把它放在校验之前等于让一个纯格式错误的请求也吃掉 100ms。
//
// 【本文件定下的、SPEC 没单列的规则】
//   用户名字符集限定为 [A-Za-z0-9_-]，长度 3–32（§5.8 给的是长度）。理由有三，
//   都与 DB 现状直接相关：
//     ① `users.username` 是 utf8mb4_general_ci —— 【大小写不敏感】，不限定字符集就会
//        出现"Bob 注册成功、bob 登录时查到的是 Bob"这种大小写别名；
//     ② MySQL 的 PAD SPACE 比较会忽略尾部空格，"bob " 与 "bob" 会被唯一索引判重，
//        用户看到的是"我明明注册了却说已存在"；
//     ③ `OJ_ADMIN_USERNAMES` 是按用户名【精确字符串】匹配的 T3.11 名单，
//        名字里混入不可见字符/前后空格时，运维在 .env 里写的与库里的对不上，
//        提权会静默不生效（§6.3.1 约束 3 只 WARN 缺失名单，不 WARN "名字带了不可见字符"）。
//
// 【登录为什么不给"用户不存在"和"密码错误"分别的文案】
//   §6.3 只说"错误密码返回 1001"。把两种情况都答成同一句"用户名或密码错误"，
//   是为了不让任何人用登录接口枚举出哪些用户名注册过（AC-1 要求的 1005 只针对注册）。
//   代价是用户分不清"名字打错了"与"密码打错了"—— 单机自用场景（§5.4 已声明信任模型）
//   这个代价可以接受，而枚举的成本在本项目里没有对应的收益。
//   另一处配套：用户不存在时也要跑一次 bcrypt（对着一个固定的假哈希），否则
//   "查库即返回"与"跑满 100ms"的时间差就是现成的用户名探测器。
// =============================================================================
#pragma once

#include <optional>
#include <string>

#include "common/errors.h"
#include "web/session.h"  // AuthUser（会话层产出，故定义在那边）

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// ---- §5.8 的长度约束 ------------------------------------------------------------
inline constexpr std::size_t kUsernameMinLength = 3;
inline constexpr std::size_t kUsernameMaxLength = 32;
inline constexpr std::size_t kPasswordMinLength = 6;
// ⚠️ 上限 64 字节（不是字符）：口令按字节进 bcrypt，64 < 72 保证 §6.2.1 的
//   "72 字节密钥流截断"这个分支在本项目里【永远不发生】—— 少一位哈希的密码
//   等于密码里少一位。
inline constexpr std::size_t kPasswordMaxLength = 64;

// ---- 失败载体 --------------------------------------------------------------------

// 1001 的字段级原因（§6.3 响应包络的 data.field / data.reason）。
// 同一类问题不占两个错误码，靠 field 区分（§6.3 端点表下的约定）。
struct FieldError {
  std::string field;    // "username" / "password"
  std::string reason;   // 人可读，≤ 60 字（前端直接显示）
};

struct AuthFailure {
  common::ApiError error = common::ApiError::kInternalError;
  FieldError field;          // 仅 1001 时有意义，其余为空
  bool server_side = false;  // true ⇒ DB 故障（调用方记 ERROR）；false ⇒ 用户输入问题
};

// ---- 纯函数（不碰 DB，永远可测）------------------------------------------------

// 用户名：3–32 个字符，且只允许 [A-Za-z0-9_-]（理由见文件头）。
std::optional<FieldError> validate_username(const std::string& username);
// 密码：6–64 字节。只查长度与"非全空白"，不查字符集 —— 口令的字符集不该由服务端规定。
std::optional<FieldError> validate_password(const std::string& password);

// ---- 结果 -----------------------------------------------------------------------

// 三个端点（register/login/me）共用的响应体：§6.3 的 {userId, username, role}。
struct AuthUserRecord {
  unsigned long long user_id = 0;
  std::string username;
  std::string role = "user";   // §3.2 的 ENUM 只有这两个值
};

// ---- 注册开关：可注入的一份策略 --------------------------------------------------

// 「本实例是否开放注册」，即 `.env` 的 `OJ_ALLOW_REGISTER`（§6.7.3）。
//
// 【为什么把它做成一个"递进来的值"而不是在函数里直接读 config()】
//   生产路径只有一个：register_user(...) 内部读 common::config().allow_register，
//   策略仍然只有 .env 一个真源（§6.3.1 的"声明式而非事件式"取向不变）。
//   但 `common::config()` 是**进程级只读快照**且 load_config() 只能整体重载一次，
//   于是"OJ_ALLOW_REGISTER=0 时返回 1010"这条分支在单测里【无法被触发】——
//   不是没写用例，是写不出来：为了测它去改进程全局配置，会连带污染同进程里
//   config_test 之外的其它模块（它们的结论会依赖用例的执行顺序）。
//   与 admin.h 的 StatusProvider 同一手法：外部递进来，未接线时不必改本文件。
struct RegisterPolicy {
  bool allow_register = true;
};

// ---- 端点业务层（吃租约内的 db::Session）-----------------------------------------

// POST /api/auth/register → 201 {userId, username, role}
// 生产入口：注册开关取自 common::config().allow_register。
//
// 【不做提权】新用户的 role 恒为 "user"：INSERT 里显式写死 'user'，不依赖 DDL 的
//   DEFAULT，也不接受请求里的 role 字段。管理员身份只由 T3.11 的启动同步决定（§6.3.1），
//   端点层任何"顺手支持一下管理员注册"都是把声明式名单变成第二个真源。
// 失败时 out_user 不被写入（保持调用方原值）。
bool register_user(db::Session& session, const std::string& username, const std::string& password,
                   AuthUserRecord* out_user, AuthFailure* out_failure, std::string* error);

// 同上，但注册开关由调用方给定（单测用；生产仍走上面那个入口，保证策略只有一个真源）。
bool register_user(db::Session& session, const RegisterPolicy& policy,
                   const std::string& username, const std::string& password,
                   AuthUserRecord* out_user, AuthFailure* out_failure, std::string* error);

// POST /api/auth/login → Set-Cookie: sid=… + {userId, username, role}
//
// 成功后 *out_set_cookie 是 web/session.h::make_set_cookie 的产物（`sid=…; HttpOnly;
// SameSite=Lax; Path=/`，[D-69]）。会话行由 session::issue 落库，它在【同一事务内】
// 先回收过期会话再插入（§6.3.2）—— 登录端点因此只需调这一个入口。
//
// ⚠️ 耗时：bcrypt cost=10 在本机约 60~100ms，且发生在【持有 DB mutex 期间】
//   （[D-21] 串行化）。§6.2.1 要点 3 已判定这个耗时可接受（< 200ms）。将来若注册/登录
//   流量变大，第一件事是把"查用户"与"算 bcrypt"拆成两段租约，而不是加连接 ——
//   串行化下加连接不提高吞吐（mysql_pool.h 的说明）。
bool login_user(db::Session& session, const std::string& username, const std::string& password,
                AuthUserRecord* out_user, std::string* out_set_cookie, AuthFailure* out_failure,
                std::string* error);

// POST /api/auth/logout：删掉这一行会话并把过期 Cookie 交回去。
// 幂等（§6.3：登出对不存在的会话也算成功）—— 具体删的动作是 session::destroy，
// 本函数只负责"token 从哪来"的口径：形状非法的 token 也照样走 destroy（删不掉就是 0 行）。
bool logout_user(db::Session& session, const std::string& token, std::string* out_set_cookie,
                 std::string* error);

// GET /api/auth/me：不碰 DB，直接序列化已解析出的登录身份（web/session.h 的 AuthUser）。
// 未登录由路由层判（1002，admin::require_admin 同一条口径）。
std::string user_payload_json(const AuthUser& user);
std::string user_payload_json(const AuthUserRecord& user);

}  // namespace web
}  // namespace oj
