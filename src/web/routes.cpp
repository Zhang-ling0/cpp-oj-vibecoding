// =============================================================================
// 路由接线层（include/web/routes.h 的配套）
// 见该头文件顶部对分层边界的说明。此处把 §6.3 的全部端点接起来：
//   认证组（register/login/logout/me/password/account）、题目组（tags/problems/
//   problems/{slug}/favorite）、提交组（6 个）、统计组（stats/me/overview）、admin 组。
// 业务逻辑一律在 web/*_api 里，本文件只做三件事：取参 → 调业务层 → 发包络。
// 不做"路由表"抽象：各端点的形状差异（要不要租约、要不要 Set-Cookie、匿名还是必须登录、
// 失败码由谁给）大到一张表反而会多出一层与业务层重复的映射。
// =============================================================================

// 必须排在 httplib.h 之前：线程池大小是 include 期展开的宏，不是运行期配置。
// §2.3 的 N=8。理由见 routes.h【线程池大小为什么写死 8】。
#define CPPHTTPLIB_THREAD_POOL_COUNT 8

#include "web/routes.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.h"
#include "common/errors.h"
#include "common/log.h"
#include "common/response.h"
#include "db/mysql_pool.h"
#include "web/admin.h"
#include "web/auth_api.h"
#include "web/problem_api.h"
#include "web/session.h"
#include "web/submission_api.h"
#include "web/user_api.h"

namespace oj {
namespace web {
namespace {

// httplib::listen() 是阻塞的，要让 SIGINT 能停下来就得有一个指针够得着的实例。
// 单进程只跑一个 Server（§2.3 的线程模型是单进程的），故裸指针足够。
// 不加锁是有意的：只在信号处理器里被【读】，加锁反而可能在处理器里死锁。
httplib::Server* g_server = nullptr;

// 包络 → 响应。这是全文件【唯一】写 status + body 的地方：
// 分成"先 write_body 再单独赋 status"两处写法时，迟早有一处忘了赋 ——
// 而忘了赋的 httplib 默认 status 是 -1，症状是客户端拿到一个空响应。
void send(const common::Reply& reply, httplib::Response& res) {
  res.status = reply.status;
  common::write_body(reply, res);
}

// 请求体解析。空体 / 非 JSON / 非对象一律 1001。
//
// 【为什么不区分"字段缺失"与"字段为空串"】
//   两者最终都落到 auth_api 的 validate_*，回的是同一个 1001 + 同一个 field。
//   好处是这里少一个分支，而少一个分支就少一种"空串与缺键行为不一致"的可能。
bool parse_body(const httplib::Request& req, nlohmann::json* out, std::string* error) {
  if (req.body.empty()) {
    *error = "请求体为空（需要 application/json 的 {\"username\":…,\"password\":…}）";
    return false;
  }
  try {
    *out = nlohmann::json::parse(req.body);
  } catch (const std::exception& e) {
    *error = std::string("请求体不是合法 JSON：") + e.what();
    return false;
  }
  if (!out->is_object()) {
    *error = "请求体必须是 JSON 对象";
    return false;
  }
  return true;
}

// 取字符串字段；缺键 / null / 非字符串一律按"没填"处理（空串）。
// 非字符串不报错是刻意的：前端发成数字或数组时，validate_* 会因长度不符回 1001，
// 那比这里先回一句"字段类型不对"更贴近前端契约（前端只认 field + reason）。
std::string field_text(const nlohmann::json& body, const char* key) {
  const auto it = body.find(key);
  if (it == body.end() || !it->is_string()) return std::string();
  return it->get<std::string>();
}

// 失败包络 + （服务端原因时）一条 app.log。
// 【为什么不在这里造 1002】：未登录由各端点自己判（有的端点匿名也放行，[D-43]），
// session 层也只返回 nullopt 而不返回码 —— 码由端点决定。
void send_failure(const httplib::Request& req, httplib::Response& res, const AuthFailure& failure) {
  if (failure.server_side) {
    // 服务端原因必须落日志：前端只会看到"服务器内部错误"，
    // 不落日志的话，"登录偶尔 500"这类问题无从追查。
    common::log_error(
        common::LogChannel::kApp,
        req.method + " " + req.path + " 失败（code=" +
            std::to_string(common::code(failure.error)) + "）：" +
            (failure.field.reason.empty() ? std::string("(无 details)") : failure.field.reason));
  }
  send(common::fail(failure.error, failure.field.field, failure.field.reason), res);
}

// 连接池没就绪时的收口。三处都用它，因为【"取不到租约"是一个状态，不是四个错误】。
void send_pool_unavailable(const httplib::Request& req, httplib::Response& res) {
  common::log_error(common::LogChannel::kApp,
                    req.method + " " + req.path + "：连接池未就绪，取不到租约");
  send(common::fail(common::ApiError::kInternalError), res);
}

// ---- 登录上下文（§1.3 的四层鉴权里唯一真正用到的那一层）-------------------------

std::string cookie_of(const httplib::Request& req) {
  return req.has_header("Cookie") ? req.get_header_value("Cookie") : std::string();
}

// 解析登录身份。out 有值 = 已登录；*out_server_error=true = DB 异常（**已自行发 1008**）；
// 两者皆否 = 匿名（[D-43]：题面/列表/标签公开可读，故匿名不是错误）。
//
// 【为什么匿名路径不取 DB 租约】authenticate(cookie, …) 的便捷重载先解析 Cookie，
// 没带 sid 就直接返回 nullopt —— 未登录流量不会在 [D-21] 的全局互斥量上排队。
std::optional<AuthUser> viewer_of(const httplib::Request& req, httplib::Response& res,
                                  bool* out_server_error) {
  *out_server_error = false;
  std::string error;
  std::optional<AuthUser> user = authenticate(cookie_of(req), &error);
  if (!error.empty()) {
    common::log_error(common::LogChannel::kApp,
                      req.method + " " + req.path + " 查会话失败：" + error);
    send(common::fail(common::ApiError::kInternalError), res);
    *out_server_error = true;
    return std::nullopt;
  }
  return user;
}

// 必须登录的端点用它。未登录一律 1002（无 Cookie / 已过期 / sid 形状非法三态同码）。
bool require_viewer(const httplib::Request& req, httplib::Response& res, AuthUser* out) {
  bool server_error = false;
  const std::optional<AuthUser> user = viewer_of(req, res, &server_error);
  if (server_error) return false;
  if (!user.has_value()) {
    send(common::fail(common::ApiError::kUnauthenticated), res);
    return false;
  }
  *out = *user;
  return true;
}

// problem_api 的 ViewerContext：由同一次会话解析结果构造（user_id 决定"这些数据是谁的"）。
ViewerContext viewer_context(const AuthUser& user) {
  ViewerContext viewer;
  viewer.user_id = user.user_id;
  viewer.username = user.username;
  return viewer;
}

// 路径里的数字参数。纯数字外的一切（含空串、负数、超长）一律 false → 1004，
// 不去 SQL 里问"这个 id 存不存在"（那会让每个坏 id 都占一次往返）。
bool path_id(const httplib::Request& req, std::size_t slot, unsigned long long* out) {
  if (req.matches.size() <= slot) return false;
  const std::string& text = req.matches[slot];
  if (text.empty() || text.size() > 19) return false;
  unsigned long long value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned long long>(c - '0');
  }
  *out = value;
  return true;
}

// 查询串原文。httplib 0.60 的 Request 没有 `query` 成员，只有拼在一起的 `target`
// （路径 + '?' + 查询串），故从这里切一刀。
//
// 【为什么切出来的串还要再 URL 解码】httplib 已经把【路径段】解过码，但查询串里的
// 值保持原样 —— 业务层的 parse_*_query 吃的就是未解码原文（problem_api.h 的注释
// 明确要求：不合法的转义要报错而不是尽力半解码）。
std::string query_of(const httplib::Request& req) {
  const std::size_t mark = req.target.find('?');
  return (mark == std::string::npos) ? std::string() : req.target.substr(mark + 1);
}

// 查询串里的数字参数（§5.4 的 from/to）。缺失 / 非法转义 / 非纯数字一律 false。
// 同名参数【最后一个生效】（§1.5）：先把全部同名项扫完再判数字，而不是见一个就返回。
bool query_param_id(const std::string& raw_query, const char* key, unsigned long long* out) {
  bool found = false;
  std::string value;
  std::size_t cursor = 0;
  while (cursor <= raw_query.size()) {
    const std::size_t amp = raw_query.find('&', cursor);
    const std::string item = raw_query.substr(
        cursor, amp == std::string::npos ? std::string::npos : amp - cursor);
    const std::size_t eq = item.find('=');
    std::string name = (eq == std::string::npos) ? item : item.substr(0, eq);
    if (url_decode(name, &name) && name == key) {
      std::string decoded = (eq == std::string::npos) ? std::string() : item.substr(eq + 1);
      if (!url_decode(decoded, &decoded)) return false;   // 坏转义：整条查询串不可信
      value = std::move(decoded);
      found = true;
    }
    if (amp == std::string::npos) break;
    cursor = amp + 1;
  }
  if (!found || value.empty() || value.size() > 19) return false;
  unsigned long long parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + static_cast<unsigned long long>(c - '0');
  }
  *out = parsed;
  return true;
}

// ---- 端点 ----------------------------------------------------------------------

// POST /api/auth/register → 201 {userId, username, role}
// §6.3：先查 OJ_ALLOW_REGISTER（这已是 auth_api 内四道闸门的第一道），
// 且【注册不涉及提权】：role 恒为 'user'，只由 T3.11 的启动同步决定。
void handle_register(const httplib::Request& req, httplib::Response& res) {
  nlohmann::json body;
  std::string parse_error;
  if (!parse_body(req, &body, &parse_error)) {
    send(common::fail(common::ApiError::kInvalidArgument, "", parse_error), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  AuthUserRecord user;
  AuthFailure failure;
  std::string error;
  if (!register_user(lease.session(), field_text(body, "username"),
                     field_text(body, "password"), &user, &failure, &error)) {
    send_failure(req, res, failure);
    return;
  }

  nlohmann::json data;
  data["userId"] = user.user_id;
  data["username"] = user.username;
  data["role"] = user.role;
  send(common::created(data), res);   // 201：§6.3 端点表写死，与 kOk 的 200 不同
}

// POST /api/auth/login → 200 {userId, username, role} + Set-Cookie
// Set-Cookie 由 session::make_set_cookie 逐字产出（[D-69] 的四个属性），
// 本函数【不拼 Cookie 字符串】—— 属性一旦在这里被"顺手"改一个字，
// session.h 的单测就再也测不到真正的线上行为。
// 过期会话回收已并入 session::issue 的同一事务（§6.3.2），故这里无需另调。
void handle_login(const httplib::Request& req, httplib::Response& res) {
  nlohmann::json body;
  std::string parse_error;
  if (!parse_body(req, &body, &parse_error)) {
    send(common::fail(common::ApiError::kInvalidArgument, "", parse_error), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  AuthUserRecord user;
  std::string set_cookie;
  AuthFailure failure;
  std::string error;
  if (!login_user(lease.session(), field_text(body, "username"),
                  field_text(body, "password"), &user, &set_cookie, &failure, &error)) {
    send_failure(req, res, failure);
    return;
  }

  nlohmann::json data;
  data["userId"] = user.user_id;
  data["username"] = user.username;
  data["role"] = user.role;
  send(common::ok(data), res);
  res.set_header("Set-Cookie", set_cookie);
  // ⚠️ 必须【先 send 后 set_header】：反过来的话 send 里的 set_content 不动头，
  //    看似没事；但一旦有人给 send 加上"统一清头"的行为，Cookie 就会被抹掉，
  //    且症状是"登录返回 200 但永远未登录"，极难查。
}

// POST /api/auth/logout → 200 + 清 Cookie。幂等：没有 Cookie 也成功
// （§6.3 的端点语义是"让这个浏览器不再是登录态"，本该幂等）。
//
// 【决策放在 plan_logout 里，理由见 routes.h 的 LogoutPlan】
void handle_logout(const httplib::Request& req, httplib::Response& res) {
  const std::string cookie =
      req.has_header("Cookie") ? req.get_header_value("Cookie") : std::string();
  const LogoutPlan plan = plan_logout(cookie);

  std::string clear_cookie;
  if (plan.needs_db) {
    db::Lease lease = db::Pool::instance().acquire();
    if (!lease.valid()) {
      send_pool_unavailable(req, res);
      return;
    }
    std::string error;
    if (!logout_user(lease.session(), plan.token, &clear_cookie, &error)) {
      common::log_error(common::LogChannel::kApp, "登出失败：" + error);
      send(common::fail(common::ApiError::kInternalError), res);
      return;
    }
  }

  nlohmann::json data;
  data["message"] = "已登出";
  send(common::ok(data), res);
  // 即使本来就没有 Cookie 也要下发清除头：浏览器里可能存着一个本服务认不出来
  // （形状非法）的 sid，让浏览器自己删掉比留着强。
  res.set_header("Set-Cookie",
                 clear_cookie.empty() ? make_clear_cookie() : clear_cookie);
}

// GET /api/auth/me → 200 {userId, username, role}；未登录 1002
// 【未登录一律 1002】：无 Cookie、会话已过期、token 形状非法三种情况在 §6.3 的
//   码表里是同一个码。前端据此渲染"未登录形态顶栏"，而不是弹错误框 ——
//   顶栏每次加载都要问一次，弹框就成了骚扰。
//
// data 复用 auth_api::user_payload_json 而不是就地再拼一遍 {userId, username, role}：
//   三处（register / login / me）共用同一个函数，键名漂移就不可能发生。
//   代价是一次 parse + dump，可忽略（本项目 §1.4 的 P95 预算以毫秒计）。
void handle_me(const httplib::Request& req, httplib::Response& res) {
  const std::string cookie =
      req.has_header("Cookie") ? req.get_header_value("Cookie") : std::string();
  std::string error;
  const std::optional<AuthUser> user = authenticate(cookie, &error);
  if (!error.empty()) {
    common::log_error(common::LogChannel::kApp, "GET /api/auth/me 查会话失败：" + error);
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  if (!user.has_value()) {
    send(common::fail(common::ApiError::kUnauthenticated), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(user_payload_json(*user))), res);
}

// ---- 认证组：改密 / 注销账号（§3.5 / §3.6）--------------------------------------

// POST /api/auth/password → 200 {message}；未登录 1002、旧密码错 1001。
//
// keep_token 从 Cookie 里取当前会话的 token：保住这一行是 D-40 的验收点
// （改密后仍是登录态）。取不到说明会话早已不存在 —— 那时用户拿到的会是 1002
// 而不是"改密成功但立刻被登出"。
void handle_change_password(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  nlohmann::json body;
  std::string parse_error;
  if (!parse_body(req, &body, &parse_error)) {
    send(common::fail(common::ApiError::kInvalidArgument, "", parse_error), res);
    return;
  }

  const std::optional<std::string> token = parse_sid(cookie_of(req));
  if (!token.has_value() || token->empty()) {
    send(common::fail(common::ApiError::kUnauthenticated), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  AuthFailure failure;
  std::string error;
  if (!change_password(lease.session(), user.user_id, *token, field_text(body, "oldPassword"),
                       field_text(body, "newPassword"), &failure, &error)) {
    send_failure(req, res, failure);
    return;
  }

  nlohmann::json data;
  data["message"] = "密码已修改，其他设备的登录已失效";
  send(common::ok(data), res);
}

// DELETE /api/auth/account → 200 {message}。硬删除（D-57），不可恢复。
// 成功后必须清 Cookie：会话行已被外键连带删掉，但浏览器里那个 sid 还在，
// 留着会让下一次 auth/me 得到 1002（"服务端已注销、浏览器还以为是登录态"）。
void handle_delete_account(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  nlohmann::json body;
  std::string parse_error;
  if (!parse_body(req, &body, &parse_error)) {
    send(common::fail(common::ApiError::kInvalidArgument, "", parse_error), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  AuthFailure failure;
  std::string error;
  if (!delete_account(lease.session(), user.user_id, field_text(body, "password"), &failure, &error)) {
    send_failure(req, res, failure);
    return;
  }

  nlohmann::json data;
  data["message"] = "账号已注销";
  send(common::ok(data), res);
  res.set_header("Set-Cookie", make_clear_cookie());
}

// ---- 题目组（§4）------------------------------------------------------------------

// GET /api/tags → 200 {algorithm:[…], source:[…]}；匿名可读。
void handle_tags(const httplib::Request&, httplib::Response& res) {
  TagGroups tags;
  std::string error;
  if (!load_tag_groups(&tags, &error)) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(tags))), res);
}

// GET /api/problems → 列表。匿名可读；登录时 myProgress / isFavorite 有值。
// 参数不合法（1001）与"匿名请求带了个人化筛选"（1001）都由业务层判定，
// 本函数只把 QueryParseError 的 field/reason 原样塞进失败包络。
void handle_problem_list(const httplib::Request& req, httplib::Response& res) {
  ListQuery query;
  QueryParseError invalid;
  if (!parse_list_query(query_of(req), &query, &invalid)) {
    send(common::fail(common::ApiError::kInvalidArgument, invalid.field, invalid.reason), res);
    return;
  }

  bool server_error = false;
  const std::optional<AuthUser> user = viewer_of(req, res, &server_error);
  if (server_error) return;

  // 匿名时 ViewerContext 的 user_id 恒为 0（problem_api.h【user_id = 0 这一个值】），
  // 于是 SQL 文本在登录/未登录两种情况下【完全相同】，只剩一个参数不同。
  const ViewerContext viewer = user.has_value() ? viewer_context(*user) : ViewerContext();
  const ViewerContext* viewer_ptr = user.has_value() ? &viewer : nullptr;

  // 匿名带个人化筛选 → 1001（spec 的明确选择：静默忽略会返回一个"看起来合法但不是
  // 用户要的东西"的列表）。业务层内部还会再兜一次（纵深防御）。
  if (!validate_list_query(viewer_ptr, query, &invalid)) {
    send(common::fail(common::ApiError::kInvalidArgument, invalid.field, invalid.reason), res);
    return;
  }

  ProblemList list;
  std::string error;
  if (!query_problems(viewer_ptr, query, &list, &error)) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(list))), res);
}

// GET /api/problems/{slug} → 详情。匿名可读全部题面（[D-43]）。
// slug 取 req.matches[1]：httplib 已对路径段做过 URL 解码（§1.4「URL 解码后精确匹配」），
// 这里【不再】解一次 —— 二次解码会把题目标识里合法的 % 字符改写掉。
void handle_problem_detail(const httplib::Request& req, httplib::Response& res) {
  const std::string slug = req.matches.size() > 1 ? req.matches[1] : std::string();
  if (slug.empty()) {
    send(common::fail(common::ApiError::kNotFound), res);
    return;
  }

  bool server_error = false;
  const std::optional<AuthUser> user = viewer_of(req, res, &server_error);
  if (server_error) return;

  const ViewerContext viewer = user.has_value() ? viewer_context(*user) : ViewerContext();
  const ViewerContext* viewer_ptr = user.has_value() ? &viewer : nullptr;
  ProblemDetail detail;
  std::string error;
  const DetailStatus status = load_problem_detail(slug, viewer_ptr, &detail, &error);
  if (status == DetailStatus::kNotFound) {
    send(common::fail(common::ApiError::kNotFound), res);
    return;
  }
  if (status == DetailStatus::kError) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(detail))), res);
}

// POST /api/problems/{slug}/favorite → 200 {isFavorite}；必须登录。
void handle_favorite(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  const std::string slug = req.matches.size() > 1 ? req.matches[1] : std::string();
  if (slug.empty()) {
    send(common::fail(common::ApiError::kNotFound), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  bool is_favorite = false;
  FavoriteStatus status = FavoriteStatus::kError;
  std::string error;
  if (!toggle_favorite(lease.session(), user.user_id, slug, &is_favorite, &status, &error)) {
    if (status == FavoriteStatus::kNotFound) {
      send(common::fail(common::ApiError::kNotFound), res);
      return;
    }
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }

  nlohmann::json data;
  data["isFavorite"] = is_favorite;
  send(common::ok(data), res);
}

// ---- 提交组（§5）-------------------------------------------------------------------

// POST /api/submissions → 201 {submissionId, status, queueAhead}。
//
// 【判题闸门放在建行之前】：judge 没起来时回 1009 且【不落库】。反过来（先建行再回 1009）
// 会在库里留下一批永远 pending 的死提交，前端会拿着 submissionId 一直轮询下去。
void handle_submission_create(const httplib::Request& req, httplib::Response& res) {
  nlohmann::json body;
  std::string parse_error;
  if (!parse_body(req, &body, &parse_error)) {
    send(common::fail(common::ApiError::kInvalidArgument, "", parse_error), res);
    return;
  }

  const std::string slug = field_text(body, "problemSlug");
  if (slug.empty()) {
    send(common::fail(common::ApiError::kInvalidArgument, "problemSlug", "缺少题目标识"), res);
    return;
  }
  std::string language = field_text(body, "language");
  if (language.empty()) language = "cpp";   // §5.1：默认 "cpp"（首版只支持 C++）
  const std::string code = field_text(body, "code");
  if (code.empty()) {
    send(common::fail(common::ApiError::kInvalidArgument, "code", "代码不能为空"), res);
    return;
  }
  if (code.size() > common::config().max_code_bytes) {
    send(common::fail(common::ApiError::kInvalidArgument, "code",
                      "代码长度超出上限（" +
                          std::to_string(common::config().max_code_bytes) + " 字节）"),
         res);
    return;
  }

  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  const bool judge_ready = judge_available();
  CreatedSubmission created;
  LookupStatus status = LookupStatus::kFound;
  std::string error;
  if (!create_submission(lease.session(), user.user_id, slug, language, code, judge_ready, &created,
                         &status, &error)) {
    if (status == LookupStatus::kNotFound) {
      send(common::fail(common::ApiError::kNotFound), res);
      return;
    }
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  if (!judge_ready) {
    // 判题器没起来：slug 已验过（1004 仍可正常报），但【不建行】——
    // 建了行也没人来判，库里会留下一批永远 pending 的死提交。
    send(common::fail(common::ApiError::kJudgeUnavailable), res);
    return;
  }
  send(common::created(nlohmann::json::parse(to_json(created))), res);
}

// GET /api/submissions → 提交历史列表（仅本人）。
void handle_submission_list(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  SubmissionQuery query;
  QueryParseError invalid;
  if (!parse_submission_query(query_of(req), &query, &invalid)) {
    send(common::fail(common::ApiError::kInvalidArgument, invalid.field, invalid.reason), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  SubmissionList list;
  std::string error;
  if (!load_submissions(lease.session(), user.user_id, query, &list, &error)) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(list))), res);
}

// GET /api/submissions/diff?from=&to= → 两段代码原文（仅本人两条）。
void handle_submission_diff(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  unsigned long long from_id = 0;
  unsigned long long to_id = 0;
  const std::string raw_query = query_of(req);
  const bool from_ok = query_param_id(raw_query, "from", &from_id);
  const bool to_ok = query_param_id(raw_query, "to", &to_id);
  if (!from_ok) {
    send(common::fail(common::ApiError::kInvalidArgument, "from", "from 必须是纯数字的提交 id"), res);
    return;
  }
  if (!to_ok) {
    send(common::fail(common::ApiError::kInvalidArgument, "to", "to 必须是纯数字的提交 id"), res);
    return;
  }
  if (from_id == to_id) {
    send(common::fail(common::ApiError::kInvalidArgument, "from", "from 与 to 不能是同一条提交"), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  DiffSide from;
  DiffSide to;
  LookupStatus status = LookupStatus::kFound;
  std::string error;
  if (!load_submission_diff(lease.session(), user.user_id, from_id, to_id, &from, &to, &status,
                            &error)) {
    if (status == LookupStatus::kNotOwned) {
      send(common::fail(common::ApiError::kForbidden), res);
      return;
    }
    if (status == LookupStatus::kNotFound) {
      send(common::fail(common::ApiError::kNotFound), res);
      return;
    }
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(from, to))), res);
}

// GET /api/submissions/{id} → 提交详情（仅本人）。归属写进 WHERE（§5.2）。
void handle_submission_detail(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  unsigned long long id = 0;
  if (!path_id(req, 1, &id)) {
    send(common::fail(common::ApiError::kNotFound), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  SubmissionDetail detail;
  LookupStatus status = LookupStatus::kNotFound;
  std::string error;
  if (!load_submission_detail(lease.session(), user.user_id, id, &detail, &status, &error)) {
    if (status == LookupStatus::kNotOwned) {
      send(common::fail(common::ApiError::kForbidden), res);
      return;
    }
    if (status == LookupStatus::kNotFound) {
      send(common::fail(common::ApiError::kNotFound), res);
      return;
    }
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(detail))), res);
}

// DELETE /api/submissions/{id} → 200 {message}（仅本人；admin 不放开）。
void handle_submission_delete(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  unsigned long long id = 0;
  if (!path_id(req, 1, &id)) {
    send(common::fail(common::ApiError::kNotFound), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  LookupStatus status = LookupStatus::kNotFound;
  std::string error;
  if (!delete_submission(lease.session(), user.user_id, id, &status, &error)) {
    if (status == LookupStatus::kNotOwned) {
      send(common::fail(common::ApiError::kForbidden), res);
      return;
    }
    if (status == LookupStatus::kNotFound) {
      send(common::fail(common::ApiError::kNotFound), res);
      return;
    }
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }

  nlohmann::json data;
  data["message"] = "提交记录已删除";
  send(common::ok(data), res);
}

// POST /api/submissions/{id}/retry → 201 {newSubmissionId}。
//
// judge 未接线时传 create=false：归属（1003）/ 状态（1011）照常判定，但**不建行**
// —— 否则会在判题器缺席时留下永远 pending 的死提交。
void handle_submission_retry(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  unsigned long long id = 0;
  if (!path_id(req, 1, &id)) {
    send(common::fail(common::ApiError::kNotFound), res);
    return;
  }

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  unsigned long long new_id = 0;
  bool retryable = false;
  LookupStatus status = LookupStatus::kNotFound;
  std::string error;
  const bool judge_ready = judge_available();
  if (!retry_submission(lease.session(), user.user_id, id, judge_ready, &new_id, &status, &retryable,
                        &error)) {
    if (status == LookupStatus::kNotOwned) {
      send(common::fail(common::ApiError::kForbidden), res);
      return;
    }
    if (status == LookupStatus::kNotFound) {
      send(common::fail(common::ApiError::kNotFound), res);
      return;
    }
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  if (!retryable) {
    send(common::fail(common::ApiError::kStateNotAllowed, "", "只有失败的提交可以重跑"), res);
    return;
  }
  if (!judge_ready) {
    send(common::fail(common::ApiError::kJudgeUnavailable), res);
    return;
  }

  nlohmann::json data;
  data["newSubmissionId"] = new_id;
  send(common::created(data), res);
}

// ---- 统计组（§6）--------------------------------------------------------------------

// GET /api/stats → 四个标量（必须登录）。
void handle_stats(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  UserStats stats;
  std::string error;
  if (!load_user_stats(lease.session(), user.user_id, &stats, &error)) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(stats))), res);
}

// GET /api/me/overview → 概览条与"我的"tab2 的唯一数据源（必须登录）。
void handle_overview(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    send_pool_unavailable(req, res);
    return;
  }

  UserOverview overview;
  std::string error;
  if (!load_user_overview(lease.session(), user.user_id, &overview, &error)) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(overview))), res);
}

// ---- 管理（§7）-------------------------------------------------------------------------

// GET /api/admin/overview → 只读管理面板。先判登录（1002）再判角色（1003），
// 顺序由 admin::require_admin 保证，不可颠倒（§7.1）。
//
// judge 快照走 unwired_status_provider()：judge 未接线时如实报 false + 三个 0，
// 运维必须一眼看出判题没起来，而不是"假装一切正常"。
void handle_admin_overview(const httplib::Request& req, httplib::Response& res) {
  AuthUser user;
  if (!require_viewer(req, res, &user)) return;

  const GuardResult guard = require_admin(std::optional<AuthUser>(user));
  if (guard == GuardResult::kUnauthenticated) {
    send(common::fail(common::ApiError::kUnauthenticated), res);
    return;
  }
  if (guard == GuardResult::kForbidden) {
    send(common::fail(common::ApiError::kForbidden), res);
    return;
  }

  Overview overview;
  std::string error;
  if (!build_overview(unwired_status_provider(), &overview, &error)) {
    send(common::fail(common::ApiError::kInternalError), res);
    return;
  }
  send(common::ok(nlohmann::json::parse(to_json(overview))), res);
}

// ---- 收口 ----------------------------------------------------------------------

// 访问日志：一个请求一行，够定位"点了哪个按钮"又不够淹没 app.log。
// 【刻意只记方法/路径/状态，不记请求体与 Cookie】—— 后两者都是凭据。
void log_access(const httplib::Request& req, const httplib::Response& res) {
  common::log_info(common::LogChannel::kApp,
                   req.method + " " + req.path + " → " + std::to_string(res.status));
}

// 未匹配路由 / 静态文件不存在的统一收口（见 routes.h【错误收口的决策】）。
//
// 【决策本体已抽成纯函数 error_reply_for()，本函数只剩"取判据 → 照做"三步】
//   原因写在 routes.h 里，一句话：判据与日志、httplib 类型混在一起时，只能靠 curl
//   人工比对，而本轮真的在这个判据上踩过一次（未登录 auth/me 返回 404/1004）。
//   ⚠️ 因此这里【不允许】再出现任何 if/switch 形式的判断 —— 那正是把决策搬回
//   不可测位置的唯一方式。status 与 body 是否为空都原样递进去。
httplib::Server::HandlerResponse handle_error(const httplib::Request& req, httplib::Response& res) {
  log_access(req, res);
  const std::optional<ErrorRouting> routing =
      error_reply_for(res.status, /*body_already_written=*/!res.body.empty());
  // nullopt = 响应体已经是我写的（统一包络），httplib 只是因为 status>=400 才回调到这。
  // 返回 Unhandled 让它保持原样：重写会把每一个业务错误都改写成 1004。
  if (!routing.has_value()) return httplib::Server::HandlerResponse::Unhandled;

  if (routing->protocol_level) {
    common::log_warn(common::LogChannel::kApp,
                     req.method + " " + req.path + " 被 httplib 以 " +
                         std::to_string(res.status) + " 拒绝（协议级，非业务错误）");
  }
  send(routing->reply, res);
  return httplib::Server::HandlerResponse::Handled;
}

// 处理器抛异常时的收口：单个请求 500 + app.log，进程继续服务（§1.4 可靠性）。
void handle_exception(const httplib::Request& req, httplib::Response& res,
                      std::exception_ptr ep) {
  std::string what = "未知异常";
  try {
    std::rethrow_exception(ep);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
    what = "非 std::exception 的异常";
  }
  common::log_error(common::LogChannel::kApp,
                    req.method + " " + req.path + " 处理时抛出异常：" + what);
  send(common::fail(common::ApiError::kInternalError), res);
}

}  // namespace

// 登出的决策（声明与理由见 routes.h 的 LogoutPlan）。
//
// 【为什么"形状非法的 sid"仍然 needs_db】
//   判据是**非空**，不是 is_token_wellformed。logout 对任意 token 都成立：
//   session::destroy 删不掉就是 0 行，那也是成功（幂等）。
//   反过来若在这里加一道形状检查、形状非法就跳过 DB，症状是"浏览器里的坏 Cookie
//   永远清不掉"—— 而清除 Cookie 那头照发，看似生效，实则下次带上同一个坏 Cookie
//   还是老样子。少一次无谓的 DELETE 比多留一个清不掉的 Cookie 划算。
LogoutPlan plan_logout(const std::string& cookie_header) {
  LogoutPlan plan;
  const std::optional<std::string> sid = parse_sid(cookie_header);
  if (!sid.has_value() || sid->empty()) return plan;   // 未登录，不碰 DB（[D-21]）
  plan.needs_db = true;
  plan.token = *sid;
  return plan;
}

// 收口决策的唯一实现（声明与理由见 routes.h【错误收口的决策】）。
//
// ⚠️ 【两条不能动的规则】
//   ① body_already_written = true ⇒ 一律返回 nullopt，【不看 status】。
//      httplib 的 set_error_handler 对任何 status >= 400 的响应都会回调到我们，
//      我方的 401/403/409/429/500 全在这一类里；一旦这里改回"按 status 收口"，
//      未登录调 auth/me 就会拿到 1004 而不是 1002 —— 本轮真的踩过。
//   ② 未知 status 一律归 1001 而非 1008：httplib 在这些场合造出的 4xx/5xx 都是
//      【协议级】问题（坏请求头、URI/头超限、方法不被接受），客户端据此知道
//      "重发无用"；报 1008 会诱导前端弹"服务器内部错误，请稍后重试"并开始重试。
std::optional<ErrorRouting> error_reply_for(int status, bool body_already_written) {
  if (body_already_written) return std::nullopt;

  switch (status) {
    case 404:
      // 路由不存在，或静态文件不存在。两者都给 1004：§6.3 里 1004 的字面含义是
      // "资源不存在"，端点不存在是它的近亲；前端只需区分"这个 API 没了"。
      return ErrorRouting{common::fail(common::ApiError::kNotFound), false};
    case 405:
      // 方法不被接受。本项目的 httplib（0.60）不会自己造 405 —— 它对"路径存在但方法
      // 不对"回的是 404，故这个分支目前不可达（与 413 同理，见 API.md §1.7）。
      // 保留它是为了将来换一个会回 405 的版本时行为不变；对前端而言仍是"不存在"。
      return ErrorRouting{common::fail(common::ApiError::kNotFound), false};
    case 413:
      // 请求体超限（例如 §5.8 的代码上限被 httplib 的 payload_max_length 抢先拦下）。
      // 报 1001 而不是 1008：这是用户内容太大，不是服务器故障。
      return ErrorRouting{
          common::fail(common::ApiError::kInvalidArgument, "", "请求体超出允许的大小"), false};
    default:
      return ErrorRouting{common::fail(common::ApiError::kInvalidArgument), true};
  }
}

bool register_routes(httplib::Server& server, const ServerOptions& options) {
  server.set_error_handler(handle_error);
  server.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                  std::exception_ptr ep) { handle_exception(req, res, ep); });

  server.Post("/api/auth/register", handle_register);
  server.Post("/api/auth/login", handle_login);
  server.Post("/api/auth/logout", handle_logout);
  server.Get("/api/auth/me", handle_me);
  server.Post("/api/auth/password", handle_change_password);
  server.Delete("/api/auth/account", handle_delete_account);

  // 题目组（匿名可读；/favorite 必须登录，由处理器自己判）
  server.Get("/api/tags", handle_tags);
  server.Get("/api/problems", handle_problem_list);
  // {slug} 写在 /favorite 之后：httplib 按注册顺序匹配，
  // `[^/]+` 不会吃掉带斜杠的尾巴，但显式顺序让读代码的人不必推演这一点。
  server.Post(R"(/api/problems/([^/]+)/favorite)", handle_favorite);
  server.Get(R"(/api/problems/([^/]+))", handle_problem_detail);

  // 提交组。id 段收窄成 [0-9]+ 是有意的：/api/submissions/diff 因此不会被
  // /api/submissions/{id} 抢走（"diff" 含非数字），顺序也就不必成为正确性的一部分。
  server.Post("/api/submissions", handle_submission_create);
  server.Get("/api/submissions", handle_submission_list);
  server.Get("/api/submissions/diff", handle_submission_diff);
  server.Get(R"(/api/submissions/([0-9]+))", handle_submission_detail);
  server.Delete(R"(/api/submissions/([0-9]+))", handle_submission_delete);
  server.Post(R"(/api/submissions/([0-9]+)/retry)", handle_submission_retry);

  // 统计组与 admin
  server.Get("/api/stats", handle_stats);
  server.Get("/api/me/overview", handle_overview);
  server.Get("/api/admin/overview", handle_admin_overview);

  // 受保护的 HTML 页面：未登录时重定向到登录页 [D-43]
  // 注意：这些处理器必须在 set_mount_point 之前注册，这样才会优先匹配
  server.Get("/history.html", [&options](const httplib::Request& req, httplib::Response& res) {
    bool server_error = false;
    std::optional<AuthUser> user = viewer_of(req, res, &server_error);
    if (server_error) return;  // DB 异常，已在 viewer_of 中处理
    if (!user.has_value()) {
      // 未登录：重定向到登录页，带上当前路径作为 redirect 参数
      // req.path 已是 URL 解码后的路径，直接使用（路径中无特殊字符）
      std::string target = "/login.html?redirect=" + req.path;
      res.set_header("Location", target);
      res.status = 302;
      return;
    }
    // 已登录：手动读取并服务静态文件
    std::string file_path = options.static_dir + "/history.html";
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
      res.status = 404;
      return;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    res.set_content(ss.str(), "text/html; charset=utf-8");
  });

  server.Get("/admin.html", [&options](const httplib::Request& req, httplib::Response& res) {
    bool server_error = false;
    std::optional<AuthUser> user = viewer_of(req, res, &server_error);
    if (server_error) return;  // DB 异常，已在 viewer_of 中处理
    if (!user.has_value()) {
      // 未登录：重定向到登录页，带上当前路径作为 redirect 参数
      // req.path 已是 URL 解码后的路径，直接使用（路径中无特殊字符）
      std::string target = "/login.html?redirect=" + req.path;
      res.set_header("Location", target);
      res.status = 302;
      return;
    }
    // 已登录：手动读取并服务静态文件
    std::string file_path = options.static_dir + "/admin.html";
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
      res.status = 404;
      return;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    res.set_content(ss.str(), "text/html; charset=utf-8");
  });

  // 静态挂载放在【最后】：httplib 优先匹配已注册的路由，
  // 顺序反了也不会被静态文件抢走 /api/*，但显式写在最后能让读代码的人一眼看出
  // "api 是精确匹配的，剩下的路径才是静态"。
  //
  // 【Cache-Control: no-cache 而不是 max-age=3600】
  //   no-cache 的含义是"每次都要先向服务器确认"，不是"不许缓存"：
  //   httplib 的静态处理器会比对 If-Modified-Since，未修改就回 304 空响应，
  //   所以流量几乎不变，但**改动一定立刻可见**。
  //   为什么必须有这一行：本项目的前端是原生 ES Module，改一个 .js 就要改所有
  //   引它的页面；一旦浏览器缓存住旧模块，症状是
  //   「The requested module './api.js' does not provide an export named 'xxx'」
  //   或「Unexpected identifier at vendor/xxx.js:1」—— 报错指向的却是**没改过的那个文件**，
  //   而磁盘上的文件是对的。这类"代码明明是对的却跑不起来"的排查成本极高，
  //   用一行响应头根除比让人学会清缓存划算得多（§9 M0 的实测教训）。
  if (!server.set_mount_point("/", options.static_dir,
                              {{"Cache-Control", "no-cache"}})) {
    std::fprintf(stderr,
                 "[oj] 静态目录不可用：%s（相对当前工作目录，请从项目根目录启动）\n",
                 options.static_dir.c_str());
    return false;
  }
  return true;
}

bool listen_and_serve(const ServerOptions& options) {
  httplib::Server server;
  if (!register_routes(server, options)) return false;

  g_server = &server;
  const bool listening = server.listen(options.listen_host, options.listen_port);
  g_server = nullptr;
  return listening;
}

void stop_server() {
  if (g_server != nullptr) g_server->stop();
}

}  // namespace web
}  // namespace oj
