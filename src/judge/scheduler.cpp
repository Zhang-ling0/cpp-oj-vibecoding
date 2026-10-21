#include "judge/scheduler.h"

#include "common/config.h"
#include "common/log.h"
#include "db/dao.h"
#include "db/mysql_pool.h"
#include "judge/queue.h"
#include "judge/worker.h"

namespace oj {
namespace judge {
namespace {

// 模块级队列：scheduler 与 worker 共用一份
JudgeQueue g_queue;

}  // namespace

Scheduler& Scheduler::instance() {
  static Scheduler s;
  return s;
}

Scheduler& scheduler() { return Scheduler::instance(); }

Scheduler::~Scheduler() { stop(); }

bool Scheduler::start() {
  if (alive_.load()) return false;
  const common::Config& cfg = common::config();
  user_gate_ = cfg.judge_user_gate > 0 ? cfg.judge_user_gate : 2;
  gate_capacity_ = cfg.judge_global_gate > 0 ? cfg.judge_global_gate : 4;
  stopping_.store(false);
  g_queue.reset();

  const int workers = cfg.judge_workers > 0 ? cfg.judge_workers : 2;
  slots_.assign(static_cast<std::size_t>(workers), Slot{});
  worker_busy_.assign(static_cast<std::size_t>(workers), false);

  // 启动恢复：judging → failed/JE；pending 重新入队（§5.7 [D-20]）
  {
    const long recovered = dao::recover_stale_judging();
    if (recovered > 0) {
      common::log_warn(common::LogChannel::kJudge,
                       "启动恢复：" + std::to_string(recovered) +
                           " 条 judging 提交已置 failed/JE（§5.7）");
    }
    bool ok = false;
    const std::vector<unsigned long long> pending = dao::load_pending_ids(&ok);
    if (ok) {
      for (unsigned long long id : pending) {
        // user_id 在恢复路径上不参与闸门（无法可靠回查时置 0，闸门按"不限用户"处理）
        g_queue.push(JudgeTask{id, 0});
      }
      if (!pending.empty()) {
        common::log_info(common::LogChannel::kJudge,
                         "启动恢复：重新入队 " + std::to_string(pending.size()) + " 条 pending");
      }
    } else {
      common::log_error(common::LogChannel::kJudge, "启动恢复：读取 pending 失败");
    }
  }

  alive_.store(true);
  workers_.reserve(static_cast<std::size_t>(workers));
  for (int i = 0; i < workers; ++i) {
    workers_.emplace_back([this, i] { worker_loop(i); });
  }
  scheduler_thread_ = std::thread([this] { scheduler_loop(); });
  common::log_info(common::LogChannel::kJudge,
                   "调度器已启动：workers=" + std::to_string(workers) +
                       " gate=" + std::to_string(gate_capacity_) +
                       " user_gate=" + std::to_string(user_gate_));
  return true;
}

void Scheduler::stop() {
  if (!alive_.load() && workers_.empty() && !scheduler_thread_.joinable()) return;
  stopping_.store(true);
  g_queue.stop();
  {
    std::lock_guard<std::mutex> lock(mu_);
    cv_task_.notify_all();
    cv_worker_.notify_all();
    cv_user_.notify_all();
    cv_gate_.notify_all();
  }
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
  for (std::thread& w : workers_) {
    if (w.joinable()) w.join();
  }
  workers_.clear();
  alive_.store(false);
  common::log_info(common::LogChannel::kJudge, "调度器已停止");
}

bool Scheduler::alive() const { return alive_.load() && !stopping_.load(); }

void Scheduler::enqueue(unsigned long long submission_id, unsigned long long user_id) {
  g_queue.push(JudgeTask{submission_id, user_id});
  std::lock_guard<std::mutex> lock(mu_);
  cv_task_.notify_all();
}

SchedulerStatus Scheduler::status() const {
  SchedulerStatus s;
  s.judge_alive = alive();
  s.queue_length = static_cast<long long>(g_queue.size());
  std::lock_guard<std::mutex> lock(mu_);
  s.active_judges = active_judges_;
  return s;
}

void Scheduler::acquire_user_slot(unsigned long long user_id) {
  std::unique_lock<std::mutex> lock(mu_);
  cv_user_.wait(lock, [this, user_id] {
    return stopping_.load() || user_active_[user_id] < user_gate_;
  });
  ++user_active_[user_id];
}

void Scheduler::release_user_slot(unsigned long long user_id) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = user_active_.find(user_id);
    if (it != user_active_.end() && --it->second <= 0) user_active_.erase(it);
  }
  cv_user_.notify_all();
}

void Scheduler::acquire_global_slot() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_gate_.wait(lock, [this] {
    return stopping_.load() || gate_in_flight_ < gate_capacity_;
  });
  ++gate_in_flight_;
}

void Scheduler::release_global_slot() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (gate_in_flight_ > 0) --gate_in_flight_;
  }
  cv_gate_.notify_all();
}

void Scheduler::scheduler_loop() {
  while (!stopping_.load()) {
    // 找一个空闲 worker 槽位
    std::unique_lock<std::mutex> lock(mu_);
    std::size_t free_slot = slots_.size();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (!slots_[i].has_task) {
        free_slot = i;
        break;
      }
    }
    if (free_slot == slots_.size()) {
      cv_worker_.wait(lock, [this] {
        if (stopping_.load()) return true;
        for (const Slot& s : slots_) {
          if (!s.has_task) return true;
        }
        return false;
      });
      if (stopping_.load()) break;
      continue;
    }
    lock.unlock();

    JudgeTask task;
    if (!g_queue.pop(&task)) break;  // queue stopped

    lock.lock();
    slots_[free_slot] = Slot{true, task.submission_id, task.user_id};
    ++active_judges_;
    cv_worker_.notify_all();
  }
}

void Scheduler::worker_loop(int index) {
  while (!stopping_.load()) {
    JudgeTask task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_worker_.wait(lock, [this, index] {
        return stopping_.load() || slots_[static_cast<std::size_t>(index)].has_task;
      });
      if (stopping_.load()) break;
      Slot& slot = slots_[static_cast<std::size_t>(index)];
      task.submission_id = slot.submission_id;
      task.user_id = slot.user_id;
      slot.has_task = false;
    }

    if (task.submission_id != 0) {
      // 单用户闸门：恢复路径 user_id=0 时跳过（视为系统任务）
      const bool hold_user = task.user_id != 0;
      if (hold_user) acquire_user_slot(task.user_id);
      judge_one(task.submission_id, task.user_id);
      if (hold_user) release_user_slot(task.user_id);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (active_judges_ > 0) --active_judges_;
    }
    cv_task_.notify_all();
  }
}

}  // namespace judge
}  // namespace oj
