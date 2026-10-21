#include "judge/queue.h"

#include <algorithm>

namespace oj {
namespace judge {

void JudgeQueue::push(JudgeTask task) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopped_) return;
    // 保持 id 升序：queueAhead 依赖 FIFO 出队（submission_api.cpp）
    auto it = std::upper_bound(
        tasks_.begin(), tasks_.end(), task.submission_id,
        [](unsigned long long id, const JudgeTask& t) { return id < t.submission_id; });
    tasks_.insert(it, task);
  }
  cv_.notify_one();
}

bool JudgeQueue::pop(JudgeTask* out) {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
  if (tasks_.empty()) return false;
  *out = tasks_.front();
  tasks_.pop_front();
  return true;
}

bool JudgeQueue::try_pop(JudgeTask* out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (tasks_.empty()) return false;
  *out = tasks_.front();
  tasks_.pop_front();
  return true;
}

void JudgeQueue::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
  }
  cv_.notify_all();
}

void JudgeQueue::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  stopped_ = false;
}

std::size_t JudgeQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tasks_.size();
}

bool JudgeQueue::stopped() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stopped_;
}

}  // namespace judge
}  // namespace oj
