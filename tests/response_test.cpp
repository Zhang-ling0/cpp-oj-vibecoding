// =============================================================================
// 统一响应包络 + 错误码表的单测（SPEC §9 M0 T0.4 之一：common/response.h + errors.h）
// 规格来源：§6.3 统一响应包络与错误码表 / [D-29] 数字业务码 + 同步 HTTP 状态码
//           / [D-37] 1010 注册已关闭 / [D-58] 1011 状态不允许
//           / §10.1 AC-1（1005 / 1001 / 1002 的 HTTP 状态必须同步）
//
// 【这一组用例为什么值得存在：包络是【全部 20 个端点】的唯一出口】
//   任何一处端点写错包络，症状都不会出现在那一个端点上 —— 它出现在
//   "所有页面同时不认识错误"或"curl 拿到的状态码与业务码矛盾"这种全局层面，
//   而全局层面的问题最难定位到某一处代码。相比之下 bcrypt 的错误会立刻
//   让"注册"这一件事坏掉 —— 可见度高得多。
//
// 【本文件钉的四组断言，按"错了会不会被发现"排序】
//   ① 逐字形状（键名与键序）—— 前端 api.js 按这三个键读，改名即全站失效
//   ② 【两条退化分支】—— 本轮刻意设计的行为，不是兜底，随手"简化"掉不会有人报错
//   ③ 不变量（data 永远是对象；reason 必被转义）—— 违反时是一段非法 JSON，
//      前端 JSON.parse 抛异常 → 整个页面白屏，报错信息里【看不出是后端的锅】
//   ④ 码表逐行核对 + 2xxx 一律 200 —— [D-29]，漂移了不会崩，只是 curl 不好用
//
// 【刻意不测的东西】
//   · httplib::Response 上的 Content-Type 覆盖（write_body 的行为）：需要一个真的
//     httplib::Response，而它只是一个数据容器，端到端已被 curl 验收覆盖；
//   · 任何 DB / 网络：本文件是纯计算，因此【连不上 mysqld 的机器上照跑不误】。
// =============================================================================
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/errors.h"
#include "common/response.h"

namespace oj {
namespace common {
namespace {

// 解析包络。带一条断言而不是让每个用例各写一遍 try/catch：
// "输出不是合法 JSON" 本身就是本文件要拦的缺陷之一。
nlohmann::json parse_body(const Reply& reply) {
  nlohmann::json out;
  EXPECT_TRUE(out.is_null());  // parse 前的初始态
  const bool ok_json = [&] {
    try {
      out = nlohmann::json::parse(reply.body);
      return true;
    } catch (const std::exception& e) {
      ADD_FAILURE() << "响应体不是合法 JSON：" << e.what() << "  body=" << reply.body;
      return false;
    }
  }();
  EXPECT_TRUE(ok_json);
  return out;
}

}  // namespace

// =============================================================================
// ResponseEnvelope —— §6.3 的包络形状与 HTTP 状态
// =============================================================================

TEST(ResponseEnvelope, SuccessUsesTheExactBytesOfSection63) {
  const Reply reply = ok();
  // ⚠️ 刻意断言【字符串相等】而不是 JSON 解析后比较：键名或键序一旦漂移，
  //   只比 JSON 相等是抓不到的（JSON 对象无序）。而键名是契约 ——
  //   前端 api.js 的 `typeof payload.code !== 'number'` 就是按它写的。
  //
  //   ⚠️ 注意字节序与 §6.3 的示例【不同】：示例是 code → message → data，
  //   实际是 code → data → message。原因是 nlohmann 的 json 用 std::map 存对象，
  //   输出按字典序。早先 response.cpp 的注释写的是"按插入序"，被本条抓了出来。
  //   不为此改用 ordered_json 或手工拼串 —— 那要么换掉全项目共用的 json 类型，
  //   要么把转义责任接回包络层（本文件用 nlohmann 正是为了不做这件事）。
  EXPECT_EQ(reply.body, R"({"code":0,"data":{},"message":"ok"})");
  EXPECT_EQ(reply.status, 200);
}

TEST(ResponseEnvelope, SuccessCarriesTheGivenDataObject) {
  nlohmann::json data;
  data["userId"] = 42;
  data["username"] = "alice";
  data["role"] = "user";

  const Reply reply = ok(data);
  EXPECT_EQ(reply.status, 200);
  const nlohmann::json body = parse_body(reply);
  EXPECT_EQ(body["code"], 0);
  EXPECT_EQ(body["message"], "ok");
  EXPECT_EQ(body["data"]["userId"], 42);
  EXPECT_EQ(body["data"]["username"], "alice");
  EXPECT_EQ(body["data"]["role"], "user");
}

// §6.3 端点表：注册成功是 201。而 errors.h 的 kOk 是 200 ——
// 两者不同是刻意的（"哪个端点返回 201"的决定权不许散到 20 个端点里），
// 因此这条差异必须被钉住：哪天真给 kOk 的 http_status 加了"看端点决定"的逻辑，
// 最先变红的是这里。
TEST(ResponseEnvelope, CreatedUses201WhileSuccessUses200) {
  EXPECT_EQ(created().status, 201);
  EXPECT_EQ(ok().status, 200);
  EXPECT_EQ(created().body, ok().body);  // 状态不同，【包络本身完全一致】
}

// [D-29]：业务码与 HTTP 状态码同步。这张表逐行抄自 §6.3，
// 而它抄自 SPEC 而不是抄自代码 —— 一旦代码侧改了而这里没改，红的是这条。
TEST(ResponseEnvelope, EveryFailureMapsToTheHttpStatusOfSection63) {
  const std::vector<std::pair<ApiError, int>> expected = {
      {ApiError::kInvalidArgument, 400},      // 1001
      {ApiError::kUnauthenticated, 401},      // 1002
      {ApiError::kForbidden, 403},            // 1003
      {ApiError::kNotFound, 404},             // 1004
      {ApiError::kConflict, 409},             // 1005
      {ApiError::kTooManyRequests, 429},      // 1006
      {ApiError::kQuotaExceeded, 429},        // 1007
      {ApiError::kInternalError, 500},        // 1008
      {ApiError::kJudgeUnavailable, 503},     // 1009
      {ApiError::kRegisterClosed, 403},       // 1010 [D-37]
      {ApiError::kStateNotAllowed, 409},      // 1011 [D-58]
  };
  for (const auto& [error, status] : expected) {
    const Reply reply = fail(error);
    EXPECT_EQ(reply.status, status) << "code=" << code(error);
    const nlohmann::json body = parse_body(reply);
    EXPECT_EQ(body["code"], code(error));
    EXPECT_EQ(body["message"], message(error));
  }
}

// =============================================================================
// ResponseFieldErrors —— 1001 的字段级形态与两条【退化分支】
// =============================================================================

TEST(ResponseFieldErrors, FieldAndReasonBothReachTheDataBlock) {
  const Reply reply = fail(ApiError::kInvalidArgument, "password", "长度不足 6 位");
  EXPECT_EQ(reply.status, 400);
  const nlohmann::json body = parse_body(reply);
  EXPECT_EQ(body["code"], 1001);
  EXPECT_EQ(body["data"]["field"], "password");
  EXPECT_EQ(body["data"]["reason"], "长度不足 6 位");
}

// 【退化分支 ①】field 为空 → 不产出 field 键，【但 reason 必须留下】。
//   为什么不是"field 为空就整体退化"：调用方真的会遇到"只有 reason 没有 field"——
//   routes.cpp 的 send_failure 把 AuthFailure 的两个字段原样递进来，而服务端原因
//   （DB 报错摘要）恰恰只填 reason。耦合起来的后果是【那段摘要被静默丢弃】，
//   用户只看到"服务器内部错误"而拿不到任何线索。
//   而 routes.cpp 的 413 分支就走的这条路径（field 空 + reason = "请求体超出允许的大小"）——
//   本条用例写出来之前，那句文案从未真正到达过客户端。
//   为何 field 要省略而不是写空串：前端按 `data.field` 决定把红字挂到哪个输入框下，
//   `"field":""` 会让它两处都不挂、又都像挂了。
TEST(ResponseFieldErrors, AnEmptyFieldOmitsTheKeyButKeepsTheReason) {
  const Reply reply = fail(ApiError::kRegisterClosed, "", "本实例已关闭注册");
  const nlohmann::json body = parse_body(reply);
  EXPECT_FALSE(body["data"].contains("field"))
      << "field 为空时不该留下空串键；body=" << reply.body;
  EXPECT_EQ(body["data"]["reason"], "本实例已关闭注册");
}

// 两个键都空 → 退化成只带码的形态（与 fail(err) 的字节完全一致）
TEST(ResponseFieldErrors, BothKeysEmptyDegradesToTheCodelessShape) {
  EXPECT_EQ(fail(ApiError::kStateNotAllowed, "", "").body,
            fail(ApiError::kStateNotAllowed).body);
}

// 【退化分支 ②】reason 为空 → 不产出该键。
//   前端的兜底是 `data.reason || message`；一个空的 reason 会【覆盖掉】message
//   那句人可读的说明（"" 在 JS 里是真值），于是响应里既没有 reason 也没有可读文案。
TEST(ResponseFieldErrors, AnEmptyReasonIsOmittedSoThatMessageStillReadsBack) {
  const Reply reply = fail(ApiError::kInvalidArgument, "newPassword", "");
  const nlohmann::json body = parse_body(reply);
  EXPECT_EQ(body["data"]["field"], "newPassword");
  EXPECT_FALSE(body["data"].contains("reason")) << "body=" << reply.body;
  EXPECT_EQ(body["message"], message(ApiError::kInvalidArgument));
  EXPECT_NE(body["message"], "");  // 兜底文案必须非空，否则前端兜到空串
}

// fail_with_data 只在【非空对象】时才带上 data；其余一律退化成 fail(error)。
// 理由同响应包络的"data 永远是对象"不变量：形状统一，前端不必处处写 `data ?? {}`。
TEST(ResponseFieldErrors, FailWithDataDegradesOnEmptyOrNonObjectPayloads) {
  const Reply from_empty = fail_with_data(ApiError::kStateNotAllowed, nlohmann::json::object());
  EXPECT_EQ(from_empty.body, fail(ApiError::kStateNotAllowed).body);

  const Reply from_array = fail_with_data(ApiError::kStateNotAllowed, nlohmann::json::array());
  EXPECT_EQ(from_array.body, fail(ApiError::kStateNotAllowed).body);

  const Reply from_text = fail_with_data(ApiError::kStateNotAllowed, nlohmann::json("账号已注销"));
  EXPECT_EQ(from_text.body, fail(ApiError::kStateNotAllowed).body);
}

TEST(ResponseFieldErrors, FailWithDataKeepsARealMessage) {
  nlohmann::json data;
  data["message"] = "账号已注销";
  const Reply reply = fail_with_data(ApiError::kOk, data);
  const nlohmann::json body = parse_body(reply);
  EXPECT_EQ(body["data"]["message"], "账号已注销");
}

// =============================================================================
// ResponseInvariants —— 违反时症状最隐蔽的两条
// =============================================================================

// 不变量：`data` 永远是对象。
//   违反时前端 `data.field` 在数组上是 undefined、在字符串上是某字符下标，
//   **不抛异常** —— 于是前端照常渲染出一个"没有错误提示"的表单。
//   把这些形状显式喂进去，是为了让"调用方误传了"在单测里就变红。
TEST(ResponseInvariants, DataIsAlwaysAnObjectWhateverTheCallerPasses) {
  const std::vector<nlohmann::json> not_objects = {
      nlohmann::json::array({1, 2, 3}),
      nlohmann::json::array(),
      nlohmann::json("账号已注销"),
      nlohmann::json(42),
      nlohmann::json(true),
  };
  for (const nlohmann::json& payload : not_objects) {
    EXPECT_TRUE(parse_body(ok(payload))["data"].is_object())
        << "传入 " << payload.type_name() << " 后 data 不再是对象";
    EXPECT_TRUE(parse_body(fail_with_data(ApiError::kInternalError, payload))["data"].is_object())
        << "传入 " << payload.type_name() << " 后 data 不再是对象";
  }
  // null 单独测：json() 默认构造即为 null，漏测它就漏掉了最常见的默认参数
  EXPECT_TRUE(parse_body(ok(nlohmann::json()))["data"].is_object());
}

// 不变量：reason / message 里的一切字符都必须被转义。
//   这是"手工拼 JSON"的死穴：reason 里有引号 → 响应体不是合法 JSON →
//   前端 JSON.parse 抛异常 → **整页白屏**，而报错信息里完全看不出是后端的锅。
//   登录/注册的字段文案是本项目里最容易带上引号/换行的地方（形如 `长度不足 6 位`
//   与直接回显用户输入的几种场景）。
TEST(ResponseInvariants, TextIsJsonEscapedSoTheBodyAlwaysParses) {
  const std::string hostile = "名字里有\"引号\"和\\反斜杠，还有换行\n与制表\t，"
                              "以及 <script>alert(1)</script> 与中文";
  const Reply reply = fail(ApiError::kInvalidArgument, "username", hostile);
  const nlohmann::json body = parse_body(reply);
  EXPECT_EQ(body["data"]["reason"], hostile);  // 解析回来必须逐字相同
  EXPECT_EQ(body["data"]["field"], "username");
  // 反向钉一次：裸的引号个数必须是偶数（JSON 结构的那些），
  // 而 hostile 里的 2 个引号已被转义成 \"，故原文的 " 不应出现在包络里
  EXPECT_EQ(reply.body.find(hostile), std::string::npos)
      << "响应体里出现了未经转义的原文";
}

// 键名是契约：§6.3 与前端 api.js 都按 code/message/data 三个键读。
// 少一个键会让 api.js 的 `typeof payload.code !== 'number'` 判定失败，
// 于是【全部端点】一起变成 "响应包络格式不正确"。
TEST(ResponseInvariants, TheEnvelopeAlwaysCarriesExactlyTheThreeContractKeys) {
  for (const Reply& reply : {ok(), created(), fail(ApiError::kInternalError),
                             fail(ApiError::kInvalidArgument, "username", "太短")}) {
    const nlohmann::json body = parse_body(reply);
    EXPECT_TRUE(body.contains("code")) << body.dump();
    EXPECT_TRUE(body.contains("message")) << body.dump();
    EXPECT_TRUE(body.contains("data")) << body.dump();
    EXPECT_EQ(body.size(), 3) << "包络多出了契约之外的键：" << body.dump();
  }
}

// =============================================================================
// ResponseErrorTable —— §6.3 码表逐行
// =============================================================================

TEST(ResponseErrorTable, EveryCodeMatchesTheTableOfSection63) {
  EXPECT_EQ(code(ApiError::kOk), 0);
  EXPECT_EQ(code(ApiError::kInvalidArgument), 1001);
  EXPECT_EQ(code(ApiError::kUnauthenticated), 1002);
  EXPECT_EQ(code(ApiError::kForbidden), 1003);
  EXPECT_EQ(code(ApiError::kNotFound), 1004);
  EXPECT_EQ(code(ApiError::kConflict), 1005);
  EXPECT_EQ(code(ApiError::kTooManyRequests), 1006);
  EXPECT_EQ(code(ApiError::kQuotaExceeded), 1007);
  EXPECT_EQ(code(ApiError::kInternalError), 1008);
  EXPECT_EQ(code(ApiError::kJudgeUnavailable), 1009);
  EXPECT_EQ(code(ApiError::kRegisterClosed), 1010);
  EXPECT_EQ(code(ApiError::kStateNotAllowed), 1011);
  EXPECT_EQ(code(ApiError::kVerdictCompileError), 2001);
  EXPECT_EQ(code(ApiError::kVerdictTimeLimit), 2002);
  EXPECT_EQ(code(ApiError::kVerdictMemoryLimit), 2003);
  EXPECT_EQ(code(ApiError::kVerdictWrongAnswer), 2004);
  EXPECT_EQ(code(ApiError::kVerdictRuntimeError), 2005);
}

// [D-29] 最关键的一条：2xxx 段【不是 API 错误】。
//   它们是判题业务结果（CE/TLE/MLE/WA/RE），走 HTTP 200 + data.verdict 表达。
//   映射成 4xx/5xx 会让浏览器/curl 的状态码与业务码打架，而"同步"的含义
//   恰恰是业务码是判题结果时 HTTP 必须成功。
TEST(ResponseErrorTable, TheTwoThousandRangeAlwaysMapsTo200) {
  const std::vector<ApiError> verdicts = {
      ApiError::kVerdictCompileError, ApiError::kVerdictTimeLimit,
      ApiError::kVerdictMemoryLimit,  ApiError::kVerdictWrongAnswer,
      ApiError::kVerdictRuntimeError,
  };
  for (const ApiError error : verdicts) {
    EXPECT_EQ(http_status(error), 200) << "code=" << code(error);
    // 同时钉住"它仍在同一个枚举里"——§6.3 的表要求前端能按码区分五种结论
    EXPECT_GE(code(error), 2001) << code(error);
  }
}

TEST(ResponseErrorTable, EveryCodeHasAMessageAndAKindHttpStatus) {
  const std::vector<ApiError> all = {
      ApiError::kOk,
      ApiError::kInvalidArgument, ApiError::kUnauthenticated, ApiError::kForbidden,
      ApiError::kNotFound, ApiError::kConflict, ApiError::kTooManyRequests,
      ApiError::kQuotaExceeded, ApiError::kInternalError, ApiError::kJudgeUnavailable,
      ApiError::kRegisterClosed, ApiError::kStateNotAllowed,
      ApiError::kVerdictCompileError, ApiError::kVerdictTimeLimit,
      ApiError::kVerdictMemoryLimit, ApiError::kVerdictWrongAnswer,
      ApiError::kVerdictRuntimeError,
  };
  for (const ApiError error : all) {
    EXPECT_NE(message(error), nullptr);
    EXPECT_NE(std::string(message(error)), "") << "code=" << code(error);
    EXPECT_GE(http_status(error), 200) << "code=" << code(error);
    EXPECT_LE(http_status(error), 599) << "code=" << code(error);
  }
}

TEST(ResponseErrorTable, OnlyOkIsOk) {
  // ok() 的语义载体：调用点常写 `if (common::ok(e))`，若某个错误码也返回 true，
  // 那一次 if 会把失败当成功放行 —— 所以这条看起来废话，值得占一行断言。
  EXPECT_TRUE(ok(ApiError::kOk));
  for (const ApiError error :
       {ApiError::kInvalidArgument, ApiError::kUnauthenticated, ApiError::kForbidden,
        ApiError::kInternalError, ApiError::kRegisterClosed, ApiError::kStateNotAllowed}) {
    EXPECT_FALSE(ok(error)) << "code=" << code(error);
  }
}

}  // namespace common
}  // namespace oj
