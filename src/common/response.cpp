// =============================================================================
// 统一响应包络的实现（include/common/response.h 的配套）
// 纯拼装：无状态、不碰 DB、不碰 httplib 的请求生命周期 —— 因此任意线程可并发调用。
//
// 【为什么直接 dump 整个 json 对象，而不是手工拼字符串】
//   手工拼 JSON 时，字符串里的 `"`、`\`、控制字符必须自己转义；漏一个就是
//   "用户名里带引号 → 响应体不是合法 JSON → 前端 JSON.parse 抛异常 → 用户看到
//   一个空白页"。而本项目的用户名允许 `[A-Za-z0-9_-]`（auth_api.h 文件头），
//   题面/理由文案则不受此限 —— 也就是说"逃逸"这一层迟早会被某个端点需要。
//   交给 nlohmann 就没有这一类 bug，且代价只有一次 dump 的时间。
// =============================================================================
#include "common/response.h"

#include <httplib.h>

namespace oj {
namespace common {
namespace {

// 包络里 code / message / data 的键名。集中在这里而不是散在四个函数里：
// 键名是契约的一部分（前端 api.js 与 §6.3 都按这三个键写），
// 写成局部常量只是让它【看起来】集中，实际仍要看四个函数。
constexpr const char* kKeyCode = "code";
constexpr const char* kKeyMessage = "message";
constexpr const char* kKeyData = "data";

nlohmann::json envelope(ApiError error, const nlohmann::json& data) {
  nlohmann::json out;
  // ⚠️ 键【不会】按插入序输出：nlohmann 的 json 用 std::map 存对象，实际字节序是
  //   code → data → message（字典序），而 §6.3 的示例写的是 code → message → data。
  //   早先本文件的注释写成"按插入序"，是【错的】—— tests/response_test.cpp 的
  //   SuccessUsesTheExactBytesOfSection63 逐字节断言把它抓了出来。
  //   不为此改用 ordered_json 或手工拼串：那要么换掉全项目共用的 json 类型，
  //   要么把转义责任接回本文件（正是本文件选择 nlohmann 的原因，见文件头）。
  //   字典序对机器与前端都无影响（JSON 对象无序），故照实写在这里。
  out[kKeyCode] = code(error);
  out[kKeyMessage] = message(error);
  out[kKeyData] = data.is_object() ? data : nlohmann::json::object();
  return out;
}

std::string dump(const nlohmann::json& value) { return value.dump(); }

}  // namespace

Reply ok(const nlohmann::json& data) {
  return Reply{200, dump(envelope(ApiError::kOk, data))};
}

Reply created(const nlohmann::json& data) {
  return Reply{201, dump(envelope(ApiError::kOk, data))};
}

Reply fail(ApiError error) {
  return Reply{http_status(error), dump(envelope(error, nlohmann::json::object()))};
}

Reply fail(ApiError error, std::string field, std::string reason) {
  // 两个键各自可选（理由见 response.h 的说明）—— ⚠️ 不要把它改回
  // "field 为空就整体退化"，那会把服务端原因的摘要一并丢掉，
  // 而 routes.cpp 的 413 分支与 send_failure 都会命中那条路径。
  nlohmann::json data;
  if (!field.empty()) data["field"] = std::move(field);
  if (!reason.empty()) data["reason"] = std::move(reason);
  if (data.empty()) return fail(error);
  return Reply{http_status(error), dump(envelope(error, data))};
}

Reply fail_with_data(ApiError error, const nlohmann::json& data) {
  if (!data.is_object() || data.empty()) return fail(error);
  return Reply{http_status(error), dump(envelope(error, data))};
}

void write_body(const Reply& reply, httplib::Response& response) {
  // Content-Type 必须由本函数【无条件】覆盖：静态文件 404 会走 httplib 的
  // error handler，那条路径上响应里可能已经带着 "text/html"，若不覆盖，
  // 前端拿到 `Content-Type: text/html` 却收到一段 JSON 包络 ——
  // fetch 本身不拦，但任何按 content-type 分流的工具/扩展会误判。
  response.set_content(reply.body, "application/json; charset=utf-8");
}

}  // namespace common
}  // namespace oj
