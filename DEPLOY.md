# DEPLOY —— CppOJ 部署手册

| 项 | 值 |
|---|---|
| 对应规格 | SPEC.md v1.9 §2.1-A / §6.7 / §9 T0.0–T0.5 |
| 部署路线 | **路线 A：宿主机原生**（当前唯一已跑通的形态）`[D-71]` |
| 目标系统 | Ubuntu 24.04 (noble) · amd64 |
| 最低资源 | **2 核 / 2GB**（功能验证档）；性能验收档 4GB `[D-74]` |
| 默认入口 | `http://127.0.0.1:8080/` |
| 更新日期 | 2026-10-21 |

> 本文是「从空白机器到可用服务」的操作手册；**为什么这么定**看 SPEC 对应章节。
> 依赖包的逐条理由见 [dependence.md](dependence.md)，本文件不重复论证，只给命令与判据。

---

## 0. 适用范围与前提

- **路线 A 是当前交付形态**：httplib / judge / g++ / mysqld 全在宿主机，建表由**应用启动时幂等执行**
  `db/init/01-schema.sql`（不是 initdb 机制）。路线 B（Docker）见 §12，**目前是空占位、未实现**。
- 两条路线的**判题流水线、API 契约、数据模型完全相同**，差别只在「谁拉起 mysqld 与 g++」。
- **工作目录 = 仓库根**。`./problems`、`./static`、`./logs`、`db/init/01-schema.sql`
  全是相对当前目录的路径，从别处启动会以「读不到文件」拒绝启动。
- 磁盘预算：源码 + 构建树约 **600MB**（`build/` 实测 575MB）、MySQL 数据目录约 200MB、
  题库与前端 < 5MB。建议预留 **≥ 2GB**。

---

## 1. ⚠️ 资源硬约束（每次开新环境先读这一节）

**卡住的不是 CPU，是内存。** 开发机实测 2 核 / 1967MB，mysqld 常驻后可用内存常只剩 300MB 上下；
带 sanitizer 的编译器进程峰值内存翻倍（ASan 影子内存），并发一高就进 swap / 被 OOM killer 杀，
**表现是「构建长时间无输出」，极易误判成编译器有 bug**。

| 场景 | 命令 | 说明 |
|---|---|---|
| 日常编译 | `make build` | Makefile 内部已固定 `cmake --build build -j2` |
| 手写编译 | `cmake --build build -j2` | **不要**省略 `-j2`，也不要写 `-j`（=核数） |
| 临时降并发 | `make build JOBS=1` | 不必改 Makefile |
| 单测 | `make test` | ctest **不加 `-j`**：417 条串行约 52s，并发只会抢资源 |
| ASan / UBSan | 见下方命令块 | **必须 `-j1`** + `ASAN_OPTIONS=detect_leaks=0` |
| 仅重新配置 | `cmake -S . -B build` | 无编译步骤，可随意并发 |

```bash
# sanitizer 构建（可选）：本机 LSan/TSan 受限，故必须关掉泄漏检测
ASAN_OPTIONS=detect_leaks=0 cmake -S . -B build-asan \
  -DCMAKE_CXX_COMPILER=g++-12 -DOJ_SANITIZE=address,undefined
cmake --build build-asan -j1
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

**症状 → 处置对照**（遇到先查这张表，再查代码）：

| 症状 | 最可能的原因 | 处置 |
|---|---|---|
| 构建长时间无输出 / 工具超时 | 并发过高（尤其 sanitizer） | 降到 `-j1`，重来 |
| ctest 报找不到 `./problems` | 工作目录不对 | 手动跑测试必须从仓库根执行 |
| 单测大面积超时 | 与 `mysqld` / 构建抢资源 | 先让构建结束；测试不加 `-j` |
| 进程被杀、dmesg 有 OOM | 同时开了构建 + 服务 + 造数 | 串行执行，别并行 |

---

## 2. 阶段 ① —— 系统依赖（一次性）

### 2.1 apt 包清单（与 SPEC §6.7.1 一致；httplib 不在此列，见 §2.2）

```bash
sudo apt-get update && sudo apt-get install -y \
  ca-certificates curl git \
  g++-12 gcc-12 make cmake libc6-dev \
  nlohmann-json3-dev libmysqlclient-dev libssl-dev \
  libgtest-dev \
  mysql-server mysql-client
```

**明确不装**（装了会出问题，不是「无所谓」）：

| 包 | 为什么不装 |
|---|---|
| `build-essential` | 连带装 g++-13，与「判题编译器锁 g++-12」冲突（`[D-13]`） |
| `libcpp-httplib-dev` | apt 版是 0.14.3，装了会让 `find_path` 命中旧头文件 —— 版本漂移正是 `[D-72]` 要消除的 |
| `libbcrypt-dev` | **24.04 官方源没有**；密码哈希走 OpenSSL Blowfish `[D-75]` |
| `pkg-config` | 库路径固定，CMake 用 `find_package` / `find_path` 直接找 |

### 2.2 cpp-httplib 0.60.1（header-only，不走 apt）

```bash
curl -sSL -o /tmp/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.60.1/httplib.h
sudo install -m 644 /tmp/httplib.h /usr/local/include/httplib.h
grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h   # 必须输出 "0.60.1"
```

> CMake 优先用 `/usr/local/include/httplib.h`（configure 时打印实际命中路径与版本）；
> 缺失则 WARNING 并回退仓库内 `third_party/httplib/httplib.h`（0.14.3，**版本不一致，视为环境未就绪**）。

### 2.3 测试工具（跑验收脚本才需要）

```bash
sudo apt-get install -y jq                       # tests/run_tests.sh 解析 JSON 用
pip3 install -r tests/python/requirements.txt    # Python 版端到端（requests）
```

### 2.4 装完验证（全绿再继续）

```bash
# ① g++-12 / ② cmake
g++-12 --version | head -1                                # 12.x
cmake --version | head -1                                 # >= 3.20（实测 3.28.3）

# ③ gtest 头文件  ④ httplib 版本  ⑤ 三个库头文件
ls /usr/include/gtest/gtest.h
grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h  # 必须是 "0.60.1"
ls /usr/include/{nlohmann/json.hpp,mysql/mysql.h,openssl/blowfish.h}

# ⑥ judge 依赖的 POSIX 头文件（getrusage / wait4 / prctl）
# ⚠️ 用编译探测，不要 ls /usr/include/sys/ —— Ubuntu 24.04 的这些头在多架构路径
#    /usr/include/x86_64-linux-gnu/sys/ 下，直接 ls 会误报「不存在」（SPEC §6.7.4
#    与 dependence.md 的原写法在 24.04 上不成立，以本条为准）
printf '#include <sys/resource.h>\n#include <sys/wait.h>\n#include <sys/prctl.h>\nint main(){return 0;}\n' \
  | g++-12 -std=c++17 -x c++ - -o /tmp/oj-hdr-probe && echo "✅ POSIX 头文件可用"
```

**第 7 项：判题机制的最小验证**（fork + setrlimit + prctl + wait，SPEC §5 的前提；
这一步失败意味着 §5 全部机制不可用）：

```bash
cat > /tmp/oj-rlimit-probe.cpp <<'EOF'
#include <csignal>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
int main() {
  struct rlimit rl{1, 1};
  if (setrlimit(RLIMIT_CPU, &rl) != 0) return 1;
  pid_t p = fork();
  if (p == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); for (;;) {} }   // 子进程烧 CPU
  if (p < 0) return 2;
  int st = 0;
  if (waitpid(p, &st, 0) != p) return 3;
  if (!WIFSIGNALED(st)) return 4;      // 必须被信号杀掉（SIGXCPU/SIGKILL）
  return 0;
}
EOF
g++-12 -std=c++17 -O2 /tmp/oj-rlimit-probe.cpp -o /tmp/oj-rlimit-probe \
  && /tmp/oj-rlimit-probe && echo "✅ rlimit 机制可用（约 1 秒）"
```

---

## 3. 阶段 ② —— 前端 vendored 资源

前端库**全部本地化**，运行时零外网请求 `[D-73]`。当前实际使用 6 个 JS：

```bash
cd /path/to/cpp-oj-vibcoding
B=https://cdn.jsdelivr.net/npm
mkdir -p static/vendor/ace

# Ace 编辑器 1.32.7（题目页代码编辑器）
curl -sSL -o static/vendor/ace/ace.js                 $B/ace-builds@1.32.7/src-min-noconflict/ace.js
curl -sSL -o static/vendor/ace/mode-c_cpp.js          $B/ace-builds@1.32.7/src-min-noconflict/mode-c_cpp.js
curl -sSL -o static/vendor/ace/theme-tomorrow.js      $B/ace-builds@1.32.7/src-min-noconflict/theme-tomorrow.js
curl -sSL -o static/vendor/ace/ext-language_tools.js  $B/ace-builds@1.32.7/src-min-noconflict/ext-language_tools.js
curl -sSL -o static/vendor/ace/LICENSE.txt            $B/ace-builds@1.32.7/LICENSE

# Markdown 渲染 + XSS 过滤
curl -sSL -o static/vendor/marked.min.js              $B/marked@12.0.2/marked.min.js
curl -sSL -o static/vendor/purify.min.js              $B/dompurify@3.1.6/dist/purify.min.js
```

> ⚠️ DOMPurify 的 npm 包名是 **`dompurify`（全小写）**。写成 `DOMPurify@…/purify.min.js` 会 404；
> 而某些 CDN 对不存在的文件**仍返回 200 + 一段提示文本** —— 仓库历史上就落过一个 43 字节的占位文件，
> 症状是整页只剩标题（`DOMPurify is not defined`）。**所以必须做下面的落盘校验，不能只看 curl 退出码。**

### 3.1 落盘校验（四步，缺一不可）

```bash
# ① 体积下限：低于 1KB 的必定是错误页/占位文件
find static/vendor -name '*.js' -size -1k | wc -l        # 应为 0

# ② 首行/内容特征：能 grep 到该库自己的版本或符号
grep -m1 -o 'ace.edit'                static/vendor/ace/ace.js
grep -m1 -o 'ace/mode/c_cpp'          static/vendor/ace/mode-c_cpp.js
grep -m1 -o 'ace/theme/tomorrow'      static/vendor/ace/theme-tomorrow.js
grep -m1 -o 'ace/ext/language_tools'  static/vendor/ace/ext-language_tools.js
grep -m1 -o 'marked v12'              static/vendor/marked.min.js
grep -m1 -o 'DOMPurify 3.1.6'         static/vendor/purify.min.js

# ③ 语法可解析（装了 node 才跑）
for f in static/vendor/ace/*.js static/vendor/*.min.js; do node --check "$f" || echo "坏文件: $f"; done

# ④ 引用对账：HTML 里引用的每个 /vendor/ 文件都真实存在（不打印任何「缺失」行即通过）
grep -ho '/vendor/[A-Za-z0-9._/-]*\.js' static/*.html | sort -u | while read -r p; do
  [ -f "static${p}" ] || echo "缺失: $p"
done
```

### 3.2 已知死文件（可不下载）

| 路径 | 状态 |
|---|---|
| `static/vendor/cm/`（CodeMirror 5 个文件） | **死文件**：编辑器已改用 Ace（`static/js/editor.js` 自述），阶段 3 待删（question.md P2-13） |
| `static/js/diff.js` | **0 字节且无引用**：并排 diff 前端未实现（后端端点已就绪），阶段 3 待处理 |
| `static/vendor/LICENSES/` | 尚未创建（三份 MIT LICENSE，T0.6 欠账） |

---

## 4. 阶段 ③ —— MySQL 初始化

```bash
# 1) 数据目录：apt 安装 mysql-server 时通常已初始化；仅全新/手动 datadir 才需要：
#    sudo mysqld --initialize-insecure --user=mysql

# 2) 启动（make run 也会自动拉起 mysqld_safe，这里手动确认一次）
sudo mysqld_safe --skip-syslog &
sleep 5 && pgrep -x mysqld && echo "mysqld 已启动"

# 3) 低内存机器必调：默认 128MB buffer pool 在 2GB 机器上占比过大，
#    会与 judge 抢内存、把 mysqld 变成 OOM killer 的首选目标（SPEC §2.1-A 第 3 条）
sudo mysql -e "SET GLOBAL innodb_buffer_pool_size = 67108864;"   # 64MB

# 4) 建库建号（数据表不用管：应用启动时幂等执行 db/init/01-schema.sql）
sudo mysql -e "CREATE DATABASE IF NOT EXISTS oj DEFAULT CHARSET utf8mb4;
               CREATE USER IF NOT EXISTS 'oj'@'127.0.0.1' IDENTIFIED BY 'oj';
               GRANT ALL ON oj.* TO 'oj'@'127.0.0.1'; FLUSH PRIVILEGES;"

# 5) 验证
mysql -h 127.0.0.1 -uoj -poj oj -e "SELECT VERSION();"   # 8.0.x
```

> `SET GLOBAL` **只对当前运行实例生效**，mysqld 重启后会回到默认值。建议写进配置文件固化：
>
> ```ini
> # /etc/mysql/mysql.conf.d/mysqld.cnf 的 [mysqld] 段
> innodb_buffer_pool_size = 64M
> ```

---

## 5. 阶段 ④ —— 构建

```bash
cd /path/to/cpp-oj-vibcoding
make build          # 等价于 cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-12
                    #   && cmake --build build -j2
```

产出：

| 产物 | 用途 |
|---|---|
| `build/oj` | 服务主程序 |
| `build/oj-reset` | 清库 + 造测试数据工具（见 §10.3） |
| `build/*_test` | gtest 单测（ctest 注册 417 条） |

```bash
make test           # 417 条单测，约 52s；全绿才继续
```

> 构建系统自检：configure 阶段会打印**实际命中的 httplib 路径与版本** —— 若看到回退到
> `third_party/httplib` 的 WARNING，说明 §2.2 没装好，先修环境再继续。

---

## 6. 阶段 ⑤ —— 配置 `.env`

放在**仓库根**（`make run` 默认读 `./.env`；也可 `./build/oj /path/to/other.env` 指定另一份）。
优先级：**进程环境变量 > `.env` > 内置默认值**（进程环境优先是给路线 B 的 compose 注入用的）。

```bash
# ---- 策略 ----
OJ_ALLOW_REGISTER=1                 # 0 = 关闭注册端点（返回 1010）
# OJ_ADMIN_USERNAMES=alice          # 角色唯一真源；须为【已存在】的用户名

# ---- MySQL ----
MYSQL_HOST=127.0.0.1
MYSQL_PORT=3306
MYSQL_USER=oj
MYSQL_PASSWORD=oj
MYSQL_DB=oj

# ---- HTTP ----
OJ_LISTEN=0.0.0.0:8080
OJ_HTTP_THREADS=8                   # 运行期生效的线程池大小

# ---- Judge ----
OJ_JUDGE_WORKERS=2                  # 2 核机：4 worker 会自相争抢
OJ_JUDGE_GATE=4                     # 必须 = 2 × OJ_JUDGE_WORKERS，违反拒绝启动
OJ_JUDGE_USER_GATE=2                # 单用户同时评测上限（超限 1007）
OJ_FENCE_MULTIPLIER=10              # 兜底墙钟系数；改闸门前必须重算
OJ_JUDGE_WORKDIR=/tmp/oj-judge      # 工作区，启动时扫残

# ---- 配额 / 会话 ----
OJ_RATE_WINDOW_SECONDS=60
OJ_RATE_MAX_SUBMITS=10              # 超限 1006
OJ_MAX_CODE_BYTES=65536             # 64KB
OJ_SESSION_TTL_DAYS=7

# ---- 路径与日志 ----
OJ_PROBLEMS_DIR=./problems
OJ_STATIC_DIR=./static
OJ_LOG_DIR=./logs
OJ_LOG_RETENTION_DAYS=7
```

### 6.1 三条启动期硬校验（不满足直接拒绝启动，不会静默修正）

| 校验 | 规则 | 现值验算 |
|---|---|---|
| ① 闸门与 worker | `OJ_JUDGE_GATE == 2 × OJ_JUDGE_WORKERS` | `4 == 2 × 2` ✓ |
| ② 用户闸门 | `OJ_JUDGE_USER_GATE ≤ OJ_JUDGE_GATE` | `2 ≤ 4` ✓ |
| ③ 兜底墙钟余量 | `闸门 × 时限 < 时限 × OJ_FENCE_MULTIPLIER + 5000` | `4×3000=12000 < 35000` ✓（余量 2.9×） |

③ 被破坏时，**并发评测会把完全合法的解法系统性判成 TLE**（墙钟被分核拉长后超过兜底阈值）。
**改闸门 / 改系数 / 改时限，三者改任一个都要按这条不等式重算**，详见 SPEC §5.4.2.1。

### 6.2 启用管理员（固定顺序）

```
1. 先用页面注册账号（如 alice）
2. 把名字写进 .env：OJ_ADMIN_USERNAMES=alice
3. 重启服务 → 启动时同步 role='admin'
```

- 名字**必须已存在**，不会自动创建；不存在只在启动日志里 WARN 点名。
- 名单是**双向同步**：名单外的用户会被降级为普通用户。
- 变量**未设置** = 完全跳过同步（不动任何 role）；**显式留空** `OJ_ADMIN_USERNAMES=` = 全员降权。
  两者语义不同，别混（SPEC §6.3.1 约束 1）。
- 页面上改角色会被**下次启动覆盖回配置** —— 真源永远是 `.env`。

---

## 7. 阶段 ⑥ —— 启动 / 停止

### 7.1 前台启动（开发用）

```bash
make run           # 没起 mysqld 会先拉起 mysqld_safe，再 ./build/oj
```

### 7.2 后台启动（长期跑）

```bash
cd /path/to/cpp-oj-vibcoding                                # 同样必须在仓库根
setsid nohup ./build/oj > /tmp/oj-run.log 2>&1 < /dev/null &
sleep 3 && curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/login.html   # 200
```

启动回显是**行缓冲**的（`setvbuf(_IOLBF)`），所以 `nohup` 重定向的日志在出错那一刻也能看到原因。

### 7.3 可选：systemd 单元（机器没有 unit，需要常驻时自建）

```ini
# /etc/systemd/system/oj.service
[Unit]
Description=CppOJ online judge
After=network.target mysql.service

[Service]
Type=simple
WorkingDirectory=/path/to/cpp-oj-vibcoding        # ← 必须是仓库根
ExecStart=/path/to/cpp-oj-vibcoding/build/oj
Restart=on-failure
RestartSec=3
KillSignal=SIGTERM                                 # 与代码的优雅退出路径一致

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now oj
journalctl -u oj -f                                # 启动回显
```

### 7.4 停止

| 方式 | 命令 | 说明 |
|---|---|---|
| 前台 | `Ctrl-C` | SIGINT，唯一设计过的干净退出路径 |
| 后台 | `pkill -x oj` 或 `kill -TERM <pid>` | SIGTERM，同一条收尾路径 |
| 退出后 | — | 顺序执行「停调度器 → 关连接池 → 关日志」，退出码 0 |

> **不接 SIGHUP**：本项目只有「改配置 + 重启」一条运维路径，不提供热重载。

### 7.5 启动顺序（失败即退出，不留半份地基）

```
1  load_config()      读 .env + 进程环境 + 三条硬校验     失败 → exit 1
2  init_logging()     双文件按天切割（app / judge）
3  连接池就绪          mysqld 不可达 → exit 1（提示先跑 make run）
4  幂等建表            db/init/01-schema.sql + 核对 11 张表
4b 幂等补列            ensure_columns()（老库长出新列）
5  题库同步            problems/*.json → 校验 → 入库 → 灌缓存
                       某题校验不过 → 跳过该题 + ERROR 汇总（不阻止启动）
                       目录读不到 / 写库失败 → exit 1
6  工作区扫残          清 /tmp/oj-judge/*（必须排在 judge 之前）
7  过期会话回收
8  角色同步            OJ_ADMIN_USERNAMES → users.role
9  judge 调度器        启动恢复：judging→failed(JE)、pending 重新入队
10 HTTP listen         此后进程常驻；之后的问题只能靠 app.log 事后查
```

---

## 8. 阶段 ⑦ —— 部署验证

### 8.1 服务级（3 条）

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/            # 200（落地页）
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/list.html    # 200
curl -s http://127.0.0.1:8080/api/problems | head -c 200                    # code=0 + 题目数组
```

### 8.2 端到端（判题链路必须真跑一遍）

```bash
# 注册 + 登录
curl -s -c /tmp/oj.jar -H 'Content-Type: application/json' \
  -d '{"username":"deploy01","password":"deploy1234"}' \
  http://127.0.0.1:8080/api/auth/register

curl -s -c /tmp/oj.jar -H 'Content-Type: application/json' \
  -d '{"username":"deploy01","password":"deploy1234"}' \
  http://127.0.0.1:8080/api/auth/login

# 提交 two-sum 的正确解 → 应返回 201 + submissionId + queueAhead
curl -s -b /tmp/oj.jar -H 'Content-Type: application/json' -X POST \
  -d '{"problemSlug":"two-sum","code":"#include <vector>\nusing namespace std;\nclass Solution {public:\n vector<int> twoSum(vector<int>& nums, int target) {\n  int n=nums.size();\n  for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(nums[i]+nums[j]==target) return {i,j};\n  return {};\n }\n};"}' \
  http://127.0.0.1:8080/api/submissions

# 轮询到 done（每 1.5s；判 verdict 应为 AC）
curl -s -b /tmp/oj.jar http://127.0.0.1:8080/api/submissions/<submissionId>

# 验完清理：硬删账号（级联删掉本次提交与明细）
curl -s -b /tmp/oj.jar -H 'Content-Type: application/json' -X DELETE \
  -d '{"password":"deploy1234"}' http://127.0.0.1:8080/api/auth/account
```

> 本块可重复执行；若第二次报 **1005（用户名已存在）**，先跑最后一条注销命令，或换一个用户名。

判题侧核对三件事：`status=done`、`verdict=AC`、`caseResults` 里每个用例都有耗时与内存。
编译错误与运行错误的分支用 `tests/run_tests.sh` 覆盖（见下）。

### 8.3 四套自动化验收

| 套件 | 命令 | 前提 |
|---|---|---|
| 单元 | `make test` | 已构建 |
| 端到端（curl） | `tests/run_tests.sh` | 服务在跑 + `jq` + `mysql` 客户端 |
| 端到端（Python） | `python3 tests/python/run_api_tests.py` | 服务在跑 + `requests` |
| 题库同步 | `tests/run_problemsync_test.sh` | 自拉 18080 独立实例，**会临时改 `problems/`** |

全绿 + `logs/app.log` 有逐请求访问行 = 部署完成（对照 §14 清单）。

---

## 9. 日志与排障

### 9.1 日志

| 文件 | 内容 | 策略 |
|---|---|---|
| `logs/app.log` | 启动回显、配置诊断、逐请求访问行、题库同步错误 | 按天切割 `app.log.YYYY-MM-DD`，保留 `OJ_LOG_RETENTION_DAYS`（默认 7）天 |
| `logs/judge.log` | 判题流水线、编译失败、逐用例异常 | 同上 |

- 两个通道**分文件** `[D-34]`；目录内容被 `.gitignore` 忽略（只忽略内容不忽略目录）。
- 启动期错误**同时**打到 stdout（`make run` 看得见）与 `app.log`（后台跑时事后查）。

### 9.2 常见故障速查

| 症状 | 排查顺序 |
|---|---|
| 启动即退出：`配置校验未通过` | 看回显里的 ERROR —— 多半是 ① 闸门 ≠ 2×worker ② 兜底墙钟不等式被破坏 |
| 启动即退出：`数据库连接失败` | `pgrep -x mysqld`；账号密码与 `.env` 是否一致；`mysql -h 127.0.0.1 -uoj -poj oj` |
| 启动即退出：`建表失败` / `题库同步失败` | 是否在仓库根启动；`db/init/01-schema.sql` 与 `problems/` 是否可读 |
| 页面样式/脚本错乱、`does not provide an export` | 浏览器缓存了旧 JS：改前端必须 bump `?v=` 版本号（HTML + js 模块相对导入两处） |
| 页面只剩标题、`DOMPurify is not defined` | `static/vendor/purify.min.js` 是占位/损坏文件 → 重跑 §3 的下载与四步校验 |
| 提交返回 **1009**（判题服务不可用） | 调度器未存活：看启动日志第 9 步是否成功；`judgeAlive` 也在 `/api/admin/overview` 里 |
| 提交返回 **1006 / 1007** | 限流（每分钟 10 次）/ 单用户并发闸（2）—— 属正常行为，等一会再提 |
| 注册返回 **1010** | `OJ_ALLOW_REGISTER=0` 关了注册 |
| 登录返回 **1011** | 账号被管理台停用；管理员可解除停用 |
| verdict=**JE**「测试程序未能运行」 | 平台侧问题：看 `logs/judge.log` 的 harness 生成/编译记录；常见于题库配置错（`__expr` 语法） |
| 构建卡死无输出 | §1：并发过高，降 `-j1` |
| 重启后大量提交变 `failed/JE` | **设计如此**：`judging` 态无法恢复上下文 → 置失败；`pending` 会自动重新入队（`[D-20]`） |
| 端口被占 | 改 `OJ_LISTEN` 或 `ss -ltnp \| grep 8080` 找到占用者 |

### 9.3 运维位置速查

| 想改什么 | 改哪里 | 生效方式 |
|---|---|---|
| 监听地址 / 并发 / 配额 / 注册开关 / 管理员名单 | `.env` | **重启** |
| 题目内容、用例、时限、内存上限 | `problems/<slug>.json`（手工改） | **重启**（启动时同步） |
| 同上（通过管理台） | `admin.html` 题目 tab | **免重启**（写文件 + 单题同步入库） |
| 表结构 | `db/init/01-schema.sql` + `src/main.cpp` 的 `ensure_columns()` | 重启后幂等生效 |
| 前端 JS/CSS | `static/**` | 立即（`Cache-Control: no-cache`），但**必须 bump `?v=`** 否则老浏览器缓存不动 |

---

## 10. 数据与题库运维

### 10.1 题库

- **唯一真源是 `problems/*.json`**；启动时 delete-then-insert 同步入库（单事务），并灌进程内只读缓存。
- 历史提交存的是**快照**，改题/删用例**不影响**历史详情（`submission_case_results` 无外键指向题库表）。
- 录题流程见 SPEC §4.3：写 JSON → 重启 → **用官方解法提交一次全绿** → 再交付别人使用。
- 校验不通过的题会被**跳过**（启动日志点名），不会让服务起不来。

### 10.2 备份 / 清库

```bash
# 备份（题库 JSON 另算，它在 git 里）
mysqldump -h 127.0.0.1 -uoj -poj oj > oj-backup-$(date +%F).sql

# ⚠️ 不可逆：删光全部表与数据（提交历史、账号、收藏全没）
mysql -h 127.0.0.1 -uoj -poj oj < db/reset.sql
# 清完重启服务 → 题库自动重新同步（JSON 还在，录题不需要重来）
```

### 10.3 `oj-reset`（测试数据工具）

```bash
./build/oj-reset --dry-run      # 先看计划：不连库、不写文件
./build/oj-reset                # 清库 + 重建题目/用户/收藏基线（提交三表恒 0 行）
```

- 造出的账号：`admin/admin123`、`ZhangLing/666666`、`alice01…erin05`，并打印可直接注入的 Cookie。
- **不伪造判题结果**（阶段 0 已删除伪造逻辑并加自检：提交三表非 0 直接失败）。
- 自己**不拉起** mysqld；破坏性操作，别在有真实数据的库上跑。

---

## 11. 并发与资源调优

四个数字**互相绑定**，动一个要重算其余（SPEC §12 v1.6 末条）：

```
workers(2)  ←→  gate(4) = 2 × workers  ←→  fence(10)  ←→  性能压测口径
```

| 参数 | 现值 | 调整规则 |
|---|---|---|
| `OJ_JUDGE_WORKERS` | 2 | 与 CPU 核数、内存匹配；`gate` 必须同步 = 2×workers |
| `OJ_JUDGE_GATE` | 4 | 提高后**必须**按 `闸门 × 时限 < 时限 × fence + 5000` 重算 fence，否则合法解法被误判 TLE |
| `OJ_FENCE_MULTIPLIER` | 10 | 同上；`×10` 的上界由「闸门 < 10」决定 |
| `OJ_HTTP_THREADS` | 8 | 2G 机器可降到 4；运行期生效 |
| MySQL `innodb_buffer_pool_size` | 64MB | 2GB 机器不要调回默认 128MB |
| 每题 `mem_limit_kb` | 131072 | **AS 口径**；树/图/大矩阵题逐题调大（如 262144），别套默认值 |

**内存预算参考**（2 核 / 1967MB 机器）：
`2 worker × 2 子进程 × 128MB RLIMIT_AS` 是**虚拟地址**上限（实际占用远小于此），
但 mysqld（64MB pool）+ 8 个 HTTP 线程 + 构建进程必须共存 —— 这就是 §1 不许放开编译并发的原因。

**性能验收换 4G 机器**：2G 档只保证功能正确；NF-1~NF-4 与 AC-38 的延迟数字必须在 4G 机器上采信。

---

## 12. 路线 B（Docker Compose）—— 可选、当前未实现

| 项 | 现状 |
|---|---|
| `Dockerfile` / `docker-compose.yml` | **0 字节占位**（T0.2 未开工，不在关键路径上） |
| 前置条件 | `sudo usermod -aG docker $USER` 后**注销重登录**，否则 `permission denied on /var/run/docker.sock` |

若要实现，必须满足这几条（否则会把 `[D-72]` 想消除的漂移原样复发）：

1. 镜像内装**同一份 httplib 0.60.1**（`/usr/local/include/httplib.h`），不要装 apt 的 0.14.3
2. 工具链锁 `g++-12`，构建仍走 `cmake -S . -B build`
3. MySQL 首次启动用 `/docker-entrypoint-initdb.d/01-schema.sql`，**同时保留**应用启动的幂等建表兜底
4. 配置走 `environment:`（进程环境优先于 `.env`），变量名与 §6 完全一致
5. `db/reset.sql` **不能**放进 init 目录（否则首启建完表又删掉）
6. 日志挂 volume（SPEC §2.1-B：`oj-logs → /var/log/oj/{app,judge}.log`），或改 `OJ_LOG_DIR` 指进挂载点

**HTTPS/反代注意**：首版只开明文 HTTP。若上 nginx + TLS，须同步做三件事（SPEC §6.3.2.1）：
`Set-Cookie` 加 `Secure`、静态与 API 全部走 HTTPS（否则 `SameSite` 视为跨站、Lax 放行规则失效）、
确认代码中无硬编码 `http://` 资源。

---

## 13. 升级与卸载

```bash
# 升级（改配置 + 重启是唯一运维路径）
git pull
make build                       # 内存紧张用 make build JOBS=1
pkill -x oj; make run            # 或 systemctl restart oj
# 启动时自动：建表/补列 → 题库重新同步 → pending 重新入队 → judging 置失败(可重跑)

# 彻底卸载（不可逆）
pkill -x oj
rm -rf build/                    # 构建产物（源码不动）
sudo mysql -e "DROP DATABASE IF EXISTS oj;"
rm -rf /tmp/oj-judge/*           # 判题工作区残留
```

---

## 14. 部署完成检查清单

- [ ] §2.4 七项环境验证全绿（含 rlimit 探针）
- [ ] §3.1 vendored 文件四步校验通过（无 <1KB 文件、符号可 grep、`node --check` 通过、HTML 引用全对账）
- [ ] MySQL 已启动，`innodb_buffer_pool_size=64M`（并已写入配置文件固化）
- [ ] `mysql -h 127.0.0.1 -uoj -poj oj -e "SELECT VERSION();"` 正常
- [ ] `make build` 成功产出 `build/oj`，configure 打印的 httplib 命中 `/usr/local/include` 0.60.1
- [ ] `make test` 417 条全绿
- [ ] `.env` 三条硬校验通过，服务启动回显到第 10 步 `listen`
- [ ] `GET /`、`GET /list.html`、`GET /api/problems` 返回 200
- [ ] 真实提交一条 AC + 一条 CE，逐用例表与编译诊断正常
- [ ] `tests/run_tests.sh` 全绿
- [ ] `logs/app.log` 与 `logs/judge.log` 有内容、按天切割生效
- [ ] （可选）管理员按 §6.2 流程启用，`admin.html` 四 tab 可访问
