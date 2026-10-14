// =============================================================================
// 路由收口的单测（SPEC §9 M3 T3.5 之一：web/routes.h 的 error_reply_for + 静态挂载）
// 规格来源：§6.3 端点清单与错误码表 / [D-29] 业务码与 HTTP 状态同步
//           [D-43] 未登录可读题面（匿名流量不是错误）
//
// 【本文件存在的唯一理由：钉住一条【已经被改错过一次】的判据】
//   本轮实踩：早先的 set_error_handler 按 `status == 404` 收口，而 httplib 对
//   【任何 status >= 400】的响应都会回调它 —— 包括我们自己端点回的那一发业务错误。
//   于是未登录调 `GET /api/auth/me` 实测拿到的是
//       404 {"code":1004,"message":"资源不存在"}
//   而不是
//       401 {"code":1002,"message":"未登录或会话已过期"}
//   症状具有极强的欺骗性：前端"看起来"有错误处理（它确实渲染了包络），
//   只是把未登录当成"端点不存在"，于是**顶栏渲染不出未登录形态、也跳不了登录页**。
//   修法是"body 为空 ⇒ 这不是我写的响应"，规则变成一句话。
//   但"一句话"不等于"改不掉" —— 下一个觉得"status == 404 更直观"的人会再改回去。
//   故把它抽成纯函数并用 BelowABodyWeAlreadyWroteIsNeverRewritten 一条钉住。
//
// 【本文件【不】起服务、不 listen、不占端口、不碰 MySQL】
//   error_reply_for 是纯函数；静态挂载只需要一个 httplib::Server 对象
//   （set_mount_point 只做 stat + 记一笔，不 bind）。因此这批用例在
//   没起 mysqld 的机器上、在被别的实例占着 8080 的机器上都照跑不误 ——
//   这一点与 mysql_pool_test / auth_api_test 的真库组刻意不同：
//   那两组没库就得跳过，于是"这台机器上全绿"可能只是全跳过了。
// =============================================================================
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/errors.h"
#include "web/routes.h"

namespace oj {
namespace web {
namespace {

// httplib 会用它自己造出、且 body 为空的那些状态码。
// 集合取自 httplib.h 里出现的 4xx/5xx 常量，外加几个明确【不该】被改动的状态码。
constexpr int kProtocolStatuses[] = {400, 402, 408, 411, 414, 421, 431, 501, 505};
constexpr int kRoutingStatuses[] = {404, 405, 413, 429, 500, 503};

std::vector<int> all_statuses() {
  std::vector<int> all;
  for (int s : kProtocolStatuses) all.push_back(s);
  for (int s : kRoutingStatuses) all.push_back(s);
  return all;
}

// 取包络里的业务码。断言"路由层给出的 HTTP 状态与包络里的码同步"（[D-29]）
// 时用它 —— 只看 status 的话，一个"status 恰好对但码是错的"响应也能通过。
int business_code_of(const ErrorRouting& routing) {
  EXPECT_FALSE(routing.reply.body.empty());
  return nlohmann::json::parse(routing.reply.body).value("code", -1);
}

}  // namespace

// =============================================================================
// ErrorRouting —— 判据（body 是否已被我方写过）
// =============================================================================

// ⭐ 本文件最重要的一条。
//   body_already_written = true ⇒ 一律放过，【与 status 无关】。
//   把 error_reply_for 改回"按 status 收口"，本条【立刻】变红 —— 而且红得很有指向性：
//   它会指出是哪一个 status 被错误地改写了。
TEST(ErrorRouting, BelowABodyWeAlreadyWroteIsNeverRewritten) {
  // 这些正是我方端点真会回的状态码（1002/1003/1005/1006/1007/1008/1009/1011）
  for (const int status : all_statuses()) {
    EXPECT_FALSE(error_reply_for(status, /*body_already_written=*/true).has_value())
        << "status=" << status
        << " 的响应体已经是我写的包络，收口把它改写了 —— "
           "未登录 auth/me 会因此从 1002 变成 1004（AC-1 的会话断言会塌）";
  }
  // 再钉一层：连"不常见的 status"也不许重写。少写这一层的话，
  // 下一个加端点的人带回来一个 418/451 就会重蹈覆辙。
  for (const int status : {200, 201, 301, 302, 418, 451, 599}) {
    EXPECT_FALSE(error_reply_for(status, true).has_value()) << "status=" << status;
  }
}

// 反面：body 为空就必须给出包络。httplib 造的 404/400/413 就是这种形状
// （见 httplib.h 的 write_response：未匹配路由时 res.body 从未被填过）。
TEST(ErrorRouting, AnEmptyBodyAlwaysGetsTheUniformEnvelope) {
  for (const int status : all_statuses()) {
    const std::optional<ErrorRouting> routing = error_reply_for(status, false);
    ASSERT_TRUE(routing.has_value()) << "status=" << status << " 没有给出收口包络";
    EXPECT_FALSE(routing->reply.body.empty());
    EXPECT_GE(business_code_of(*routing), 1000)
        << "status=" << status << " 给出了一个非错误码";
  }
}

TEST(ErrorRouting, UnknownRouteOrMissingStaticFileBecomes1004) {
  const std::optional<ErrorRouting> routing = error_reply_for(404, false);
  ASSERT_TRUE(routing.has_value());
  EXPECT_EQ(business_code_of(*routing), 1004);
  EXPECT_EQ(routing->reply.status, 404);
  EXPECT_FALSE(routing->protocol_level)
      << "「路径不存在」是正常的用户操作，不该按协议级拒绝发 WARN";
}

TEST(ErrorRouting, MethodNotAllowedBecomes1004JustLikeNotFound) {
  const std::optional<ErrorRouting> routing = error_reply_for(405, false);
  ASSERT_TRUE(routing.has_value());
  EXPECT_EQ(business_code_of(*routing), 1004);
  EXPECT_FALSE(routing->protocol_level);
}

TEST(ErrorRouting, PayloadTooLargeBecomes1001AndKeepsTheHumanReason) {
  const std::optional<ErrorRouting> routing = error_reply_for(413, false);
  ASSERT_TRUE(routing.has_value());
  EXPECT_EQ(business_code_of(*routing), 1001);
  EXPECT_EQ(routing->reply.status, 400);
  // ⭐ 这条【曾经失效】：早先 fail(error, "", reason) 在 field 为空时整体退化，
  //   于是这句"请求体超出允许的大小"从未真正到达过客户端。
  //   routes.cpp 的 413 分支正是靠它给用户一句能照做的说明。
  const nlohmann::json body = nlohmann::json::parse(routing->reply.body);
  EXPECT_TRUE(body["data"].contains("reason")) << routing->reply.body;
  EXPECT_FALSE(body["data"]["reason"].get<std::string>().empty());
  EXPECT_FALSE(routing->protocol_level);
}

// 协议级拒绝一律归 1001：httplib 在这些场合造出的 4xx/5xx 都是"请求本身有问题"
// （坏请求头、URI/头超限、方法不被接受），客户端据此知道"重发无用"。
// 报 1008 会诱导前端弹"服务器内部错误，请稍后重试"并开始重试 —— 而重试必然再失败。
TEST(ErrorRouting, ProtocolLevelRejectionsBecome1001AndAreFlagged) {
  for (const int status : kProtocolStatuses) {
    const std::optional<ErrorRouting> routing = error_reply_for(status, false);
    ASSERT_TRUE(routing.has_value()) << "status=" << status;
    EXPECT_EQ(business_code_of(*routing), 1001) << "status=" << status;
    EXPECT_TRUE(routing->protocol_level) << "status=" << status << " 未被标为协议级";
  }
}

TEST(ErrorRouting, TheThreeHandledStatusesAreNotFlaggedAsProtocolLevel) {
  // {404,405,413} 是收口自己显式认识的三种；它们都不该发协议级 WARN。
  for (const int status : {404, 405, 413}) {
    const std::optional<ErrorRouting> routing = error_reply_for(status, false);
    ASSERT_TRUE(routing.has_value()) << "status=" << status;
    EXPECT_FALSE(routing->protocol_level)
        << "status=" << status
        << " 被标成协议级 —— 调用方会为「路径不存在」这种正常情况发无谓的 WARN";
  }
}

// [D-29] 在路由层的落地：不管走哪条分支，"HTTP 状态 == 包络码的 http_status"必须恒成立。
//   这条比上面几条更强：它是【关系式】而非【逐点】断言，加新分支也不会漏掉。
TEST(ErrorRouting, EveryBranchAgreesWithTheErrorTable) {
  for (const int status : all_statuses()) {
    const std::optional<ErrorRouting> routing = error_reply_for(status, false);
    ASSERT_TRUE(routing.has_value()) << "status=" << status;
    const int code = business_code_of(*routing);
    EXPECT_EQ(routing->reply.status, common::http_status(static_cast<common::ApiError>(code)))
        << "status=" << status << " 收口后与 §6.3 码表不同步";
  }
}

TEST(ErrorRouting, TheEnvelopeAlwaysHasTheContractKeys) {
  // 与 response_test 的同名不变量呼应，但放在路由层：这里的 body 是收口现造的，
  // 而 response_test 造的是业务层直接调用的形态。两条路径都要是合法包络。
  for (const int status : all_statuses()) {
    const std::optional<ErrorRouting> routing = error_reply_for(status, false);
    ASSERT_TRUE(routing.has_value());
    const nlohmann::json body = nlohmann::json::parse(routing->reply.body);
    EXPECT_TRUE(body.contains("code")) << status;
    EXPECT_TRUE(body.contains("message")) << status;
    EXPECT_TRUE(body["data"].is_object()) << status;
  }
}

// =============================================================================
// StaticMount —— 静态目录缺失必须让启动【响亮地失败】（§2.1-A）
// =============================================================================

// 静态文件是五页前端唯一的来源；缺了它服务起来了也没有任何页面可访问，
// 而 AC-1 的验收要过浏览器。因此 register_routes 对不可用目录必须返回 false，
// 由 main.cpp 转成"拒绝启动"，而不是起一个只有 API 的空壳。
TEST(StaticMount, RegisterRoutesFailsWhenTheStaticDirIsNotADirectory) {
  httplib::Server server;
  ServerOptions options;
  // 一个必然不存在的路径。用相对路径且带明显非法字符，避免本机碰巧存在同名目录
  options.static_dir = "./tests/no-such-static-dir-for-routes-test";
  EXPECT_FALSE(register_routes(server, options));
}

TEST(StaticMount, RegisterRoutesSucceedsForARealDirectory) {
  httplib::Server server;
  ServerOptions options;
  options.static_dir = "./static";   // 仓库内真实存在的目录（ctest 的工作目录是仓库根）
  EXPECT_TRUE(register_routes(server, options));
}

// 幂等：同一个 Server 上注册两次应当都成功（set_mount_point 是 append 语义，
// 不会因为重复调用而失败）。这条钉住"将来若有人在别处也调 register_routes，
// 不会因为『已经挂过一次』而静默改变行为"。
TEST(StaticMount, RegisterRoutesIsSafeToCallTwiceOnTheSameServer) {
  httplib::Server server;
  ServerOptions options;
  options.static_dir = "./static";
  EXPECT_TRUE(register_routes(server, options));
  EXPECT_TRUE(register_routes(server, options));
}

}  // namespace web
}  // namespace oj
