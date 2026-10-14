// =============================================================================
// CppOJ 业务错误码 —— §6.3 错误码表的【唯一真源】
// 对应任务：SPEC.md §9 M0 T0.4 之一（common/errors.h）
// 规格来源：§6.3 统一响应包络 + 错误码表 / [D-29]（数字业务码 + 同步 HTTP 状态码）
//           / §10.1 AC-1（注册登录）
//
// 【为什么单独一个文件，而不是各模块各写一份】
//   §6.3 的表把三件事绑在一起：业务码、HTTP 状态码、中文文案。三者任何一处各写一份，
//   就会出现"401 的地方报了 403""同一码在两处文案不同"这类漂移 —— 它不会崩，
//   只会在前端表现为"错误提示不对"。因此集中在这里，取值处（auth_api / admin /
//   将来 T3.5 的 routes）只引这一个头。
//
// 【2xxx 段不是 API 错误】
//   2001~2005 是【判题业务结果】（CE/TLE/MLE/WA/RE），走 HTTP 200 + data.verdict 表达，
//   不是异常。放在同一个枚举里只是为了让"码表"完整，路由层【不得】把它们映射成 4xx/5xx
//   （§6.3 的约定原文）。
//
// 【与 web/admin.h 的既有关系】
//   admin.h（T3.12，早于本文件落地）自带 error_code_of/error_message_of 两个小函数，
//   它的单测锁定了这两个函数的返回值。接线 T3.5 统一包络时应让它们转调本文件，
//   而不是保留第二份映射 —— 在那之前两边对同一码给出同样的结果，故不构成不一致。
// =============================================================================
#pragma once

namespace oj {
namespace common {

// §6.3 错误码表。数值即响应包络里的 code。
enum class ApiError : int {
  kOk = 0,                  // 成功（HTTP 200/201）

  // ---- 1xxx：API 层异常 ----
  kInvalidArgument = 1001,  // 400 参数校验失败（用户名/密码长度、slug 格式、代码超长）
  kUnauthenticated = 1002,  // 401 未登录或会话过期
  kForbidden = 1003,        // 403 无权限（访问他人提交；非 admin 访问 /api/admin/*）
  kNotFound = 1004,         // 404 资源不存在
  kConflict = 1005,         // 409 冲突（用户名已注册）
  kTooManyRequests = 1006,  // 429 请求过于频繁（每分钟提交数超限）
  kQuotaExceeded = 1007,    // 429 配额超限（单用户同时评测数超限）
  kInternalError = 1008,    // 500 服务器内部错误（DB 异常、未捕获异常）
  kJudgeUnavailable = 1009, // 503 判题服务不可用
  kRegisterClosed = 1010,   // 403 注册已关闭（OJ_ALLOW_REGISTER=0）
  kStateNotAllowed = 1011,  // 409 当前状态不允许（重跑非 failed/JE 的提交；未登录点提交/收藏）

  // ---- 2xxx：判题业务结果，HTTP 仍为 200（§6.3 约定，不作异常处理）----
  kVerdictCompileError = 2001,
  kVerdictTimeLimit = 2002,
  kVerdictMemoryLimit = 2003,
  kVerdictWrongAnswer = 2004,
  kVerdictRuntimeError = 2005,
};

// 业务码（§6.3 的 code 字段）
int code(ApiError error);

// [D-29]：与业务码【同步】的 HTTP 状态码。2xxx 段一律 200 —— 它们不是 API 异常。
int http_status(ApiError error);

// 默认中文文案。失败响应包络的 message 字段直接用它（§6.3 的示例）。
const char* message(ApiError error);

// 便于 if (error == ApiError::kOk) 之类写法
bool ok(ApiError error);

}  // namespace common
}  // namespace oj
