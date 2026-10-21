// =============================================================================
// 提交配额与并发闸（阶段 2 · 解决 P0-1）—— 1006 / 1007 的唯一实现处
// 规格来源：SPEC §5.8 [D-22] 配额 / §6.3 错误码 1006 1007 / §2.3 闸门
//           .env：OJ_RATE_WINDOW_SECONDS、OJ_RATE_MAX_SUBMITS、OJ_JUDGE_USER_GATE
//
// 【两个检查点是两件事，不合并】
//   1006 滑动窗口   —— 纯内存计数，拦「连点提交」，与 DB 无关；
//   1007 并发评测闸 —— DB 计数（pending + judging），拦「同一个人排一堆评测」。
//   合并成一个函数的代价是：1006 不需要租约（可在取租约【之前】拦掉最便宜的那层），
//   而 1007 必须读 DB。揉在一起就让最热的那条路径每次都多排一次全局 DB 互斥（D-21）。
//
// 【检查点位置】两者都在 handle_submission_create【建行之前】：
//   先回错误、不落库 —— 与 judge 闸门（1009 不建行）同一条理由：
//   落了行就得有人来判，而被判的正是"我本来不打算收下"的请求。
//
// 【1006 为什么按【用户】而不是按 IP】
//   本项目 D-23 起登录态是唯一身份来源；按 IP 会让同 NAT 的所有人共享一个配额，
//   而共享代理后一个人打满配额、整片用户一起吃 429。用户维度的另一个好处是
//   登出/换号不残留：窗口里的时间戳跟着 user_id 走，不跟着连接走。
//
// 【内存是刻意有界的】
//   只保留「窗口内还有时间戳」的用户；每过一个完整窗口做一次全表清扫。
//   因此常驻上界 ≈ 活跃提交用户数 × 每人 ≤ rate_max_submits_per_window 条 8 字节时间戳，
//   与总用户数无关（不会随注册量增长）。2 核 2G 的机器上这条不是风格问题。
// =============================================================================
#pragma once

#include <string>

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// 滑动窗口配额（→ 1006）。allowed=false 时 *retry_after_sec 是"最早还能提交"的秒数（≥ 1）。
// 配置取 common::config() 的 rate_window_seconds / rate_max_submits_per_window。
bool rate_allow(unsigned long long user_id, int* retry_after_seconds);

// 单用户并发评测闸（→ 1007）：该用户名下 status IN ('pending','judging') 的条数
// 已达 OJ_JUDGE_USER_GATE 时拒绝。*active 回传实测条数（用于错误文案）。
enum class GateResult {
  kAllowed,
  kLimited,   // → 1007
  kError      // DB 查询失败 → 1008（与 kLimited 必须分开：把 DB 故障报成"你提交太多了"
              // 会让用户去等一个永远不会到来的窗口）
};
GateResult user_gate_check(db::Session& session, unsigned long long user_id, long long* active,
                           std::string* error);

// 仅供单元测试：清空内存窗口（进程内状态，没有对应的 DB 对象可清）。
void rate_reset();

}  // namespace web
}  // namespace oj
