// =============================================================================
// CppOJ HTTP 路由 —— httplib 起服务 + 认证四端点的接线层
// 对应任务：SPEC.md §9 M0 T0.5（httplib 起服务，静态文件映射跑通 list.html）
//           + M3 T3.5 的【认证组】四个端点（§6.3）
// 规格来源：§2.3 线程模型（httplib 线程池 N=8 + DB mutex 串行化）
//           / §6.3 端点清单与统一包络 / [D-29] / [D-43] 未登录可读题面
//           / [D-69] CSRF 依赖 SameSite=Lax（§6.3.2.1）
//
// 【这一层的职责边界：只做「HTTP 语法 → 业务层调用 → 包络」三件事】
//   它【不含】任何 SQL、不含任何校验规则、不含任何 bcrypt：
//   注册/登录的四道闸门在 web/auth_api.h，会话签发与回收在 web/session.h，
//   包络在 common/response.h。本文件只是把它们按 §6.3 的端点表【接线】起来。
//   分层的收益是每一层都能单独单测：auth_api_test 不起 HTTP，
//   本文件则只需要 curl 级验收（§9 M3 的两条 curl 验收）。
//
// 【为什么匿名流量连租约都不取（除 /api/auth/* 之外）】
//   [D-43]：未登录可读全部题面。也就是说【匿名流量占多数】。
//   而 [D-21] 把全部 DB 访问串行化在一把进程级互斥量上，匿名流量若也去取租约，
//   就会排在登录请求后面等锁 —— 于是"多点几次页面"能把登录拖慢。
//   故静态资源与"我是谁"之外的端点在【本轮】一律不碰 DB；将来 T3.6/T3.7 的
//   题目与提交端点需要读 DB 时，仍走「先解析 Cookie → 有 sid 才取租约」这条
//   已在 web/session.h 里定好的口径（authenticate() 的便捷重载正是这个顺序）。
//
// 【线程池大小为什么写死 8 而不是读 config().http_threads】
//   CPPHTTPLIB_THREAD_POOL_COUNT 是【编译期】宏（httplib.h 在 include 时展开），
//   而 http_threads 是运行期配置。两者在这里恰好都是 8（§2.3 的 N=8、
//   Config::http_threads 默认值 8），但它们【不是同一件事】：
//   改了宏等于改了每个 HTTP 线程的栈大小（默认 8MB 虚拟），1967MB 的机器上
//   这 8 个线程是 64MB 虚拟地址，不是 64MB 常驻 —— 但也不该随手调大。
//   故：宏按 §2.3 固定 8，config().http_threads 留给 httplib 没有的用途
//   （例如将来给读超时留不同线程池），两者不同步这件事在此写明而不是留成隐含假设。
//
// 【错误与异常的收口只有两处】
//   ① 未匹配的路由 / 静态文件不存在 → set_error_handler → 1004 包络
//      （而不是 httplib 默认的纯文本 404 —— 那会让前端 api.js 解析失败后
//       落到 catch 分支，给用户看"网络错误"这种误导性的提示）
//   ② 处理函数抛异常 → set_exception_handler → 1008 包络 + app.log ERROR
//      判题子进程崩溃拖不垮主进程是 §1.4 的要求，同理一个 handler 抛出的
//      std::bad_alloc 也不能让整个服务 5xx 堆积 —— 单个请求失败即可。
//
// 【Set-Cookie 只在两处出现：登录与登出】
//   登录写 sid（session::make_set_cookie），登出写清空 Cookie
//   （session::make_clear_cookie）。两者必须来自同一对函数：Cookie 的名字/路径/
//   属性三者任一不一致，浏览器就会当成另一个 Cookie 留着，
//   表现是"服务端已登出、刷新又变回登录态"。
// =============================================================================
#pragma once

#include <optional>
#include <string>

#include "common/response.h"

namespace httplib {
class Server;
}

namespace oj {
namespace web {

// ---- 错误收口的决策（纯函数，不碰 httplib 的请求生命周期）--------------------

// 路由收口的【唯一决策点】：给定 httplib 回传的状态码，以及"响应体是否已经被我方写过"，
// 决定是**原样放过**（返回 nullopt）还是换成 §6.3 的统一包络。
//
// 【为什么必须抽成纯函数，而不是留在 handle_error 里】
//   判据一旦与日志、switch、httplib 的类型写在一起，就只能靠 curl 人工比对 ——
//   而本轮真的在这个判据上踩过：早先按 `status == 404` 收口，结果未登录调
//   `GET /api/auth/me` 实测返回 `404 {"code":1004}` 而不是 `401 {"code":1002}`。
//   根因是 httplib 对【任何 status >= 400】的响应都会调 set_error_handler，
//   **包括我们自己端点回的那一发业务错误**。改成"body 为空 ⇒ 这不是我写的响应"之后，
//   规则本身只有一句话，但那句话随时可能被下一个人改回去 —— 它需要一个能被钉住的用例。
//
// 【body_already_written 才是判据，status 只是分流依据】
//   httplib 自己造的 404/400/413 有一个共同且唯一的特征：**body 从未被填过**
//   （见 httplib.h 的 write_response）。我方业务错误的 body 则一定非空（包络 JSON）。
//   于是"是否重写"与"重写成什么"被拆成两件事：前者由调用方传进来，后者在这里按 status 决定。
//
// 【protocol_level 为什么随决策一起返回，而不是让调用方重推一次】
//   它表达的是"这次拒绝**不是**我方业务逻辑的产物"，值得在 app.log 里留一条 WARN。
//   若让调用方自己判断，就得在 routes.cpp 里再写一遍 {404,405,413} 这个集合 ——
//   两处白名单各改一处就分叉，而症状是"改了映射却忘了改 WARN 白名单"这类静默偏差
//   （SPEC §8.1.1 的通则：多数硬漏洞不是"没做"，而是两处各自合理的设计放在一起就矛盾）。
struct ErrorRouting {
  common::Reply reply;
  bool protocol_level = false;  // true ⇒ httplib 协议级拒绝（非我方业务错误）
};

// 返回 nullopt = 【原样放过】，不改写响应体。
// 纯函数：不打日志、不碰 httplib 类型，故任意线程可并发调用，也不需要起服务就能测。
std::optional<ErrorRouting> error_reply_for(int status, bool body_already_written);

// ---- 起服务 -------------------------------------------------------------------

// 起服务需要的全部外部依赖。做成一个可递进的结构体而不是读 config() 全局：
//   · 单测可以指一个临时 static 目录起一个真实的 Server（不起在 0.0.0.0）；
//   · 端口 0 时交给操作系统分配，供本机 curl 验收使用（避免与已在跑的实例抢端口）。
struct ServerOptions {
  std::string static_dir = "./static";
  std::string listen_host = "0.0.0.0";
  int listen_port = 8080;
};

// 把全部路由挂到 server 上（当前只有 §6.3 认证组的四个端点 + 静态挂载）。
// 与 listen 分离，是为了将来加"跑在测试里起一个临时端口"的用法时不必再拆。
// 返回 false 表示静态目录不可用 —— 静态文件是五页前端唯一的来源，
// 缺了它服务起来了也没有任何页面可访问（AC-1 的验收要过浏览器），故此时拒绝启动。
bool register_routes(httplib::Server& server, const ServerOptions& options);

// 阻塞监听。返回 false 表示 bind/listen 失败（端口被占是最常见的一种），
// 此时 stdout 已经打印了人可读原因。
bool listen_and_serve(const ServerOptions& options);

// 请求尚未被处理时由 httplib 调停服务（SIGINT 时 main 用来跳出 listen 阻塞）。
void stop_server();

}  // namespace web
}  // namespace oj
