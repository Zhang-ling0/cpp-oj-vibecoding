// =============================================================================
// CppOJ 只读管理端点 `GET /api/admin/overview` 的业务层
// 对应任务：SPEC.md §9 M3 T3.12 `[D-37]`
// 规格来源：§6.3.3（端点与守卫）/ §6.4（进度推导口径）/ §6.5（聚合写法）
//           / §6.3 错误码 1002/1003 / §2.3 锁顺序 / §3.2 DDL
//
// 【范围：这一层只负责"算什么、怎么算、要几条 SQL"】
//   不含 HTTP 路由与响应包络（T3.5 + common/response.h，尚未开工）、
//   不含 Cookie → 登录用户的解析（T3.3）、不含判题调度器本体（T2.7）。
//   因此三个入口都刻意做成"外部把依赖递进来"的形式：
//     · 守卫吃【已解析出的登录用户】（AuthUser），不自己去读 Cookie；
//     · 判题运行态吃【一个取值函数】（StatusProvider），不自己去拿调度器指针；
//     · 聚合 SQL 吃【租约内的 db::Session】，不自己管连接生命周期。
//   这样接线时不需要改本文件，而本文件现在就能被单测覆盖。
//
// 【锁顺序：为什么带 StatusProvider 的重载先取快照、再取 DB 租约】（§6.3.3 实现约束 2）
//   §2.3 的既有约束是「DB 锁仅在 DAO 调用期间持有」。`queueLength` / `activeJudges` /
//   `judgeAlive` 三项来自调度器的进程内状态，要拿调度器的锁；若【先】取 DB 租约再去读，
//   就多出一条 `DB mutex → scheduler lock` 的嵌套边 —— 而本进程还有一条反向边
//   （judge 写回事务持 DB 锁），两条边各自都能跑，合起来就是死锁的四要素。
//   反过来（先快照、后租约）则两把锁严格串行、永不同时持有。
//   实现顺序因此不是风格问题，是这条约束的唯一可行解。
//
// 【严格只读，且刻意不做"题库增删改"】（§6.3.3「为什么是只读」/ §6.3.4）
//   D-37 给 admin 的能力只有"看"。本文件里没有一条 INSERT/UPDATE/DELETE ——
//   不是没写完，是 §6.3.4 已经把"在线编辑题库"论证掉了：写回 JSON 与 §5.2 的边界冲突，
//   让 DB 当第二真源则要引入仲裁与版本化。角色调整同样只经 .env + 重启（§6.3.1）。
//   将来若真要做，先读 §6.3.4，而不是在这里顺手加一个 upsert。
// =============================================================================
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "web/session.h"   // AuthUser（登录身份由会话层产出，故定义在那边）

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// ---- 守卫：[D-37]「users.role 第一次真正参与放行决策」------------------------
//
// 顺序上【先判登录（1002）再判角色（1003）】：一个没登录的人携带了别人的 Cookie，
// 正确答案是"未登录"，不是"无权限" —— 前者会让他去登录，后者会让他以为自己
// 登录了却没权限（§6.3.3 授权守卫）。
//
// 【AuthUser 为什么上面没有自己的定义】它在 T3.3 之前放在本文件里，是"谁消费谁定义"
// 的产物。现在会话层（web/session.h）已经落地，而它才是生产这个类型的那一方 ——
// 守卫吃的是它的输出，让类型留在 admin.h 会让"生产方要 include 消费方"或"两边各写一份"。
// 故已收敛到 web/session.h，守卫的签名不变。

enum class GuardResult {
  kAllowed,           // → 0    放行
  kUnauthenticated,   // → 1002 未登录或会话已过期
  kForbidden          // → 1003 已登录但非 admin
};

// 唯一的守卫入口。user 为空（无 Cookie / 会话不存在 / 已过期）⇒ kUnauthenticated。
// 不在此处记日志：同一个未登录请求会打到多个端点，逐端点记日志会把 app.log 淹掉。
GuardResult require_admin(const std::optional<AuthUser>& user);
         
// 业务码与中文文案。与 §6.3 错误码表一一对应；路由层（T3.5）据此填响应包络。
// 不在此处返回 HTTP 状态码（1002→401 / 1003→403）：那是包络层的事，两处各写一份必然漂移。
int error_code_of(GuardResult result);
const char* error_message_of(GuardResult result);

// ---- 判题运行态：[D-37]「system 里有两项不在 DB 里」-------------------------
//
// 三项都来自 §2.1 判题调度器的进程内状态，没有对应的表：
//   queueLength   内存 FIFO 队列里还没开始的条数
//   activeJudges  当前正在评测的并发数（受全局闸门约束）
//   judgeAlive    调度器是否存活
// 由调用方提供一个"取快照"的函数，而不是把调度器指针递进来：调度器（T2.7）与本模块
// 谁先落地都不影响对方，且单测可以递一个自己写的 lambda。
struct JudgeRuntimeStatus {
  long long queue_length = 0;
  long long active_judges = 0;
  bool judge_alive = false;
};

using StatusProvider = std::function<JudgeRuntimeStatus()>;

// judge 未接线（T2.7 之前）时的取值：judgeAlive=false 且两项计数为 0。
// 这是【如实反映】，不是"假装一切正常"—— 运维页面上必须一眼看出判题没起来，
// 而三个 0 加一个 false 正是这个意思。（queueLength 的真实来源是内存队列，
// DB 里 status='pending' 的行数不是它，故不在这里用 COUNT 顶替。）
JudgeRuntimeStatus unwired_status();
StatusProvider unwired_status_provider();

// ---- 响应体（§6.3.3 的字段表，逐项对应）--------------------------------------

// system 块。八个数字来自 DB（一条 SQL 里的标量子查询，无 N+1），
// 三项运行态来自 StatusProvider。
struct SystemBlock {
  long long user_count = 0;
  long long admin_count = 0;
  long long problem_count = 0;
  // 全库 problem_cases 行数。刻意【不】用 SUM(problems.case_count)：
  //   后者是题库加载器维护的受控反范式列（§3.4），它与真实行数相等只是因为加载器
  //   在同一事务里刷新过它；真出现漂移时，运维页应该显示【真相】而不是副本。
  long long case_count = 0;
  long long submission_count = 0;
  long long ac_count = 0;          // verdict='AC' 的提交条数（不是"通过题数"）
  long long failed_count = 0;      // status='failed'（写回失败，§5.7 的兜底态）
  // 未完成的提交数：status IN ('pending','judging')。
  // ⚠️ 口径说明：§6.3.3 只给了字段名没给定义。这里取【未完成】而不是只取 judging，
  //   理由是 pending 的权威长度在内存队列（queueLength），若这里只数 judging，
  //   judge 未接线时排队中的提交在页面上就彻底不可见了。
  //   要改成只数 judging 的话，只需改 load_system_counts() 里这一处 WHERE 条件。
  long long judging_count = 0;

  // 以下三项来自 StatusProvider，不是 DB
  long long queue_length = 0;
  long long active_judges = 0;
  bool judge_alive = false;
};

// users[] 的一行。字段与 §6.3.3 的 users[] 表一一对应，顺序也一致。
// createdAt / lastSubmitAt / lastVerdict 按 [D-17] 存的是 UTC，
// 本地化由前端做（§6.2 的时间约定），故此处一律原样透传。
struct AdminUserRow {
  std::string username;
  std::string role;
  std::string created_at;
  long long submission_count = 0;
  long long ac_count = 0;
  long long solved = 0;
  long long attempted = 0;
  long long favorites = 0;
  std::string last_submit_at;   // 无提交时为空串（序列化为 JSON null）
  std::string last_verdict;     // 无提交时为空串；最新一条还没出结论时也是空串（verdict 可为 NULL）
};

struct Overview {
  SystemBlock system;
  std::vector<AdminUserRow> users;
};

// ---- 组装 --------------------------------------------------------------------

// 两条 SQL 各自独立，便于单测分别覆盖；out 不变式见各函数注释。
bool load_system_counts(db::Session& session, SystemBlock* out, std::string* error);
// users[] 按 user id 升序（注册顺序，稳定可复现；同名用户的 id 顺序与注册一致）。
bool load_users(db::Session& session, std::vector<AdminUserRow>* out, std::string* error);

// 组装入口。**锁顺序在这里落地**（见文件头）：先 provider() 取快照并【释放其锁】，
// 再取 [D-21] 的 DB 租约，最后读 DB 组装。失败时 *out 不被写入（保持调用方原值），
// 原因写进 error。
bool build_overview(const StatusProvider& provider, Overview* out, std::string* error);

// 已知快照时的组装（单测与将来复用同一次读取的场景走这条）。
// 调用方若持有 DB 租约，**必须**在取租约【之前】就取好 status —— 本函数不再兜底检查。
bool build_overview(db::Session& session, const JudgeRuntimeStatus& status,
                    Overview* out, std::string* error);

// §6.3 的响应包络里 data 的形状（camelCase，与 §6.3.3 字段表一致）。
// 空串的可空字段序列化为 JSON null 而不是 ""——两者在前端的含义不同
//（"没有提交" 与 "提交了但结论未知"）。
std::string to_json(const Overview& overview);

}  // namespace web
}  // namespace oj