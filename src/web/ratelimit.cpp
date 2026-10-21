// 实现见 include/web/ratelimit.h（口径与取舍写在那里）。本文件只放计数与一条 SQL。
#include "web/ratelimit.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"

namespace oj {
namespace web {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex& mutex() {
  static std::mutex m;
  return m;
}

// user_id → 窗口内各次提交的毫秒时间戳（升序追加，天然有序）。
// 只有【本窗口内还有记录】的用户才占一条 key，见 ratelimit.h【内存是刻意有界的】。
std::unordered_map<unsigned long long, std::vector<std::int64_t>>& windows() {
  static std::unordered_map<unsigned long long, std::vector<std::int64_t>> map;
  return map;
}

// 上一次全表清扫的时刻（毫秒）。清扫频率 = 每个窗口一次：足以让过期 key 归零，
// 又不至于让每次配额检查都退化成 O(用户数)。
std::int64_t& last_sweep_ms() {
  static std::int64_t value = 0;
  return value;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

}  // namespace

bool rate_allow(unsigned long long user_id, int* retry_after_seconds) {
  const common::Config& cfg = common::config();
  const std::int64_t window_ms = static_cast<std::int64_t>(cfg.rate_window_seconds) * 1000;
  const std::size_t max_hits = static_cast<std::size_t>(cfg.rate_max_submits_per_window);
  const std::int64_t now = now_ms();

  std::lock_guard<std::mutex> guard(mutex());

  // 全表清扫：每过一个完整窗口才做一次（清扫期间新写入的时间戳必然是"新的"，不会被误删）
  if (now - last_sweep_ms() >= window_ms) {
    last_sweep_ms() = now;
    for (auto it = windows().begin(); it != windows().end();) {
      std::vector<std::int64_t>& hits = it->second;
      hits.erase(hits.begin(),
                 std::upper_bound(hits.begin(), hits.end(), now - window_ms));
      if (hits.empty()) {
        it = windows().erase(it);
      } else {
        ++it;
      }
    }
  }

  std::vector<std::int64_t>& hits = windows()[user_id];
  hits.erase(hits.begin(), std::upper_bound(hits.begin(), hits.end(), now - window_ms));

  if (hits.size() >= max_hits) {
    // 最早那条记录到期的时刻就是"下一次能提交"的时刻（滑动窗口，不是固定窗口）
    const std::int64_t oldest = hits.front();
    std::int64_t wait = oldest + window_ms - now;
    if (wait < 1000) wait = 1000;   // 不满 1s 也报 1s，避免回一个 "0s 后重试"
    if (retry_after_seconds != nullptr) {
      *retry_after_seconds = static_cast<int>((wait + 999) / 1000);
    }
    return false;
  }

  hits.push_back(now);
  return true;
}

GateResult user_gate_check(db::Session& session, unsigned long long user_id, long long* active,
                           std::string* error) {
  const common::Config& cfg = common::config();
  const db::ResultSet counted = session.run(
      "SELECT COUNT(*) FROM submissions WHERE user_id = ? AND status IN ('pending','judging')",
      {db::Value::from_u64(user_id)});
  if (counted.failed()) {
    const std::string detail = "并发评测闸：统计未完成提交失败：" + counted.error().format();
    common::log_error(common::LogChannel::kApp, detail);
    if (error != nullptr) *error = detail;
    return GateResult::kError;
  }
  const long long running = counted.empty() ? 0 : counted.at(0).i64(0);
  if (active != nullptr) *active = running;
  return running >= cfg.judge_user_gate ? GateResult::kLimited : GateResult::kAllowed;
}

void rate_reset() {
  std::lock_guard<std::mutex> guard(mutex());
  windows().clear();
  last_sweep_ms() = 0;
}

}  // namespace web
}  // namespace oj
