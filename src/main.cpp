// =============================================================================
// CppOJ 入口 —— 【占位实现，待 T0.5 替换】
// 当前只做一件事：加载配置、把诊断与生效配置回显到 stdout 与 app.log。
// T0.5（httplib 起服务 + 静态文件映射）落地后，这里会变成
// §2.3 的启动顺序：加载题库 → 启动 judge 调度器 → listen() 阻塞。
// =============================================================================
#include <cstdio>
#include <string>

#include "common/config.h"
#include "common/log.h"

int main(int argc, char** argv) {
  // 允许 `./oj /path/to/.env` 覆盖默认路径，便于临时验证另一份配置
  const std::string env_path = (argc > 1) ? argv[1] : ".env";

  const oj::common::LoadResult result = oj::common::load_config(env_path);
  std::printf("%s\n", oj::common::format_diagnostics(result).c_str());

  // 日志必须在配置之后、线程池之前初始化（log.h 的【启动契约】）：
  // log_dir / retention_days 都来自配置；反过来，"连配置都没读到"这种
  // 启动期失败会无处可记 —— init_logging() 在配置未加载时用内置默认值。
  const oj::common::LogInitReport log_report = oj::common::init_logging();
  for (const std::string& problem : log_report.problems) {
    std::fprintf(stderr, "[oj] %s\n", problem.c_str());
  }

  if (!result.ok) {
    // 配置自相矛盾时拒绝启动：带着半份配置跑起来比跑不起来危险得多
    oj::common::log_error(oj::common::LogChannel::kApp,
                          "配置校验未通过（" +
                              std::to_string(result.error_count()) +
                              " 条 ERROR），拒绝启动");
    oj::common::shutdown_logging();
    std::fprintf(stderr, "[oj] 配置校验未通过（%zu 条 ERROR），拒绝启动\n",
                 result.error_count());
    return 1;
  }

  const std::string config_echo = oj::common::dump_config();
  std::printf("\n%s\n", config_echo.c_str());
  std::printf("\n[oj] 配置已加载。HTTP 服务尚未接线（T0.5），本次进程即将退出。\n");

  // 启动回显落 app.log：题库被跳过的题、D-37 的 admin 名单结论都追加在它后面
  oj::common::log_text(oj::common::LogChannel::kApp, oj::common::LogLevel::kInfo,
                       config_echo);
  oj::common::log_info(oj::common::LogChannel::kApp,
                       "配置已加载，HTTP 服务待接线（T0.5）");
  oj::common::shutdown_logging();
  return 0;
}