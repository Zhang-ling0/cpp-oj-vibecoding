// =============================================================================
// ApiError 的三个取值函数（include/common/errors.h 的配套）
// 纯查表，无状态，不碰 DB —— 与 config 一样可以任意线程并发调用。
//
// 【2xxx 段在这里的处理方式是本文件唯一"有讲究"的地方】
//   它们在枚举里是为了让码表完整（前端要能按码区分五种结论），但对外【不是错误】：
//   http_status 一律返回 200，message 也写成人可读的结论而非"错误"。
//   映射成 4xx/5xx 会让浏览器/curl 的状态码与业务码打架（[D-29] 要求两者同步，
//   而"同步"的含义就是业务码是判题结果时 HTTP 必须成功）。
// =============================================================================
#include "common/errors.h"

namespace oj {
namespace common {

int code(ApiError error) { return static_cast<int>(error); }

bool ok(ApiError error) { return error == ApiError::kOk; }

int http_status(ApiError error) {
  switch (error) {
    case ApiError::kOk:
      return 200;   // 201 由注册端点自己决定（§6.3：注册成功是 201）
    case ApiError::kInvalidArgument:
      return 400;
    case ApiError::kUnauthenticated:
      return 401;
    case ApiError::kForbidden:
      return 403;
    case ApiError::kNotFound:
      return 404;
    case ApiError::kConflict:
      return 409;
    case ApiError::kTooManyRequests:
      return 429;
    case ApiError::kQuotaExceeded:
      return 429;
    case ApiError::kInternalError:
      return 500;
    case ApiError::kJudgeUnavailable:
      return 503;
    case ApiError::kRegisterClosed:
      return 403;
    case ApiError::kStateNotAllowed:
      return 409;
    // 判题结论：HTTP 200 + data.verdict（§6.3 尾注）
    case ApiError::kVerdictCompileError:
    case ApiError::kVerdictTimeLimit:
    case ApiError::kVerdictMemoryLimit:
    case ApiError::kVerdictWrongAnswer:
    case ApiError::kVerdictRuntimeError:
      return 200;
  }
  return 500;  // 不可达：枚举加了新值而这里没跟上时，宁可回 500 也不回 0（与 admin.cpp 同一条约定）
}

const char* message(ApiError error) {
  switch (error) {
    case ApiError::kOk:
      return "ok";
    case ApiError::kInvalidArgument:
      return "参数校验失败";
    case ApiError::kUnauthenticated:
      return "未登录或会话已过期";
    case ApiError::kForbidden:
      return "无权限";
    case ApiError::kNotFound:
      return "资源不存在";
    case ApiError::kConflict:
      return "用户名已注册";
    case ApiError::kTooManyRequests:
      return "提交过于频繁，请稍后再试";
    case ApiError::kQuotaExceeded:
      return "提交过于频繁，请等待当前评测完成";
    case ApiError::kInternalError:
      return "服务器内部错误";
    case ApiError::kJudgeUnavailable:
      return "判题服务不可用，请稍后再试";
    case ApiError::kRegisterClosed:
      return "注册已关闭";
    case ApiError::kStateNotAllowed:
      return "当前状态不允许该操作";
    case ApiError::kVerdictCompileError:
      return "编译错误";
    case ApiError::kVerdictTimeLimit:
      return "运行超时";
    case ApiError::kVerdictMemoryLimit:
      return "内存超限";
    case ApiError::kVerdictWrongAnswer:
      return "答案错误";
    case ApiError::kVerdictRuntimeError:
      return "运行错误";
  }
  return "服务器内部错误";
}

}  // namespace common
}  // namespace oj
