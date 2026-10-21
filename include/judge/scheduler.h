// 判题调度器：1 调度线程 + N worker；全局闸门 / 单用户闸门；StatusProvider 快照
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace oj {
namespace judge {

struct SchedulerStatus {
  long long queue_length = 0;
  long long active_judges = 0;
  bool judge_alive = false;
};

// 进程内单例。start() 必须在题库缓存与工作区扫残之后、HTTP listen 之前调用。
class Scheduler {
 public:
  static Scheduler& instance();

  // workers/gate 取自 config（.env）。重复 start 返回 false。
  bool start();
  // 停止并 join 全部线程。幂等。
  void stop();

  bool alive() const;
  void enqueue(unsigned long long submission_id, unsigned long long user_id);
  SchedulerStatus status() const;

  // 单用户并发评测闸（OJ_JUDGE_USER_GATE）。worker 在处理一条提交前后调用。
  void acquire_user_slot(unsigned long long user_id);
  void release_user_slot(unsigned long long user_id);

  // 全局评测闸门（OJ_JUDGE_GATE）：约束同时执行的用例子进程数。
  void acquire_global_slot();
  void release_global_slot();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

 private:
  Scheduler() = default;
  ~Scheduler();
  void scheduler_loop();
  void worker_loop(int index);

  mutable std::mutex mu_;
  std::condition_variable cv_task_;
  std::condition_variable cv_worker_;
  std::condition_variable cv_user_;
  std::condition_variable cv_gate_;

  std::vector<std::thread> workers_;
  std::thread scheduler_thread_;

  // 空闲 worker 槽位（每 worker 一个 pending task；空 = nullopt 语义用 has_task）
  struct Slot {
    bool has_task = false;
    unsigned long long submission_id = 0;
    unsigned long long user_id = 0;
  };
  std::vector<Slot> slots_;
  std::vector<bool> worker_busy_;

  std::unordered_map<unsigned long long, int> user_active_;
  int user_gate_ = 2;
  int gate_capacity_ = 4;
  int gate_in_flight_ = 0;
  long long active_judges_ = 0;
  std::atomic<bool> alive_{false};
  std::atomic<bool> stopping_{false};
};

// 便捷入口
Scheduler& scheduler();

}  // namespace judge
}  // namespace oj
