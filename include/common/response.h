// =============================================================================
// CppOJ 统一响应包络 —— §6.3 的 `{code, message, data}` 唯一拼装处
// 对应任务：SPEC.md §9 M0 T0.4 之一（common/response.h）
// 规格来源：§6.3 统一响应包络 / [D-29]（数字业务码 + 同步 HTTP 状态码）
//           / §10.1 AC-1（1005 / 1001 / 1002 的 HTTP 状态必须同步）
//
// 【这一层只做「拼字符串」，不碰 DB、不碰路由】
//   上游是业务层（web/auth_api、web/problem_api …）给的结构体或 nlohmann::json，
//   下游是 web/routes.cpp 给 httplib::Response 填内容。于是：
//     · 业务层返回 AuthFailure{code, field, reason}，路由层不必知道"1001 要带 field"
//     · 路由层不必知道"2xxx 一律映射 200"（那是 errors.h 的 http_status）
//   两边都只依赖本文件，故"包络长什么样"在这一个进程里只有一处实现。
//
// 【为什么 data 永远是对象、且失败时也带 data】
//   §6.3 的示例里失败包络带 `data.field` / `data.reason`。前端 api.js 因此可以
//   无条件读 `data.field`，不必在每个调用点先判断 data 是不是 null。
//   成功且无内容时给 `{}` 而不是省略 —— 省略会让 "data ?? {}" 这种兜底散到前端各处。
//
// 【HTTP 状态码一律转调 errors.h::http_status，不在本文件另存一份映射】
//   [D-29] 要求业务码与 HTTP 状态码同步。两份映射表迟早会有一处对不上，
//   而这类漂移不会崩，只会让浏览器/curl 拿到与业务码矛盾的状态。
//
// 【唯一的例外：注册成功是 201】
//   §6.3 端点表写死 `POST /api/auth/register → 201`，而 errors.h 的 kOk 是 200。
//   故另给 created()，而不是给 http_status 加"看端点决定"的参数 ——
//   那会把"哪个端点返回 201"的决定权散到 20 个端点里。
// =============================================================================
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "common/errors.h"

namespace httplib {
struct Response;
}

namespace oj {
namespace common {

// 一次 HTTP 响应的【内容】。status 与 body 分开携带，而不是直接返回 httplib::Response：
//   · 业务层与单测不需要知道 httplib 的存在（与 web/* 的分层一致）；
//   · 路由层要在【填完包络之后】再决定要不要追加 Set-Cookie 等额外头，
//     那一步需要拿到中间态。
struct Reply {
  int status = 200;
  std::string body;
};

// ---- 成功 ----------------------------------------------------------------------

// HTTP 200。data 默认为空对象。
Reply ok(const nlohmann::json& data = nlohmann::json::object());

// HTTP 201 —— 目前只有注册端点用它（§6.3 端点表）。
Reply created(const nlohmann::json& data = nlohmann::json::object());

// ---- 失败 ----------------------------------------------------------------------

// 只带错误码与默认文案：`{"code":1002,"message":"未登录","data":{}}`
Reply fail(ApiError error);

// 1001 的字段级形态（§6.3 的失败包络示例）：
//   `{"code":1001,"message":"参数校验失败","data":{"field":"password","reason":"长度不足 6 位"}}`
//
// ⚠️ 【field 与 reason 是两个各自独立的可选键，不是"有 field 才带 reason"】
//   四种形态：
//     fail(err)                          → data {}
//     fail(err, "password", "")          → data {"field":"password"}
//     fail(err, "", "注册已关闭")          → data {"reason":"注册已关闭"}
//     fail(err, "password", "长度不足…")   → data {"field":…,"reason":…}
//   两个键【都空】时才退化成 fail(err)。
//   为何不耦合：调用方真的会遇到"没有 field 但有 reason"——
//   routes.cpp 的 send_failure 把 AuthFailure 的两个字段原样递进来，而服务端原因
//   （DB 报错摘要）恰恰只填 reason 不填 field。耦合起来的后果是**那段摘要被静默丢弃**，
//   用户只会看到"服务器内部错误"而拿不到任何线索。
//   为何各自可选：前端按 `data.field` 决定把红字挂到哪个输入框下，而 `"field":""`
//   会让它两处都不挂、又都像挂了；`data.reason || message` 的兜底里空串是真值，
//   会把 message 那句人可读说明覆盖掉。故两者都是"有值才写键"。
Reply fail(ApiError error, std::string field, std::string reason);

// data 由调用方自给（例如注销端点的 `{"message":"账号已注销"}`）。data 为空对象时
// 同样退化成 fail(error)，保证"data 永远是对象"这条不变量没有例外。
Reply fail_with_data(ApiError error, const nlohmann::json& data);

// ---- 落到 httplib ---------------------------------------------------------------

// 把 Reply 的 body 以 application/json 写进响应。**不碰 status** ——
// 路由层可能已经给过状态（例如静态文件 404 走 error handler），由它自己决定。
void write_body(const Reply& reply, httplib::Response& response);

}  // namespace common
}  // namespace oj
