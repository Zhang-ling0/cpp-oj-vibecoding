// 单条提交的判题流水线：取题 → 生成 → 编译 → 逐用例 → 写回
#pragma once

namespace oj {
namespace judge {

// 处理一条提交（阻塞）。异常与失败路径一律写回 failed/JE，不向外抛。
void judge_one(unsigned long long submission_id, unsigned long long user_id);

}  // namespace judge
}  // namespace oj
