// Judge 内存队列：FIFO 按 id 升序（queueAhead 的隐含契约，submission_api.cpp）
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace oj {
namespace judge {

struct JudgeTask {
  unsigned long long submission_id = 0;
  unsigned long long user_id = 0;
};

class JudgeQueue {
 public:
  // 按 submission_id 升序插入（乱序 push 时保持全局 FIFO）
  void push(JudgeTask task);
  // 阻塞直到有任务或 stop；返回 false 表示已 stop 且队列空
  bool pop(JudgeTask* out);
  // 非阻塞取任务；空队列返回 false
  bool try_pop(JudgeTask* out);
  void stop();
  // 重新开放队列（stop 之后再次 start 用）。清空停止标志，不清任务。
  void reset();
  std::size_t size() const;
  bool stopped() const;

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<JudgeTask> tasks_;
  bool stopped_ = false;
};

}  // namespace judge
}  // namespace oj
