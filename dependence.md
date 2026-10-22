# dependence.md — CppOJ 开发环境依赖

Ubuntu 24.04 (noble) · amd64 · 对应 SPEC v1.9 §6.7

---

## 1. apt 安装

```bash
sudo apt-get update && sudo apt-get install -y \
  ca-certificates curl git \
  g++-12 gcc-12 make cmake libc6-dev \
  nlohmann-json3-dev libmysqlclient-dev libssl-dev \
  libgtest-dev \
  mysql-server mysql-client

# ---------- cpp-httplib：header-only 单文件，不走 apt ----------
# 装 0.60.1（[D-72] v1.9 修订的真源）。本机已装好，重装机器时执行这一行即可。
curl -sSL -o /tmp/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.60.1/httplib.h
sudo install -m 644 /tmp/httplib.h /usr/local/include/httplib.h
grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h   # 应为 "0.60.1"
```

| 用途 | 包 |
|---|---|
| 引导 | `ca-certificates`（HTTPS 根证书）`curl`（下载器）`git`（版本库） |
| 工具链 | `g++-12` `gcc-12`（判题编译器，锁定 12）`cmake`（**v1.8 起的唯一构建入口**）`make`（`make build/run/test/clean` 那层入口）`libc6-dev`（POSIX 头文件） |
| 第三方库 | `nlohmann-json3-dev`（JSON）`libmysqlclient-dev`（MySQL）`libssl-dev`（密码哈希 Blowfish） |
| HTTP | **httplib.h 0.60.1（自装单头文件，`/usr/local/include/`）** `[v1.9]` |
| 测试 | `libgtest-dev`（gtest 1.14，自带 `GTestConfig.cmake`，`ctest` 直接可用） |
| 数据库 | `mysql-server` `mysql-client`（验收脚本跑 SQL 用） |

**明确不装**：

| 包 | 原因 |
|---|---|
| `pkg-config` | 四个库的头文件/库文件路径都固定，CMake 用 `find_package` / `find_path` 直接找 |
| `build-essential` | 会连带装 **g++-13**，与"判题编译器锁 g++-12"冲突（24.04 的默认 `g++` 就是 13） |
| `libbcrypt-dev` | **24.04 官方源无此包**（`apt-cache search bcrypt` 无结果）。密码哈希改用 OpenSSL Blowfish 自行实现（SPEC §6.2.1） |
| **`libcpp-httplib-dev`** | **v1.9**：本机用的是自装的 0.60.1；误装 apt 的 0.14.3 会让 `find_path` 命中 `/usr/include` 而静默用旧版 —— 版本漂移正是 `[D-72]` 想消除的东西 |

> `libc6-dev` 虽由 `g++-12 → libstdc++-12-dev → libc6-dev` 传递带入，仍建议显式安装 —— judge 用到的 `getrusage`(`<sys/resource.h>`)、`wait4`(`<sys/wait.h>`)、`prctl`(`<sys/prctl.h>`) 头文件全来自它。

> **`cmake` 曾被列进「不装」**（v1.7 的理由是"与 Makefile 并存只会让人不知用哪个"）。v1.8 起 `CMakeLists.txt` 是唯一构建规则、`Makefile` 只剩 15 行转发，那条顾虑不存在，于是移回必需。

## 1.1 构建与测试（v1.8）

```bash
make build        # == cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-12 && cmake --build build -j
make test         # == ctest --test-dir build --output-on-failure
make test ARGS='-R Admin'    # 只跑名字匹配的用例
make run          # 拉起 mysqld（若未运行）后启动 build/oj
make clean
```

| 依赖 | CMake 怎么找 | 缺了 / 变了会怎样 |
|---|---|---|
| Threads / OpenSSL / nlohmann_json / GTest | `find_package`（apt 包都自带 CONFIG） | 配置阶段 `FATAL_ERROR` |
| MySQL C Connector | `find_path(mysql/mysql.h)` + `find_library(mysqlclient)` | 仅 `WARNING`，等 `dao.cpp` 真的 `#include <mysql/mysql.h>` 才失败 |
| cpp-httplib 0.60.1 | `find_path(httplib.h)` → 命中 `/usr/local/include` | configure 打印**路径 + 版本**；版本 ≠ 0.60.1 时 `WARNING` 点名 |
| cpp-httplib 兜底 | 上一条没命中 → 用 `third_party/httplib`（**0.14.3**） | `WARNING` 提示"版本不一致，视为环境未就绪"；两者都没有才 `FATAL_ERROR` |

> **`[D-72]` v1.9 的口径**：httplib 真源是本机 `/usr/local/include/httplib.h` **0.60.1**，不是 apt 的 0.14.3（实测 apt 包从未安装）。头文件是 header-only 单文件，"安装"= 放一个文件，无需编译链接。仓库内 `third_party/httplib/httplib.h`（0.14.3）**只作为环境缺失时的兜底**，不再是版本真源。路线 B 的镜像必须自带同一份 0.60.1。

### 1.2 编辑器 / 静态分析（v1.9）

`CMakeLists.txt` 打开了 `CMAKE_EXPORT_COMPILE_COMMANDS`，产物是 `build/compile_commands.json`，并在**仓库根目录建一个相对软链接**指向它（每次 configure 自动重建，删了也会自愈）。

```bash
readlink compile_commands.json      # → build/compile_commands.json
python3 -m json.tool compile_commands.json > /dev/null   # 23 条：ojcore 21 + oj 1 + config_test 1
```

**约定：构建只在终端里 `make build`，VSCode 里不点 configure。**

| 事项 | 说明 |
|---|---|
| 为什么需要它 | cpptools 需要 `-I<root>/include` 才能解析 `#include "common/config.h"`；缺了就是每个源文件第一行报错 + 跨头文件跳转失效 |
| 为什么软链接在根目录 | cpptools 从**工作区根目录及上级**找该文件，而 `build/` 是 `src/` 的兄弟目录不是祖先，向上找找不到 |
| 为什么关掉 CMake Tools 的自动 configure | 它默认在 `.vscode/build` 另起一套构建目录，与 `make build` 的 `build/` 变成两套产物、两个 compdb；`.vscode/settings.json` 已关掉它并把 build 目录指到 `build/` |
| 换构建目录 | `cmake -S . -B /tmp/x` 后软链接会自动改指向该目录的 compdb（相对路径），仍可用 |
| 撞到「根目录已存在非符号链接的 compile_commands.json」 | 说明有人手写了它；CMake 不会覆盖，需要自己删掉或把真实 compdb 挪到别处 |

> 装了 `clang-format` 但仓库里**没有** `.clangformat`，本项目暂不做代码风格统一 —— 那是独立决定（涉及 §10.3 的可讲述性），不夹带进构建配置。clangd 也未安装；VSCode 用的是自带的 cpptools 1.34.4。

---

## 2. 前端库（7 个文件，vendored 到 `static/vendor/`）

⚠️ **必须用 `/npm/` 路径** —— `/gh/` 实测对 `codemirror.min.js` 与 `marked.min.js` 均返回 **404**。

```bash
cd /home/ZL/project/cpp-oj-vibcoding
V=5.65.16; B=https://cdn.jsdelivr.net/npm
mkdir -p static/vendor/cm/{mode/clike,addon/edit,addon/comment}

curl -sSL -o static/vendor/cm/codemirror.min.js                $B/codemirror@$V/lib/codemirror.min.js
curl -sSL -o static/vendor/cm/mode/clike/clike.min.js         $B/codemirror@$V/mode/clike/clike.min.js
curl -sSL -o static/vendor/cm/addon/edit/matchbrackets.min.js $B/codemirror@$V/addon/edit/matchbrackets.min.js
curl -sSL -o static/vendor/cm/addon/edit/closebrackets.min.js $B/codemirror@$V/addon/edit/closebrackets.min.js
curl -sSL -o static/vendor/cm/addon/comment/comment.min.js     $B/codemirror@$V/addon/comment/comment.min.js
curl -sSL -o static/vendor/marked.min.js                       $B/marked@12.0.2/marked.min.js
curl -sSL -o static/vendor/purify.min.js                       $B/DOMPurify@3.1.6/dist/purify.min.js

# 完整性自检：应无任何输出（404 响应体只有 50~90 字节）
find static/vendor -name '*.js' -size -1k -print
```

**CodeMirror 5 不是单文件** —— 核心 `codemirror.min.js` 实测 `clike` 出现 0 次，必须额外加载 `mode/clike`（C++ 高亮）与 3 个 addon。加载顺序 `core → mode → addon`，全部加 `defer`。

| 缺哪个 | 后果 |
|---|---|
| `codemirror.min.js` | 编辑器完全不出现 |
| `mode/clike/clike.min.js` | 无 C++ 语法高亮 |
| `addon/edit/matchbrackets.min.js` | ⚠️ `matchBrackets` **静默失效，不报错** |
| `addon/edit/closebrackets.min.js` | 无自动补全 `{}` |
| `addon/comment/comment.min.js` | `Ctrl+/` 无效 |
| `marked.min.js` | 题面显示原始 Markdown 源码 |
| `purify.min.js` | ⚠️ **XSS 防护缺口** |

---

## 3. MySQL 初始化

```bash
sudo mysqld --initialize-insecure --user=mysql
sudo mysqld_safe &

# 低内存机器必调，否则会与 judge 抢内存、成为 OOM killer 首选目标
sudo mysql -e "SET GLOBAL innodb_buffer_pool_size = 67108864;"   # 64MB

# 建库建号（数据表由应用启动时幂等创建）
sudo mysql -e "CREATE DATABASE IF NOT EXISTS oj DEFAULT CHARSET utf8mb4;
                CREATE USER IF NOT EXISTS 'oj'@'127.0.0.1' IDENTIFIED BY 'oj';
                GRANT ALL ON oj.* TO 'oj'@'127.0.0.1'; FLUSH PRIVILEGES;"
```

---

## 4. 运行时配置 `.env`

不是依赖，但**是跑起来的必需项**。放在项目根目录（与 `dependence.md` 同级）。

```bash
OJ_ALLOW_REGISTER=1
OJ_ADMIN_USERNAMES=alise
MYSQL_HOST=127.0.0.1
MYSQL_PORT=3306
MYSQL_USER=oj
MYSQL_PASSWORD=oj
MYSQL_DB=oj
OJ_LISTEN=0.0.0.0:8080
OJ_JUDGE_WORKERS=2
OJ_JUDGE_GATE=4
OJ_FENCE_MULTIPLIER=10
```

### 四个需要说明的配置项

| 变量 | 说明 |
|---|---|
| `OJ_ALLOW_REGISTER` | `1` = 开放注册；`0` = 关闭注册端点。关闭后登录页的注册 tab **仍然显示**，用户提交时收到错误码 1010 |
| `OJ_ADMIN_USERNAMES` | 管理员名单，逗号分隔，**角色的唯一真源**。见下方详细说明 |
| `OJ_JUDGE_GATE` | 全局并发闸门，**必须 = 2 × `OJ_JUDGE_WORKERS`**。见下方详细说明 |
| `OJ_FENCE_MULTIPLIER` | 兜底墙钟系数。改闸门前**必须重算**。见下方详细说明 |

#### `OJ_ADMIN_USERNAMES`

- 名单里的用户在**服务启动时**被同步成 `role='admin'`，改动需**重启**才生效
- ⚠️ **名单里的用户名必须已存在**。填一个没注册过的名字**不会创建该用户**，只在启动日志里 `WARN` 点名 —— 所以顺序是「先注册 → 再把名字加进来 → 重启」
- 名单外的所有用户会在启动时被降级为普通用户（**双向同步**，不是只提权）
- 变量**未设置**时完全跳过同步、不动任何 `role`；**显式留空串**则全部降为普通用户

#### `OJ_JUDGE_GATE`

必须严格等于 `2 × OJ_JUDGE_WORKERS`（本机 2 核 → 2 和 4）。

- 等于 `worker × 2`：每个 worker 最多同时派生 2 个子进程，且不会有 worker 一次占满全部额度把其他 worker 饿死
- 等于 `worker`：并发退化为 1，一次只判一题，吞吐减半
- 大于 `worker × 2`：worker 数不再是并发瓶颈，但 2 核机器上墙钟放大倍数上升，逼近兜底墙钟

#### `OJ_FENCE_MULTIPLIER`

兜底墙钟阈值 = `time_limit_ms × OJ_FENCE_MULTIPLIER + 5000`。必须满足约束：

```
闸门 × time_limit_ms  <  兜底墙钟阈值
```

当前取值：`4 × 3000ms = 12s  <  3 × 10 + 5 = 35s`  ✓（余量 2.9 倍）

⚠️ **将来调高 `OJ_JUDGE_GATE` 时必须同步调高 `OJ_FENCE_MULTIPLIER`**，否则并发评测时 CPU 用量高但完全合法的解法会被误判为 TLE（墙钟被分核拉长后超过兜底阈值）。详见 SPEC §5.4.2.1。

#### 可选键（仓库 `.env` 已预置，SPEC 未列为必填）

下面这些键 SPEC 没有硬性要求，取值默认落在 SPEC 已定的常量上，留成环境变量只是为了测试隔离与临时调参：

| 变量 | 默认 | 来源 |
|---|---|---|
| `OJ_JUDGE_USER_GATE` | 2 | §1.4 单用户同时评测数上限 |
| `OJ_HTTP_THREADS` | 4 | §2.3 httplib 线程池（阶段 3 起默认 4） |
| `OJ_JUDGE_WORKDIR` | `/tmp/oj-judge` | §5.1 工作区 |
| `OJ_PROBLEMS_DIR` / `OJ_STATIC_DIR` | `./problems` / `./static` | §2.1-A |
| `OJ_LOG_DIR` / `OJ_LOG_RETENTION_DAYS` | `./logs` / 7 | §1.4 可观测 |
| `OJ_SESSION_TTL_DAYS` | 7 | §1.3 P1 |
| `OJ_MAX_CODE_BYTES` | 65536 | §5.8 |
| `OJ_RATE_WINDOW_SECONDS` / `OJ_RATE_MAX_SUBMITS` | 60 / 10 | §5.8 |

> `.env` 支持 `export KEY=VALUE`、引号包裹值、以及**行内注释**（仅「空白之后的 `#` 才算注释，所以 `MYSQL_PASSWORD=p#ss` 不会被截断）。
> 校验由 `src/common/config.cpp` 在启动时执行：值非法 → `WARN` 并回落默认值；**闸门 ≠ 2 × workers 或兜底墙钟不等式被破坏 → `ERROR` 并拒绝启动**（详见 SPEC §2.3 / §5.4.2.1）。

---

## 5. 验证（7 项，全绿再继续）

```bash
g++-12 --version | head -1                             # 应为 12.x
cmake --version | head -1                              # 应为 ≥ 3.20（实测 3.28.3）
ls /usr/include/gtest/gtest.h                          # 单测依赖
grep -m1 CPPHTTPLIB_VERSION /usr/local/include/httplib.h   # 应为 "0.60.1"（不是 apt 的 0.14.3）
ls /usr/include/{nlohmann/json.hpp,mysql/mysql.h,openssl/blowfish.h}
ls /usr/include/sys/{resource.h,wait.h,prctl.h}       # judge 依赖的 POSIX 头文件
find static/vendor -name '*.js' -size -1k | wc -l      # 应为 0
mysql -h 127.0.0.1 -uoj -poj oj -e "SELECT VERSION();"
```

构建自检：`make build && make test` —— 应产出 `build/oj`，ctest 全绿（当前 44 条 config 单测）。

---

详细说明见 `SPEC.md` §6.7。
