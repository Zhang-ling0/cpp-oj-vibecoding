// =============================================================================
// CppOJ 会话管理 —— 「64 hex token 会话 + HttpOnly Cookie」的完整一层
// 对应任务：SPEC.md §9 M3 T3.3（认证）/ T3.10（过期会话回收 §6.3.2）
// 规格来源：[D-23] 用户名密码 + Session Cookie / [D-69] CSRF 依赖声明（§6.3.2.1）
//           §6.3 端点清单（`sid=...; HttpOnly; SameSite=Lax; Path=/`）
//           §3.2 DDL（sessions：token CHAR(64) 主键 + idx_user + idx_expire）
//           §1.4 性能与并发 / §2.3 锁顺序（DB mutex 仅在租约内持有）
//
// 【范围：这一层只回答两个问题】
//   ①「这个请求是谁发的」      —— Cookie 头 → AuthUser（cookie → token → sessions JOIN users）
//   ②「给他一个会话 / 收回会话」—— 建会话、查会话、删会话、删别人的会话、回收过期会话
//   **不含**注册与登录端点本身（T3.5 路由 + T3.3 的 bcrypt 校验）：密码哈希与
//   响应包络都不是会话管理的职责。登录端点将来的形状是
//     验密码（T3.3） → issue() 拿 token → make_set_cookie() 写 Set-Cookie
//   本文件正好提供这三步所需的全部零件，且每一步都可单测。
//
// 【为什么是【一组自由函数】而不是一个带状态的 Manager 类】
//   会话的唯一真源是 `sessions` 表，本模块【没有也不该有】任何内存缓存 ——
//   一旦缓存，logout / 改密踢人 / 注销账号（D-40/D-57）就都要多一条"让缓存失效"的
//   路径，而那正是"两处各自合理、放在一起就矛盾"的典型来源（SPEC §8.1.1 的通则）。
//   没有状态就没有对象可 new，函数集合即"会话管理器"。代价是不存在 per-实例配置，
//   收益是【不存在忘记调用 invalidate 的可能】。
//
// 【token 形状检查必须在每次查库【之前】（本文件最容易被改坏的一条）】
//   `sessions.token` 是 utf8mb4 的 CHAR(64)，默认排序规则 `utf8mb4_general_ci`
//   **大小写不敏感**。也就是说 `WHERE token = 'AB…'` 会命中库里那条 `ab…` 的会话。
//   本模块只生成小写 hex，并在【唯一入口 find_user()】里先把形状钉死
//   （恰好 64 个 [0-9a-f]），于是任何非小写 hex 的伪造值根本到不了 SQL。
//   ⚠️ 若将来有人给别的入口加一条查会话的 SQL，必须同样先过 is_token_wellformed()：
//   Cookie 的值来自不可信输入（浏览器，也可能被手工构造），绕过这一步等于
//   把 ci 排序规则变成 token 校验的一部分。
//
// 【Set-Cookie 的四个属性与 D-69 的欠账】
//   HttpOnly    JS 读不到 → XSS 偷不走 token（本模块实际防线的第一层）
//   SameSite=Lax 跨站的 POST/DELETE 不携带 Cookie → CSRF 请求到不了鉴权层
//               （§6.3.2.1 明确声明：这是本项目【唯一的 CSRF 防线】，隐式但有效）
//   Path=/      整站可用；改成更窄的值会让 /api 之外的页面拿不到登录态
//   ⚠️ 【故意不加 Secure】——首版是明文 HTTP（§6.2），加了 Cookie 永不下发。
//      启用 HTTPS 时必须同时补 Secure 并让全站走 HTTPS（§6.3.2.1 的三条清单），
//      这个欠账是显式的，不是遗漏。
//
// 【删除会话只有一条路径：purge_expired / destroy / destroy_others】
//   查会话（find_user）**不写库**：已过期的 token 一律按"不存在"处理，那一行
//   留给回收任务删。理由是读路径一旦带写，logout 与回收就有了两套删除语义
//   （删自己的 vs 删全部过期的），并发时说不清谁覆盖谁。现在"谁被删"只有三种答案，
//   且都在本文件里。
//
// 【锁：只有一把，且只在租约内】（§2.3 / [D-21]）
//   所有 DB 操作都吃调用方递进来的 `db::Session&`（即已持有的租约），本模块【不自己取租约】——
//   除了 authenticate() 的便捷重载，而它先解析 Cookie：**没带 sid 的请求连租约都不取**，
//   未登录流量（[D-43]：题面公开可读）因此不会在全局互斥量上排队。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace oj {
namespace db {
class Session;
}  // namespace db

namespace web {

// ---- 常量 ---------------------------------------------------------------------

// Cookie 名。§6.3 的登录端点写死了 `Set-Cookie: sid=...`；改名要同步改前端
// auth.js 与 §10.1 AC-1 的 curl 验收，故它是契约的一部分而不是内部实现细节。
inline constexpr const char* kSessionCookieName = "sid";

// 32 字节随机数 → 64 个小写 hex。DDL 是 `token CHAR(64) NOT NULL PRIMARY KEY`。
inline constexpr std::size_t kTokenHexLength = 64;
inline constexpr std::size_t kTokenRandomBytes = 32;

// ---- 登录身份 ------------------------------------------------------------------

// 「已确认登录」的那个人。
// 【为什么定义在这里而不是 admin.h】它由本模块产出、由 admin.h / problem_api 的
//   ViewerContext / 路由层消费 —— 谁生产谁定义，否则每上一层就多一个同义类型。
//   user_id 放在【最后】且有默认值，聚合初始化 AuthUser{"bob","user"} 依然可用。
struct AuthUser {
  std::string username;
  std::string role;   // users.role 的两个枚举值之一（§3.2 是 ENUM，别的值进不来）
  unsigned long long user_id = 0;

  // 默认值封闭：任何非 "admin" 一律不算管理员。加了第三个角色时这里会立刻暴露。
  bool is_admin() const { return role == "admin"; }
};

// ---- 纯函数（不碰 DB，永远可测）------------------------------------------------

// 32 字节 CSPRNG（RAND_bytes，libcrypto）→ 64 个小写 hex。
// ⚠️ 绝不用 rand()/std::mt19937：token 是 bearer credential，预测它等于放行任何人。
// 随机源不可用时**抛 std::runtime_error** —— 这是全模块唯一允许的异常，因为它发生在
// 建会话之前（此刻还没有任何状态被改动），而"悄悄退化成弱随机"是无法察觉的故障。
std::string generate_token();

// 恰好 kTokenHexLength 个 [0-9a-f]。见文件头【token 形状检查】——这是防 ci 排序规则的
// 唯一屏障，所有查会话的入口都必须先过它。
bool is_token_wellformed(const std::string& token);

// 从 `Cookie:` 请求头里取出 sid 的值。取不到返回 nullopt（**不是错误**，是未登录）。
// 规则：按 ';' 切分 → 去掉首尾空白 → 第一个 '=' 左侧去空白后【全等】匹配 sid
//       → 右侧去空白并剥掉一层成对的双引号 → 空值视为取不到。
// 之所以要求【全等】而不是前缀匹配：`xsid=` / `sid_extra=` 这类同前缀的 Cookie
// （同域下别的应用或旧版本遗留）绝不能被当成本站的会话。
std::optional<std::string> parse_sid(const std::string& cookie_header);

// `Set-Cookie` 头的完整值：`sid=<token>; HttpOnly; SameSite=Lax; Path=/`
// 顺序照抄 §6.3 的写法，方便与文档逐字对照。见文件头【四个属性与 D-69 的欠账】。
std::string make_set_cookie(const std::string& token);

// 登出用的过期 Cookie：名字/路径/属性必须与 make_set_cookie() 【完全一致】，
// 否则浏览器会当成另一个 Cookie 留着，logout 就"清了服务端没清客户端"。
// Max-Age=0 与 Expires=1970 同时给出：前者是现代浏览器的事实标准，后者兜住
// 不认 Max-Age 的老实现，两者矛盾时以 Expires 早的那个为准，结论一致。
std::string make_clear_cookie();

// ---- DB（吃租约内的 db::Session，不自己取租约）----------------------------------

// 过期会话回收（§6.3.2）：`DELETE FROM sessions WHERE expires_at < UTC_TIMESTAMP()`。
// ⚠️ 必须 UTC_TIMESTAMP() 而非 NOW()（§6.3.2 唯一的正确性要点）；连接时区虽已被
//   mysql_pool 锁成 +00:00，SQL 里仍统一写 UTC_TIMESTAMP()，免得哪天这层判例被抄走。
// 走 idx_expire，无需新增索引。removed 可为 nullptr。
// **失败不影响登录**：调用方按 SPEC 只记日志即可，本函数只如实返回 false。
bool purge_expired(db::Session& session, long* removed, std::string* error);

// 登录成功后的"发一个会话"：**同一事务内先回收、再插入**（§6.3.2 的约束）。
// 同事务不是洁癖：回收与插入若分开提交，两个并发登录可能删掉对方刚落库、
// 尚未交给浏览器的会话 —— 那是一行数据与一次登录的静默丢失。
// token 与 expires_at 都由本函数生成（expires_at = UTC_TIMESTAMP() + TTL 天），
// 有效期取 config().session_ttl_days（§6.7.3 的 OJ_SESSION_TTL_DAYS，config 层已夹到
// [1,3650]），因此**代码里没有第二处 TTL 常量**、也没有第二处时钟。
// token_out：成功时写入新 token（失败时不变）。purged：本轮回收掉的行数。
// 两个出参都可为 nullptr（只要 token 就够的调用方不必造变量）。
bool issue(db::Session& session, unsigned long long user_id, std::string* token_out,
           long* purged, std::string* error);

// token → 登录身份。**过期即不存在**（SQL 里 `expires_at > UTC_TIMESTAMP()`）。
// 返回 nullopt 有两种含义，由 error 区分：
//   error 为空 → 没人登录（无此会话 / 已过期 / token 形状不合法）⇒ 路由层回 1002
//   error 非空 → DB 出错                                        ⇒ 路由层回 1008
// 单条 JOIN 取完用户名字与角色，没有 N+1；本函数【不写库】（见文件头）。
std::optional<AuthUser> find_user(db::Session& session, const std::string& token,
                                  std::string* error);

// 登出：删掉这一行。**该 token 本来就不存在也算成功** —— 登出的语义是"让这个
// 浏览器不再是登录态"，幂等是它应有而非可选的性质（前端连点两次不该报错）。
bool destroy(db::Session& session, const std::string& token, std::string* error);

// 改密踢掉其他设备（D-40 §6.3.5）：`DELETE ... WHERE user_id = ? AND token <> ?`。
// ⚠️ `token <> ?`（保留当前会话）是硬要求：没有它，改完密码的人会把自己踢下线，
//   体验等同于登出失败，而"改密后仍是登录态"正是该端点的验收点（AC-21）。
// removed 可为 nullptr（踢掉 0 个设备是完全正常的结果，不是错误）。
bool destroy_others(db::Session& session, unsigned long long user_id,
                    const std::string& keep_token, unsigned long long* removed,
                    std::string* error);

// ---- 入口：Cookie 头 → 登录身份 -------------------------------------------------

// 已持有租约时用这个（路由层通常已经为本次请求取了租约）。
// 无 sid → 直接 nullopt，不查库；token 形状不合法 → nullopt，同样不查库。
std::optional<AuthUser> authenticate(db::Session& session, const std::string& cookie_header,
                                     std::string* error);

// 便捷重载：自己取一次 [D-21] 租约。**先解析 Cookie 再取租约** —— 未登录请求
// （[D-43] 允许匿名读题面）不占用全局互斥量，也不打 DB。
std::optional<AuthUser> authenticate(const std::string& cookie_header, std::string* error);

}  // namespace web
}  // namespace oj