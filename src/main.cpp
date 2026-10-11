// =============================================================================
// CppOJ 入口 —— 【占位实现，待 T0.5 替换】
// 当前只做一件事：加载配置、把诊断与生效配置回显到 stdout。
// T0.5（httplib 起服务 + 静态文件映射）落地后，这里会变成
// §2.3 的启动顺序：加载题库 → 启动 judge 调度器 → listen() 阻塞。
// =============================================================================
#include <cstdio>
#include <string>

#include "common/config.h"

int main(int argc, char** argv) {
  // 允许 `./oj /path/to/.env` 覆盖默认路径，便于临时验证另一份配置
  const std::string env_path = (argc > 1) ? argv[1] : ".env";

  const oj::common::LoadResult result = oj::common::load_config(env_path);
  std::printf("%s\n", oj::common::format_diagnostics(result).c_str());

  if (!result.ok) {
    // 配置自相矛盾时拒绝启动：带着半份配置跑起来比跑不起来危险得多
    std::fprintf(stderr, "[oj] 配置校验未通过（%zu 条 ERROR），拒绝启动\n",
                 result.error_count());
    return 1;
  }

  std::printf("\n%s\n", oj::common::dump_config().c_str());
  std::printf("\n[oj] 配置已加载。HTTP 服务尚未接线（T0.5），本次进程即将退出。\n");
  return 0;
}