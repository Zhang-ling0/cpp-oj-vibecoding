# dependence.md — CppOJ 开发环境依赖

Ubuntu 24.04 (noble) · amd64 · 对应 SPEC v1.7 §6.7

---

## 1. apt 安装

```bash
sudo apt-get update && sudo apt-get install -y \
  ca-certificates curl git \
  g++-12 gcc-12 make libc6-dev \
  nlohmann-json3-dev libcpp-httplib-dev libmysqlclient-dev libssl-dev \
  mysql-server mysql-client
```

| 用途 | 包 |
|---|---|
| 引导 | `ca-certificates`（HTTPS 根证书）`curl`（下载器）`git`（版本库） |
| 工具链 | `g++-12` `gcc-12`（判题编译器，锁定 12）`make`（构建入口）`libc6-dev`（POSIX 头文件） |
| 第三方库 | `nlohmann-json3-dev`（JSON）`libcpp-httplib-dev`（HTTP）`libmysqlclient-dev`（MySQL）`libssl-dev`（密码哈希 Blowfish） |
| 数据库 | `mysql-server` `mysql-client`（验收脚本跑 SQL 用） |

**明确不装**：

| 包 | 原因 |
|---|---|
| `cmake` | 构建系统已定为 `Makefile`，两个并存只会让人不知用哪个 |
| `pkg-config` | 四个库的头文件/库文件路径都固定，Makefile 直接写 `-lmysqlclient -lssl -lcrypto` |
| `build-essential` | 会连带装 **g++-13**，与"判题编译器锁 g++-12"冲突 |
| `libbcrypt-dev` | **24.04 官方源无此包**（`apt-cache search bcrypt` 无结果）。密码哈希改用 OpenSSL Blowfish 自行实现（SPEC §6.2.1） |

> `libc6-dev` 虽由 `g++-12 → libstdc++-12-dev → libc6-dev` 传递带入，仍建议显式安装 —— judge 用到的 `getrusage`(`<sys/resource.h>`)、`wait4`(`<sys/wait.h>`)、`prctl`(`<sys/prctl.h>`) 头文件全来自它。

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

---

## 5. 验证（6 项，全绿再继续）

```bash
g++-12 --version | head -1                             # 应为 12.x
dpkg -L libcpp-httplib-dev | grep httplib.h           # 应为 /usr/include/httplib.h
ls /usr/include/{nlohmann/json.hpp,mysql/mysql.h,openssl/blowfish.h}
ls /usr/include/sys/{resource.h,wait.h,prctl.h}       # judge 依赖的 POSIX 头文件
find static/vendor -name '*.js' -size -1k | wc -l      # 应为 0
mysql -h 127.0.0.1 -uoj -poj oj -e "SELECT VERSION();"
```

---

详细说明见 `SPEC.md` §6.7。
