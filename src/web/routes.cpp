// =============================================================================
// 路由接线层（include/web/routes.h 的配套）
// 见该头文件顶部对分层边界的说明。此处只把 §6.3 认证组的四个端点接起来：
//   POST /api/auth/register / login / logout，GET /api/auth/me
// 其余端点（题目组 T3.6、提交组 T3.7/T3.8/T3.15…、admin T3.12、改密 T3.13、
// 注销 T3.18、限流 T3.4）在各自业务层就绪后陆续【追加】到同一个 register_routes 里 ——
// 不做"路由表"抽象：四个端点的形状差异（要不要租约、要不要 Set-Cookie、
// 失败码由谁给）大到一张表反而会多出一层与业务层重复的映射。
// =============================================================================

// 必须排在 httplib.h 之前：线程池大小是 include 期展开的宏，不是运行期配置。
// §2.3 的 N=8。理由见 routes.h【线程池大小为什么写死 8】。
#define CPPHTTPLIB_THREAD_POOL_COUNT 8

#include "web/routes.h"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/errors.h"
#include "common/log.h"
#include "common/response.h"
#include "db/mysql_pool.h"
#include "web/auth_api.h"
#include "web/session.h"

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
void handle_logout(const httplib::Request& req, httplib::Response& res) {
  std::string token;
  if (req.has_header("Cookie")) {
    const std::optional<std::string> sid = parse_sid(req.get_header_value("Cookie"));
    if (sid.has_value()) token = *sid;
  }

  std::string clear_cookie;
  if (!token.empty()) {
    db::Lease lease = db::Pool::instance().acquire();
    if (!lease.valid()) {
      send_pool_unavailable(req, res);
      return;
    }
    std::string error;
    if (!logout_user(lease.session(), token, &clear_cookie, &error)) {
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
      // 方法不被接受。httplib 的兜底错误处理器会对"路径存在但方法不对"回 405；
      // 对前端而言这仍然是"你要用的东西不存在"，故与 404 同码。
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

  // 静态挂载放在【最后】：httplib 优先匹配已注册的路由，
  // 顺序反了也不会被静态文件抢走 /api/*，但显式写在最后能让读代码的人一眼看出
  // "api 是精确匹配的，剩下的路径才是静态"。
  if (!server.set_mount_point("/", options.static_dir)) {
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
