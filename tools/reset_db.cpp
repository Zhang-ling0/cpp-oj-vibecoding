// =============================================================================
// CppOJ 数据库重置 + 造数工具（可执行文件 oj-reset）
//
// 【为什么需要它】《web自动化测试文档.md》§3.1/§3.2 的实测基线里，
//   提交记录 0 条、题库 1 道题、用户 7 个 —— 于是 §3.2 列出的 6 类断言
//   （难度筛选 / 分页 / 仅看收藏 / 掌握分布 total=0 / 标签排序 / 结果面板）
//   在本实例上【无法证伪】，文档明确写着"不得记为通过"。
//   §3.3 给的临时手段是手工 SQL 造数；本工具把那段过程固化成一条可重复执行的命令：
//   清空 11 张表 → 重建题库 → 造用户/会话/收藏/提交/逐用例明细 → 打印核对结果。
//
// 【用法】（必须从项目根目录执行，.env 与 problems/ 都是相对路径）
//   make build JOBS=2 && ./build/oj-reset            # 真跑
//   ./build/oj-reset --dry-run                       # 只打印将要做什么
//   ./build/oj-reset --env /path/other.env            # 换一份配置
//
// 【造出来的数据对应文档里的哪些用例】（详见末尾回显）
//   · ≥25 道题、easy/medium/hard 三档齐全、标签多样
//       → 解除 WT-LS-04 / WT-LS-09 / WT-LS-22 / WT-MG-03 / WT-MG-05 的 🔴 与 🟢 部分
//   · 6 种 verdict + failed/JE 的提交与逐用例明细、CE 的编译诊断行
//       → 解除 WT-RS-01~10 / WT-HR-01~16 的渲染分支（§3.3 路径 B）
//   · 31 条提交（> 每页 20 条）→ 解除 WT-HR-02 的分页
//   · 7 个账号（admin 1 + 普通 6）+ 可直接注入 Cookie 的会话行
//       → WT-TB-03 / WT-AD-04（共 7 人）/ WT-AC-06（双会话）/ 会话过期用例
//
// 【为什么走 problems/*.json 而不是直接 INSERT problems】
//   D-30 定的是"题库文件为源、启动时同步入库"。直接往 problems 表插行虽然更省事，
//   但库里会出现【文件里不存在】的题：加载器下次启动既不会认它、也不会清它，
//   于是"哪道题是真的"这件事在两处真源之间悬着。写文件 + 复用 problem::load_all()
//   则全程只经过 D-64 那套校验，坏题当场被跳过并报出原因，与正式启动路径完全一致。
//
// 【本机 2 核 / 1967MB 的三条自我约束（SPEC §0.0）】
//   ① 不申请大块虚拟内存：所有数据是流式写库的（一条提交写完就丢），
//      常驻的只有 26 道题的元信息与用例快照（约几十 KB），没有大数组、没有预留容量。
//   ② 不编译任何东西：不调 g++、不起 judge worker。判题相关数据一律【按 judge 写回后的
//      样子】直接落库（§3.3 路径 B），否则在这台机器上编译 + 评测会把服务卡死。
//   ③ 不起线程：连接池 size=1（[D-21] 本就串行化），全部语句在主线程顺序执行。
//
// 【它会做什么不可逆的事】11 张表全部清空（含 users、submissions），
//   与 db/reset.sql 同一量级。执行前请确认 oj 库里没有需要保留的数据，
//   并【先停掉 ./build/oj】：它持有的是自己的进程内题库缓存，改完库不重启会读到旧题库。
// =============================================================================
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"
#include "problem/loader.h"
#include "web/bcrypt.h"

namespace {

using oj::db::Session;
using oj::db::Value;
using json = nlohmann::json;

// ---- 运行期统计（只用于回显）--------------------------------------------------
struct Stats {
  int users = 0;
  int sessions = 0;
  int favorites = 0;
  int submissions = 0;
  int case_rows = 0;
  int compile_rows = 0;
  int problem_files_written = 0;
  int problem_files_kept = 0;
};

Stats g_stats;
bool g_dry_run = false;
bool g_regen_problems = false;  // 见 --regen-problems

void step(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void step(const char* fmt, ...) {
  std::va_list args;
  va_start(args, fmt);
  std::printf("  ");
  std::vprintf(fmt, args);
  std::printf("\n");
  va_end(args);
}

// 任何一步失败都立刻退出：半套数据比"什么都没有"更难排查（与 main.cpp 同一判断）。
// ⚠️ 这里【不】调 Pool::shutdown()：die() 会被调用点在持有租约时触发，而 shutdown()
//   要先拿全局互斥量才肯关连接 —— 那把锁正攥在本线程手里，于是它会一直等下去（自死锁）。
//   一次性工具的连接交给进程退出回收即可，没有"干净收尾"的需求。
void die(const std::string& what) {
  std::fprintf(stderr, "[oj-reset] 失败：%s\n", what.c_str());
  oj::common::log_error(oj::common::LogChannel::kApp, "造数失败：" + what);
  std::exit(1);
}

// 一次性取 insert_id / affected_rows 的小助手：把"查询 → 判断 → 取值"压成三行。
unsigned long long last_insert_id(Session& s, const std::string& what) {
  const oj::db::ResultSet rows = s.run("SELECT LAST_INSERT_ID() AS id");
  if (rows.failed() || rows.empty()) die(what + "：读 LAST_INSERT_ID 失败：" + rows.error().format());
  return rows.at(0).u64(0);
}

void must_exec(Session& s, const std::string& sql, const std::string& what) {
  if (!s.exec(sql)) die(what + "：" + s.error().format());
}

void must_run(Session& s, const std::string& sql, const std::vector<Value>& params,
              const std::string& what) {
  const oj::db::ResultSet rows = s.run(sql, params);
  if (rows.failed()) die(what + "：" + rows.error().format());
}

// =============================================================================
// 时间：DATETIME 列存 UTC（D-17），且连接已固定 time_zone='+00:00'（mysql_pool.h）
//   —— 所以这里在应用侧算出绝对时间戳再作为文本下发，不用 DATE_SUB(NOW(), INTERVAL ?)
//   这类"占位符夹在 INTERVAL 里"的写法（不同 MySQL 版本行为不一致，踩了不好查）。
// =============================================================================
std::string utc_stamp(long minutes_ago, long seconds_offset = 0) {
  const std::time_t when =
      std::time(nullptr) - minutes_ago * 60 + seconds_offset;
  struct tm parts;
  ::gmtime_r(&when, &parts);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return buffer;
}

// 会话 token：64 个小写 hex（§6.3.2 的形状）。这里用可读的固定串而不是随机数，
//   理由是【可复现】：造完数之后，测试用例可以直接把 token 写进 Playwright 的
//   storageState，不用先登录一次。session.cpp 的 is_token_wellformed() 只查
//   "64 个 [0-9a-f]"，本函数按位取 hex 因此一定合法。
std::string fixed_token(const std::string& tag) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const unsigned char c : tag) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0f]);
  }
  while (out.size() < 64) out.push_back('0');
  return out.substr(0, 64);
}

// =============================================================================
// 题库种子：25 道新题（加上仓库自带的 two-sum 共 26 道，满足 ≥25 的分页要求）
//
//   表里每项只写【差异】——slug/标题/难度/标签/一句任务描述；题面、签名、模板、
//   用例、期望值全部由 op 推导。这样加一道题是一行，而不是一份 JSON。
// =============================================================================
enum class Op {
  kSum, kMax, kMin, kCountEven, kCountOdd, kSumPositive, kMaxAbs, kIndexOfMax,
  kSecondMax, kSumSquares, kMaxMinDiff, kFirstNegative, kCountDistinct,
  kReplaceNegatives,
  kReverseString, kToUpper, kIsPalindrome, kCountVowels, kLongestWordLength, kCountSpaces,
  kBinarySearch, kCountLess, kCountGreater, kCountEqual, kRotateLeft,
};

// 形参形状：决定 signature / params / code_template 怎么拼，以及期望值怎么算。
enum class Shape { kVecToInt, kVecToVec, kStrToStr, kStrToBool, kStrToInt, kVecIntToInt, kVecIntToVec };

struct OpSpec {
  Shape shape;
  const char* method;
  const char* return_type;
};

OpSpec spec_of(Op op) {
  switch (op) {
    case Op::kSum:              return {Shape::kVecToInt, "sumArray", "int"};
    case Op::kMax:              return {Shape::kVecToInt, "maxElement", "int"};
    case Op::kMin:              return {Shape::kVecToInt, "minElement", "int"};
    case Op::kCountEven:        return {Shape::kVecToInt, "countEven", "int"};
    case Op::kCountOdd:         return {Shape::kVecToInt, "countOdd", "int"};
    case Op::kSumPositive:      return {Shape::kVecToInt, "sumPositive", "int"};
    case Op::kMaxAbs:           return {Shape::kVecToInt, "maxAbs", "int"};
    case Op::kIndexOfMax:       return {Shape::kVecToInt, "indexOfMax", "int"};
    case Op::kSecondMax:        return {Shape::kVecToInt, "secondMax", "int"};
    case Op::kSumSquares:       return {Shape::kVecToInt, "sumSquares", "int"};
    case Op::kMaxMinDiff:       return {Shape::kVecToInt, "maxMinDiff", "int"};
    case Op::kFirstNegative:    return {Shape::kVecToInt, "firstNegative", "int"};
    case Op::kCountDistinct:    return {Shape::kVecToInt, "countDistinct", "int"};
    case Op::kReplaceNegatives: return {Shape::kVecToVec, "replaceNegatives", "std::vector<int>"};
    case Op::kReverseString:    return {Shape::kStrToStr, "reverseString", "std::string"};
    case Op::kToUpper:          return {Shape::kStrToStr, "toUpper", "std::string"};
    case Op::kIsPalindrome:     return {Shape::kStrToBool, "isPalindrome", "bool"};
    case Op::kCountVowels:      return {Shape::kStrToInt, "countVowels", "int"};
    case Op::kLongestWordLength:return {Shape::kStrToInt, "longestWordLength", "int"};
    case Op::kCountSpaces:      return {Shape::kStrToInt, "countSpaces", "int"};
    case Op::kBinarySearch:     return {Shape::kVecIntToInt, "binarySearch", "int"};
    case Op::kCountLess:        return {Shape::kVecIntToInt, "countLess", "int"};
    case Op::kCountGreater:     return {Shape::kVecIntToInt, "countGreater", "int"};
    case Op::kCountEqual:       return {Shape::kVecIntToInt, "countEqual", "int"};
    case Op::kRotateLeft:       return {Shape::kVecIntToVec, "rotateLeft", "std::vector<int>"};
  }
  return {Shape::kVecToInt, "unknown", "int"};  // 不会走到
}

struct ProblemSeed {
  const char* slug;
  const char* title;
  const char* difficulty;
  const char* algo_tags;    // 逗号分隔
  const char* source_tags;  // 逗号分隔
  Op op;
  const char* desc;  // 题面里的一句话任务描述
};

const ProblemSeed kSeeds[] = {
    {"array-sum", "数组元素之和", "easy", "数组", "基础练习", Op::kSum,
     "给定一个整数数组 `nums`，返回所有元素的和。"},
    {"array-max", "数组最大值", "easy", "数组", "基础练习", Op::kMax,
     "给定一个整数数组 `nums`，返回其中最大的元素。"},
    {"array-min", "数组最小值", "easy", "数组", "基础练习", Op::kMin,
     "给定一个整数数组 `nums`，返回其中最小的元素。"},
    {"count-even", "统计偶数个数", "easy", "数组", "基础练习", Op::kCountEven,
     "给定一个整数数组 `nums`，返回其中偶数元素的数量。"},
    {"count-odd", "统计奇数个数", "easy", "数组", "基础练习", Op::kCountOdd,
     "给定一个整数数组 `nums`，返回其中奇数元素的数量。"},
    {"sum-positive", "正数求和", "easy", "数组", "基础练习", Op::kSumPositive,
     "给定一个整数数组 `nums`，返回其中所有正数的和（没有正数时返回 0）。"},
    {"max-abs", "绝对值最大的元素", "easy", "数组,模拟", "基础练习", Op::kMaxAbs,
     "给定一个整数数组 `nums`，返回绝对值最大的元素本身（保留原符号）。"},
    {"index-of-max", "最大值的下标", "medium", "数组", "高频面试题", Op::kIndexOfMax,
     "给定一个非空整数数组 `nums`，返回最大值第一次出现的下标。"},
    {"second-max", "第二大值", "medium", "数组,排序", "高频面试题", Op::kSecondMax,
     "给定一个至少两个元素的整数数组 `nums`，返回降序排列后的第二个元素。"},
    {"sum-squares", "平方和", "easy", "数组,数学", "基础练习", Op::kSumSquares,
     "给定一个整数数组 `nums`，返回每个元素平方之后的总和。"},
    {"max-min-diff", "最大值与最小值之差", "easy", "数组", "基础练习", Op::kMaxMinDiff,
     "给定一个非空整数数组 `nums`，返回最大值与最小值的差。"},
    {"first-negative", "第一个负数的下标", "hard", "数组", "基础练习", Op::kFirstNegative,
     "给定一个整数数组 `nums`，返回第一个负元素的下标；没有负数时返回 -1。"},
    {"count-distinct", "不同元素个数", "medium", "数组,哈希表", "高频面试题", Op::kCountDistinct,
     "给定一个整数数组 `nums`，返回其中不同整数的个数。"},
    {"replace-negatives", "把负数替换为 0", "easy", "数组", "基础练习", Op::kReplaceNegatives,
     "给定一个整数数组 `nums`，返回一个新数组，其中所有负数都被替换为 0。"},
    {"reverse-string", "反转字符串", "easy", "字符串,双指针", "基础练习", Op::kReverseString,
     "给定一个字符串 `s`，返回它的逆序字符串。"},
    {"to-upper", "转成大写", "medium", "字符串", "基础练习", Op::kToUpper,
     "给定一个字符串 `s`，返回其中所有英文字母转成大写之后的结果。"},
    {"is-palindrome", "判断回文串", "medium", "字符串,双指针", "高频面试题", Op::kIsPalindrome,
     "给定一个字符串 `s`，判断它从左往右读与从右往左读是否相同。"},
    {"count-vowels", "统计元音字母个数", "hard", "字符串", "基础练习", Op::kCountVowels,
     "给定一个字符串 `s`，返回其中元音字母（a、e、i、o、u，不区分大小写）的个数。"},
    {"longest-word-length", "最长单词长度", "hard", "字符串,模拟", "外企面经", Op::kLongestWordLength,
     "给定一个用空格分隔的字符串 `s`，返回其中最长单词的字符个数。"},
    {"count-spaces", "统计空格个数", "hard", "字符串,模拟", "基础练习", Op::kCountSpaces,
     "给定一个字符串 `s`，返回其中空格字符的个数。"},
    {"binary-search", "二分查找", "medium", "数组,二分查找", "高频面试题", Op::kBinarySearch,
     "给定一个【升序】整数数组 `nums` 与目标值 `k`，返回 `k` 的下标；不存在时返回 -1。"},
    {"count-less", "小于阈值的个数", "easy", "数组", "基础练习", Op::kCountLess,
     "给定一个整数数组 `nums` 与阈值 `k`，返回其中严格小于 `k` 的元素个数。"},
    {"count-greater", "大于阈值的个数", "easy", "数组", "基础练习", Op::kCountGreater,
     "给定一个整数数组 `nums` 与阈值 `k`，返回其中严格大于 `k` 的元素个数。"},
    {"count-equal", "等于阈值的个数", "medium", "数组,哈希表", "基础练习", Op::kCountEqual,
     "给定一个整数数组 `nums` 与目标值 `k`，返回其中等于 `k` 的元素个数。"},
    {"rotate-left", "数组左旋 k 位", "hard", "数组,双指针", "高频面试题", Op::kRotateLeft,
     "给定一个整数数组 `nums` 与整数 `k`，返回把 `nums` 左旋 `k` 位后的新数组（`k` 可超出长度，按取模处理）。"},
};
constexpr int kSeedCount = static_cast<int>(sizeof(kSeeds) / sizeof(kSeeds[0]));

// ---- 用例池 ------------------------------------------------------------------
// 造数用的输入池：长度 3~5、含负数与 0、有一组含重复值（让 countDistinct 有意义）。
const std::vector<std::vector<int>> kVecPool = {
    {1, 2, 3, 4, 5}, {-2, 7, 0, 5, 11}, {9, -4, 3}, {100, 200, 300},
    {-1, -2, -3, -4, -5},             {42, 7, 13}, {4, 4, 9, 9, 2},
};
const std::vector<std::string> kStrPool = {"hello", "level", "racecar", "Cpp OJ", "a man a plan"};
const char* kCaseBriefs[] = {"基础用例", "含负数与零", "较大数值或长输入"};

std::string join_numbers(const std::vector<int>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(values[i]);
  }
  return out + "]";
}

// 逐 op 求期望值。全部是 O(n) 的朴素实现 —— 这不是"标准答案"，只是让用例自洽：
// 期望值必须与题面一致，否则将来 judge 接线后这批数据会自己变成假阳性/假阴性的来源。
json eval_op(Op op, const std::vector<int>& v, int k, const std::string& s) {
  switch (op) {
    case Op::kSum: {
      long long sum = 0;
      for (const int x : v) sum += x;
      return json(static_cast<int>(sum));
    }
    case Op::kMax: return json(*std::max_element(v.begin(), v.end()));
    case Op::kMin: return json(*std::min_element(v.begin(), v.end()));
    case Op::kCountEven: {
      int n = 0;
      for (const int x : v) n += (x % 2 == 0) ? 1 : 0;
      return json(n);
    }
    case Op::kCountOdd: {
      int n = 0;
      for (const int x : v) n += (x % 2 != 0) ? 1 : 0;
      return json(n);
    }
    case Op::kSumPositive: {
      int sum = 0;
      for (const int x : v) if (x > 0) sum += x;
      return json(sum);
    }
    case Op::kMaxAbs: {
      int best = v.front();
      for (const int x : v) {
        if (std::abs(x) > std::abs(best)) best = x;
      }
      return json(best);
    }
    case Op::kIndexOfMax: {
      int best = 0;
      for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) best = static_cast<int>(i);
      }
      return json(best);
    }
    case Op::kSecondMax: {
      std::vector<int> sorted = v;
      std::sort(sorted.rbegin(), sorted.rend());
      return json(sorted.at(1));
    }
    case Op::kSumSquares: {
      long long sum = 0;
      for (const int x : v) sum += static_cast<long long>(x) * x;
      return json(static_cast<int>(sum));
    }
    case Op::kMaxMinDiff:
      return json(*std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end()));
    case Op::kFirstNegative: {
      for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] < 0) return json(static_cast<int>(i));
      }
      return json(-1);
    }
    case Op::kCountDistinct: {
      std::vector<int> sorted = v;
      std::sort(sorted.begin(), sorted.end());
      sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
      return json(static_cast<int>(sorted.size()));
    }
    case Op::kReplaceNegatives: {
      std::vector<int> out = v;
      for (int& x : out) {
        if (x < 0) x = 0;
      }
      return out;
    }
    case Op::kReverseString: {
      std::string out = s;
      std::reverse(out.begin(), out.end());
      return json(out);
    }
    case Op::kToUpper: {
      std::string out = s;
      for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return json(out);
    }
    case Op::kIsPalindrome: {
      std::string reversed = s;
      std::reverse(reversed.begin(), reversed.end());
      return json(reversed == s);
    }
    case Op::kCountVowels: {
      int n = 0;
      for (const char c : s) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == 'a' || lower == 'e' || lower == 'i' || lower == 'o' || lower == 'u') ++n;
      }
      return json(n);
    }
    case Op::kLongestWordLength: {
      int best = 0, current = 0;
      for (const char c : s) {
        if (c == ' ') {
          best = std::max(best, current);
          current = 0;
        } else {
          ++current;
        }
      }
      return json(std::max(best, current));
    }
    case Op::kCountSpaces: {
      int n = 0;
      for (const char c : s) {
        if (c == ' ') ++n;
      }
      return json(n);
    }
    case Op::kBinarySearch: {
      for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == k) return json(static_cast<int>(i));
      }
      return json(-1);
    }
    case Op::kCountLess: {
      int n = 0;
      for (const int x : v) n += (x < k) ? 1 : 0;
      return json(n);
    }
    case Op::kCountGreater: {
      int n = 0;
      for (const int x : v) n += (x > k) ? 1 : 0;
      return json(n);
    }
    case Op::kCountEqual: {
      int n = 0;
      for (const int x : v) n += (x == k) ? 1 : 0;
      return json(n);
    }
    case Op::kRotateLeft: {
      if (v.empty()) return json(v);
      const std::size_t shift = static_cast<std::size_t>(((k % static_cast<int>(v.size())) +
                                                           static_cast<int>(v.size())) %
                                                          static_cast<int>(v.size()));
      std::vector<int> out;
      out.reserve(v.size());
      for (std::size_t i = 0; i < v.size(); ++i) out.push_back(v[(i + shift) % v.size()]);
      return out;
    }
  }
  return json(0);  // 不会走到
}

std::string param_types(const OpSpec& spec, std::string* first_name, std::string* second_name) {
  switch (spec.shape) {
    case Shape::kVecToInt:
    case Shape::kVecToVec:
      *first_name = "nums";
      return "std::vector<int>&";
    case Shape::kStrToStr:
    case Shape::kStrToBool:
    case Shape::kStrToInt:
      *first_name = "s";
      return "std::string";
    case Shape::kVecIntToInt:
    case Shape::kVecIntToVec:
      *first_name = "nums";
      *second_name = "k";
      return "std::vector<int>&, int";
  }
  return "std::vector<int>&";
}

std::string make_signature(const OpSpec& spec) {
  std::string first, second;
  const std::string types = param_types(spec, &first, &second);
  std::string decl = std::string(spec.return_type) + " " + spec.method + "(" + types + ");";
  return "class Solution {\npublic:\n    " + decl + "\n};";
}

std::string make_code_template(const OpSpec& spec) {
  std::string first, second;
  param_types(spec, &first, &second);
  const bool uses_vector = spec.shape != Shape::kStrToStr && spec.shape != Shape::kStrToBool &&
                           spec.shape != Shape::kStrToInt;
  const bool uses_string = spec.shape == Shape::kStrToStr || spec.shape == Shape::kStrToBool ||
                           spec.shape == Shape::kStrToInt;
  std::string head;
  if (uses_vector) head += "#include <vector>\n";
  if (uses_string) head += "#include <string>\n";
  head += "using namespace std;\n\n";
  const std::string params = second.empty()
                                 ? std::string("vector<") +
                                       (uses_vector ? "int" : "string") + ">& " + first
                                 : (uses_vector ? std::string("vector<int>& ") + first + ", int " + second
                                                : std::string("string ") + first + ", int " + second);
  return head + "class Solution {\npublic:\n    " + spec.return_type + " " + spec.method + "(" +
         params + ") {\n        \n    }\n};";
}

// "数组,哈希表" → ["数组","哈希表"]。手拼字符串容易漏引号（漏了就是 JSON 语法错，
// 而这层出错要等到 D-64 校验才报，绕了一圈），所以交给 json 数组构造。
json split_tags(const char* csv) {
  json out = json::array();
  const std::string text = csv;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t comma = text.find(',', begin);
    const std::string item =
        text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return out;
}

// 组出一道题的全部 JSON 字段。variant 让同一 op 的不同题拿到不同的用例（轮换输入池），
// 于是列表里 26 道题不会长得一模一样，但生成逻辑仍然只有一份。
json build_problem_json(const ProblemSeed& seed, int variant, int sort_no) {
  const OpSpec spec = spec_of(seed.op);
  const bool has_second_param =
      spec.shape == Shape::kVecIntToInt || spec.shape == Shape::kVecIntToVec;
  const bool uses_string = spec.shape == Shape::kStrToStr || spec.shape == Shape::kStrToBool ||
                           spec.shape == Shape::kStrToInt;

  std::string first_name, second_name;
  param_types(spec, &first_name, &second_name);

  json root;
  root["slug"] = seed.slug;
  root["title"] = seed.title;
  root["difficulty"] = seed.difficulty;
  root["is_featured"] = (variant % 9 == 4);
  root["sort_no"] = sort_no;
  root["tags"] = json{{"algorithm", split_tags(seed.algo_tags)},
                      {"source", split_tags(seed.source_tags)}};
  root["statement_md"] = std::string("## 题目描述\n\n") + seed.desc +
                         "\n\n函数签名见下方编辑器预置模板，参数与返回值类型不要改动。\n";
  root["hint_md"] =
      "先把签名抄准，再从最小输入（空/单元素）推演一遍；注意返回值类型与边界值。";
  root["signature"] = make_signature(spec);
  root["method_name"] = spec.method;
  root["return_type"] = spec.return_type;
  root["harness_prelude"] = "";
  root["code_template"] = make_code_template(spec);
  root["time_limit_ms"] = 3000;
  root["mem_limit_kb"] = 131072;

  json params = json::array();
  {
    json item;
    item["name"] = first_name;
    item["cpp_type"] = (first_name == "s") ? "std::string" : "std::vector<int>&";
    params.push_back(item);
    if (has_second_param) {
      json second;
      second["name"] = second_name;
      second["cpp_type"] = "int";
      params.push_back(second);
    }
  }
  root["params"] = params;

  json cases = json::array();
  std::string example_args;
  std::string example_expected;
  for (int i = 0; i < 3; ++i) {
    const std::size_t index = static_cast<std::size_t>((variant + i) % kVecPool.size());
    json args = json::array();
    json expected;
    std::string args_text;
    if (uses_string) {
      const std::string text = kStrPool[(variant + i) % kStrPool.size()];
      args.push_back(text);
      args_text = "s=\"" + text + "\"";
      expected = eval_op(seed.op, {}, 0, text);
    } else {
      const std::vector<int> values = kVecPool[index];
      args.push_back(values);
      args_text = "nums=" + join_numbers(values);
      const int k = values[values.size() / 2];
      if (has_second_param) {
        args.push_back(k);
        args_text += ", k=" + std::to_string(k);
      }
      expected = eval_op(seed.op, values, k, "");
    }
    if (i == 0) {
      example_args = args_text;
      example_expected = expected.dump();
    }
    json item;
    item["args"] = args;
    item["expected"] = expected;
    item["brief"] = kCaseBriefs[i];
    cases.push_back(item);
  }
  root["cases"] = cases;
  root["statement_md"] = std::string("## 题目描述\n\n") + seed.desc +
                         "\n\n### 示例\n\n```\n输入: " + example_args + "\n输出: " +
                         example_expected + "\n```\n\n### 约束\n\n- 参数与返回值类型以签名 "
                         "为准，不要改动\n- 用例规模见题面示例，判题时限 3000ms\n";
  return root;
}

// =============================================================================
// 题库元信息（造完库后从 DB 读回，用于拼逐用例明细的展示文本）
// =============================================================================
struct CaseRow {
  int case_idx = 0;
  std::string args_json;
  std::string expected_json;
  std::string args_text;
  std::string expected_text;
};

struct ProblemRow {
  long long id = 0;
  std::string slug;
  std::string code_template;
  int case_count = 0;
  int time_limit_ms = 3000;
  int mem_limit_kb = 131072;
  std::vector<std::string> param_names;  // 形参名，用于把 args 渲染成 "nums=[…], k=3"
  std::vector<CaseRow> cases;
};

// 展示文本：单形参直接给值（与《web自动化测试文档.md》§3.3 路径 B 的示例一致），
// 多形参才带上形参名。数据仍来自 problem_cases.args_json，不另存展示副本。
std::string prettify_args(const std::string& args_json, const std::vector<std::string>& names) {
  const json parsed = json::parse(args_json);
  if (!parsed.is_array() || parsed.empty()) return args_json;
  std::string out;
  for (std::size_t i = 0; i < parsed.size(); ++i) {
    if (i != 0) out += ", ";
    if (names.size() > 1 && i < names.size()) out += names[i] + "=";
    out += parsed[i].dump();
  }
  return out;
}

std::map<long long, ProblemRow> load_problems(Session& s) {
  std::map<long long, ProblemRow> rows;
  const oj::db::ResultSet heads = s.run(
      "SELECT id, slug, code_template, case_count, time_limit_ms, mem_limit_kb FROM problems "
      "ORDER BY sort_no, id");
  if (heads.failed()) die("读 problems 失败：" + heads.error().format());
  for (std::size_t i = 0; i < heads.size(); ++i) {
    const oj::db::Row& row = heads.at(i);
    ProblemRow problem;
    problem.id = static_cast<long long>(row.u64(0));
    problem.slug = row.str(1);
    problem.code_template = row.str(2);
    problem.case_count = static_cast<int>(row.u64(3));
    problem.time_limit_ms = static_cast<int>(row.u64(4));
    problem.mem_limit_kb = static_cast<int>(row.u64(5));
    rows.emplace(problem.id, std::move(problem));
  }
  const oj::db::ResultSet params =
      s.run("SELECT problem_id, param_idx, name FROM problem_params ORDER BY problem_id, param_idx");
  if (params.failed()) die("读 problem_params 失败：" + params.error().format());
  for (std::size_t i = 0; i < params.size(); ++i) {
    auto it = rows.find(static_cast<long long>(params.at(i).u64(0)));
    if (it != rows.end()) it->second.param_names.push_back(params.at(i).str(2));
  }
  const oj::db::ResultSet cases = s.run(
      "SELECT problem_id, case_idx, args_json, expected_json FROM problem_cases "
      "ORDER BY problem_id, case_idx");
  if (cases.failed()) die("读 problem_cases 失败：" + cases.error().format());
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const oj::db::Row& row = cases.at(i);
    const long long problem_id = static_cast<long long>(row.u64(0));
    auto it = rows.find(problem_id);
    if (it == rows.end()) continue;
    CaseRow item;
    item.case_idx = static_cast<int>(row.u64(1));
    item.args_json = row.str(2);
    item.expected_json = row.str(3);
    item.args_text = prettify_args(item.args_json, it->second.param_names);
    item.expected_text = item.expected_json;
    it->second.cases.push_back(std::move(item));
  }
  return rows;
}

// =============================================================================
// 造数：账号 / 会话 / 收藏 / 提交 / 逐用例明细 / 编译诊断
// =============================================================================
struct SeedUser {
  const char* username;
  const char* password;
  const char* role;
  const char* note;
};

// 7 个账号，正好对上 WT-AD-04 的「共 7 人」；alice01 刻意零提交，
// 对应 WT-AD-05（通过率显示 — 而不是 0%）。
const SeedUser kUsers[] = {
    {"admin", "admin123", "admin", "运维概览用（D-37 的 admin）"},
    {"ZhangLing", "666666", "user", "主力普通账号（库中存大写，登录大小写不敏感）"},
    {"alice01", "pass1234", "user", "零提交账号，用于 WT-AD-05"},
    {"bob02", "pass1234", "user", "有提交，用于 WT-SX-02~06 越权用例"},
    {"carol03", "pass1234", "user", "有提交，用于管理端清单多样性"},
    {"dave04", "pass1234", "user", "只有一条 TLE，用于管理端清单多样性"},
    {"erin05", "pass1234", "user", "零提交账号"},
};
constexpr int kUserCount = static_cast<int>(sizeof(kUsers) / sizeof(kUsers[0]));

std::map<std::string, long long> insert_users(Session& s) {
  std::map<std::string, long long> ids;
  for (int i = 0; i < kUserCount; ++i) {
    const SeedUser& user = kUsers[i];
    // cost=10（生产口径，AC-49 要的就是 `$2b$10$`）。7 个账号约 0.5~1s，
    // 换 cost=4 能快十倍，但会让 WT-AU-21 的断言失真，所以不省这点时间。
    const std::string hash = oj::web::hash_password(user.password, oj::web::kBcryptDefaultCost);
    must_run(
        s,
        "INSERT INTO users (username, password_hash, role, created_at) VALUES (?, ?, ?, ?)",
        {Value::from_text(user.username), Value::from_text(hash), Value::from_text(user.role),
         Value::from_text(utc_stamp(60L * 24 * 30))},
        std::string("插入账号 ") + user.username);
    ids.emplace(user.username, static_cast<long long>(last_insert_id(s, "插入账号")));
    ++g_stats.users;
  }
  return ids;
}

void insert_session(Session& s, long long user_id, const std::string& tag, long minutes_left) {
  must_run(s,
           "INSERT INTO sessions (token, user_id, expires_at, created_at) VALUES (?, ?, ?, ?)",
           {Value::from_text(fixed_token(tag)), Value::from_i64(user_id),
            Value::from_text(utc_stamp(-minutes_left)), Value::from_text(utc_stamp(60))},
           "插入会话 " + tag);
  ++g_stats.sessions;
}

// kPending 只服务于两个断言：管理端「未完成提交」计数（WT-AD-02）与历史页的
// pending 徽标。它是"提交已受理但还没判完"的中间态，因此 verdict / 度量 / finished_at
// 全部为 NULL —— 与 D-65 的 CE 兜底口径同源：聚合列缺失时由题库用例数兜底。
enum class Kind { kAc, kWa, kTle, kMle, kRe, kCe, kJe, kPending };

const char* verdict_of(Kind kind) {
  switch (kind) {
    case Kind::kAc: return "AC";
    case Kind::kWa: return "WA";
    case Kind::kTle: return "TLE";
    case Kind::kMle: return "MLE";
    case Kind::kRe: return "RE";
    case Kind::kCe: return "CE";
    case Kind::kJe: return "JE";
    case Kind::kPending: return nullptr;  // verdict 列可空（DDL：仅 status=done 时非空）
  }
  return "AC";
}

struct SubSeed {
  const char* username;
  const char* slug;
  Kind kind;
  int pass_cases;   // 前几个用例通过；CE / JE 不产生明细行（§5.6 的 0 明细行不变量）
  long ago_minutes;
};

// ZhangLing 的 34 条提交：verdict 按固定循环铺开，时间逐条前移，于是「默认时间倒序、
// 首行最新」（WT-HR-01）与「第 1 / 2 页」（WT-HR-02）都能验。
//
// ⚠️ 刻意只覆盖 21 道题，留下 5 道【从未提交】：sum-positive / count-spaces / to-upper /
// replace-negatives / count-equal。原因是 WT-LS-20 要同时验三态（已通过 / 尝试过 / 未开始）——
// 一旦 26 道题全被碰过，"未开始"就没有样本了，而没有样本的断言不能算通过。
// 加新提交时先确认不会把最后这几道题碰掉。
//
// ⚠️⚠️ 本表的顺序【不能改乱】：提交列表端点按 `ORDER BY s.id DESC` 排序
//   （src/web/submission_api.cpp:228，不是 created_at DESC），而 WT-HR-01 断言的是
//   "首行是最新那条"。二者一致的前提是 id 序 == 时间序，因此 ago_minutes 必须
//   随行号单调递减 —— 曾经把一条 JE（1350 分钟前）排在两条 pending（60/25 分钟前）
//   之后，结果历史页首行显示的是 22 小时前的那条，而它的 id 却是最大的。
const SubSeed kSubmissions[] = {
    {"ZhangLing", "two-sum",                 Kind::kAc,  2, 4320},
    {"ZhangLing", "array-sum",               Kind::kWa,  1, 4200},
    {"ZhangLing", "array-max",               Kind::kAc,  3, 4100},
    {"ZhangLing", "reverse-string",          Kind::kTle, 1, 4000},
    {"ZhangLing", "is-palindrome",           Kind::kAc,  3, 3900},
    {"ZhangLing", "count-vowels",            Kind::kAc,  3, 3800},
    {"ZhangLing", "array-sum",               Kind::kRe,  2, 3700},
    {"ZhangLing", "array-min",               Kind::kAc,  3, 3600},
    {"ZhangLing", "second-max",              Kind::kMle, 1, 3500},
    {"ZhangLing", "binary-search",           Kind::kAc,  3, 3400},
    {"ZhangLing", "rotate-left",             Kind::kCe,  0, 3300},
    {"ZhangLing", "count-distinct",          Kind::kAc,  3, 3200},
    {"ZhangLing", "two-sum",                 Kind::kWa,  1, 3100},
    {"ZhangLing", "index-of-max",            Kind::kAc,  3, 3000},
    {"ZhangLing", "max-abs",                 Kind::kJe,  0, 2900},
    {"ZhangLing", "sum-squares",             Kind::kAc,  3, 2800},
    {"ZhangLing", "first-negative",          Kind::kWa,  2, 2700},
    {"ZhangLing", "longest-word-length",     Kind::kAc,  3, 2600},
    {"ZhangLing", "max-min-diff",            Kind::kCe,  0, 2500},
    {"ZhangLing", "count-even",              Kind::kAc,  3, 2400},
    {"ZhangLing", "count-odd",               Kind::kMle, 2, 2300},
    {"ZhangLing", "array-max",               Kind::kAc,  3, 2200},
    {"ZhangLing", "count-less",              Kind::kRe,  0, 2100},
    {"ZhangLing", "count-greater",           Kind::kAc,  3, 2000},
    {"ZhangLing", "count-even",              Kind::kAc,  3, 1900},
    {"ZhangLing", "reverse-string",          Kind::kTle, 2, 1800},
    {"ZhangLing", "count-distinct",          Kind::kAc,  3, 1700},
    {"ZhangLing", "count-greater",           Kind::kWa,  0, 1600},
    {"ZhangLing", "two-sum",                 Kind::kAc,  2, 1500},
    {"ZhangLing", "array-max",               Kind::kAc,  3, 1440},
    {"ZhangLing", "is-palindrome",           Kind::kAc,  3, 1380},
    // 第二条 failed/JE：WT-HR-15/16 的「重跑按钮仅对 failed/JE 渲染」需要多于一条才看得出过滤
    {"ZhangLing", "longest-word-length",     Kind::kJe,  0, 1350},
    // 两条 pending：管理端「未完成提交」计数（WT-AD-02）与历史页的 pending 徽标
    //   ⚠️ judge 接线后重启，这两条会被 D-20 重新入队判掉（题面是种子题，判出来是
    //   什么结论取决于写的解法，回归前记得重跑一次本工具）。
    {"ZhangLing", "array-sum",               Kind::kPending, 0, 60},
    {"ZhangLing", "array-sum",               Kind::kPending, 0, 25},
    // 其他账号：bob02 的三条正好覆盖 WT-SX-02~06「他人提交」所需的非空数据
    {"bob02", "two-sum",                     Kind::kAc,  2, 3000},
    {"bob02", "array-sum",                   Kind::kWa,  1, 2900},
    {"bob02", "reverse-string",              Kind::kCe,  0, 2800},
    {"carol03", "array-max",                 Kind::kAc,  3, 2400},
    {"carol03", "is-palindrome",             Kind::kAc,  3, 2300},
    {"dave04", "count-vowels",               Kind::kTle, 2, 2000},
};

const char* code_for(Kind kind, const std::string& template_code, int serial) {
  static std::string buffer;
  switch (kind) {
    case Kind::kCe:
      // 故意写错：vecto<int> —— 对应 WT-RS-09 的 CE 形态
      buffer = "#include <vector>\nusing namespace std;\n\nclass Solution {\npublic:\n"
               "    vecto<int> broken(int n) {\n        return n;\n    }\n};\n";
      break;
    case Kind::kTle:
      buffer = "#include <vector>\nusing namespace std;\n\nclass Solution {\npublic:\n"
               "    int spin(int n) {\n        while (true) {}\n    }\n};\n";
      break;
    case Kind::kMle:
      buffer = "#include <vector>\nusing namespace std;\n\nclass Solution {\npublic:\n"
               "    int hog(int n) {\n        std::vector<int> a(1 << 28, 7);\n        return a[0];\n    }\n};\n";
      break;
    case Kind::kRe:
      buffer = "#include <vector>\nusing namespace std;\n\nclass Solution {\npublic:\n"
               "    int crash(int n) {\n        int* p = nullptr;\n        return *p;\n    }\n};\n";
      break;
    default:
      buffer = template_code + "\n// oj-reset 造数：第 " + std::to_string(serial) + " 次提交\n";
      break;
  }
  return buffer.c_str();
}

// 逐用例明细：AC 行给输出=期望；失败行给出错输出或错误说明，TLE/MLE/RE 的
// time_ms 与 mem_kb 留 NULL（§3.2 注释：harness 被 SIGKILL 时没有度量）。
void insert_case_results(Session& s, long long submission_id, const ProblemRow& problem, Kind kind,
                         int pass_cases, const char* memory_note) {
  const int total = static_cast<int>(problem.cases.size());
  for (int i = 0; i < total; ++i) {
    const CaseRow& item = problem.cases[static_cast<std::size_t>(i)];
    const bool passed = i < pass_cases;
    const char* row_verdict = "AC";
    std::string output = item.expected_text;
    std::string error;
    if (!passed) {
      switch (kind) {
        case Kind::kWa:
          row_verdict = "WA";
          output = "[]";
          error = "第 " + std::to_string(i + 1) + " 个用例答案错误";
          break;
        case Kind::kTle:
          row_verdict = "TLE";
          output.clear();
          error = "超出时间限制 (" + std::to_string(problem.time_limit_ms) + "ms)";
          break;
        case Kind::kMle:
          row_verdict = "MLE";
          output.clear();
          error = memory_note;
          break;
        case Kind::kRe:
          row_verdict = "RE";
          output.clear();
          error = "段错误（SIGSEGV）：解法在用例执行中崩溃";
          break;
        default:
          break;
      }
    }
    const bool has_metrics = (std::strcmp(row_verdict, "AC") == 0) ||
                             (std::strcmp(row_verdict, "WA") == 0);
    must_run(s,
             "INSERT INTO submission_case_results "
             "(submission_id, case_idx, verdict, passed, args_text, output_text, expected_text, "
             " time_ms, mem_kb, error_msg) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
             {Value::from_i64(submission_id), Value::from_i64(item.case_idx),
              Value::from_text(row_verdict), Value::from_bool(passed),
              Value::from_text(item.args_text), Value::from_text(output),
              Value::from_text(item.expected_text),
              has_metrics ? Value::from_i64(2 + (i % 8)) : Value::null(),
              has_metrics ? Value::from_i64(2048 + (i % 3) * 512) : Value::null(),
              error.empty() ? Value::null() : Value::from_text(error)},
             "插入逐用例明细行");
    ++g_stats.case_rows;
  }
}

void insert_submission(Session& s, const SubSeed& seed, long long user_id,
                       const ProblemRow& problem, int serial) {
  const bool is_je = seed.kind == Kind::kJe;
  const bool is_ce = seed.kind == Kind::kCe;
  const bool is_pending = seed.kind == Kind::kPending;
  const bool no_details = is_je || is_ce || is_pending;

  // 汇总列口径（§3.2 的注释就是这两条）：
  //   CE 时 case_total = 题库用例数、case_passed = 0（D-65）
  //   failed / JE 同样是 0 条明细行（D-67），case_total 也用题库用例数兜底
  int case_passed = 0;
  int case_total = problem.case_count;
  int total_ms = 0;
  int max_mem_kb = 0;
  if (!no_details) {
    case_passed = seed.pass_cases;
    for (int i = 0; i < seed.pass_cases; ++i) {
      total_ms += 2 + (i % 8);
      max_mem_kb = std::max(max_mem_kb, 2048 + (i % 3) * 512);
    }
  }
  if (case_passed > case_total) case_total = case_passed;

  std::string message;
  switch (seed.kind) {
    case Kind::kWa:  message = "第 " + std::to_string(seed.pass_cases + 1) + " 个用例答案错误"; break;
    case Kind::kTle: message = "第 " + std::to_string(seed.pass_cases + 1) + " 个用例超时"; break;
    case Kind::kMle: message = "第 " + std::to_string(seed.pass_cases + 1) + " 个用例超出地址空间上限"; break;
    case Kind::kRe:  message = "第 " + std::to_string(seed.pass_cases + 1) + " 个用例运行时错误"; break;
    case Kind::kCe:  message = "编译错误 · " + std::to_string(case_total) + " 个用例未执行"; break;
    case Kind::kJe:  message = "平台错误：判题进程未产出 @@OJ@@ 标记，不是你的代码问题"; break;
    case Kind::kAc:  break;
    case Kind::kPending: break;
  }

  const std::string created = utc_stamp(seed.ago_minutes);
  const std::string finished = utc_stamp(seed.ago_minutes, 3 + (serial % 7));
  const std::string memory_note =
      "超出内存限制 (地址空间 " + std::to_string(problem.mem_limit_kb / 1024) + "MB)";

  const char* status = is_pending ? "pending" : (is_je ? "failed" : "done");
  const char* verdict = verdict_of(seed.kind);
  must_run(s,
           "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict, "
           " message, compiler_output, case_passed, case_total, total_ms, max_mem_kb, "
           " created_at, finished_at) VALUES (?, ?, 'cpp', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
           {Value::from_i64(user_id), Value::from_i64(problem.id),
            Value::from_text(code_for(seed.kind, problem.code_template, serial)),
            Value::from_text(status),
            verdict == nullptr ? Value::null() : Value::from_text(verdict),
            message.empty() ? Value::null() : Value::from_text(message),
            Value::null(),
            is_pending ? Value::null() : Value::from_i64(case_passed),
            is_pending ? Value::null() : Value::from_i64(case_total),
            is_pending ? Value::null() : Value::from_i64(total_ms),
            is_pending ? Value::null() : Value::from_i64(max_mem_kb), Value::from_text(created),
            is_pending ? Value::null() : Value::from_text(finished)},
           "插入提交");
  const long long submission_id = static_cast<long long>(last_insert_id(s, "插入提交"));
  ++g_stats.submissions;

  if (is_ce) {
    // CE：补上 compiler_output + 两条诊断行（D-64：只保留 user.cpp，不暴露服务器路径）
    const std::string raw =
        "user.cpp: In member function ‘int broken(int)’:\n"
        "user.cpp:4:9: error: ‘vecto’ was not declared in this scope\n"
        "user.cpp:4:20: note: suggested alternative: ‘vector’\n"
        "user.cpp:6:12: error: expected ‘;’ before ‘}’ token\n"
        "compilation terminated.\n";
    must_run(s, "UPDATE submissions SET compiler_output = ? WHERE id = ?",
             {Value::from_text(raw), Value::from_i64(submission_id)}, "回填 CE 的编译器输出");
    const char* messages[] = {"‘vecto’ was not declared in this scope",
                              "expected ‘;’ before ‘}’ token"};
    const int lines[] = {4, 6};
    const int cols[] = {9, 12};
    for (int i = 0; i < 2; ++i) {
      must_run(s,
               "INSERT INTO submission_compile_errors "
               "(submission_id, seq, file_name, line, col, severity, message) "
               "VALUES (?, ?, 'user.cpp', ?, ?, 'error', ?)",
               {Value::from_i64(submission_id), Value::from_i64(i + 1),
                Value::from_i64(lines[i]), Value::from_i64(cols[i]), Value::from_text(messages[i])},
               "插入编译诊断行");
      ++g_stats.compile_rows;
    }
    return;
  }
  if (is_je || is_pending) return;  // failed / JE 与 pending：0 条明细行（D-67）
  insert_case_results(s, submission_id, problem, seed.kind, seed.pass_cases,
                      memory_note.c_str());
}

// =============================================================================
// 清库
// =============================================================================
const char* kTables[] = {
    "submission_compile_errors", "submission_case_results", "submissions",
    "user_problems",             "problem_tags",           "problem_params",
    "problem_cases",             "problems",               "tags",
    "sessions",                  "users",
};
constexpr int kTableCount = static_cast<int>(sizeof(kTables) / sizeof(kTables[0]));

void wipe(Session& s) {
  // 探针残留：run_problemsync_test.sh 之类在同库里留下的临时表（实测见过 oj_probe7）。
  // 它们不属于 §3.2 的 11 张表，但留在库里会让"这个库到底有什么"变得没法一眼看完。
  const oj::db::ResultSet probes =
      s.run("SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = DATABASE() AND table_name LIKE 'oj\\_probe%'");
  if (probes.failed()) die("读探针表失败：" + probes.error().format());
  for (std::size_t i = 0; i < probes.size(); ++i) {
    const std::string name = probes.at(i).str(0);
    must_exec(s, "DROP TABLE IF EXISTS `" + name + "`", "删除探针表 " + name);
    step("删除探针残留表 %s", name.c_str());
  }

  // DELETE 而不是 TRUNCATE：TRUNCATE 在有外键指向该表时会被服务端直接拒绝
  // （errno 1701，且 FOREIGN_KEY_CHECKS=0 也不管用），那就得按依赖序逐张来，
  // 顺序一旦被后人改动就会删到一半失败。这里统一：先关外键检查，再反序 DELETE。
  must_exec(s, "SET FOREIGN_KEY_CHECKS = 0", "关闭外键检查");
  for (int i = 0; i < kTableCount; ++i) {
    must_exec(s, std::string("DELETE FROM ") + kTables[i], std::string("清空 ") + kTables[i]);
  }
  // AUTO_INCREMENT 归 1：造出来的 submission id 因此可复现（同样输入 → 同样 id）。
  for (int i = 0; i < kTableCount; ++i) {
    must_exec(s, std::string("ALTER TABLE ") + kTables[i] + " AUTO_INCREMENT = 1", "重置自增");
  }
  must_exec(s, "SET FOREIGN_KEY_CHECKS = 1", "恢复外键检查");
}

// =============================================================================
// 计数回显（顺带当作自检：不满足就红字报警，而不是"造完就那样"）
// =============================================================================
long long count_rows(Session& s, const std::string& table) {
  const oj::db::ResultSet rows = s.run("SELECT COUNT(*) AS n FROM " + table);
  if (rows.failed() || rows.empty()) die("统计 " + table + " 失败");
  return static_cast<long long>(rows.at(0).u64(0));
}

void report_summary(Session& s) {
  std::printf("\n[oj-reset] 各表行数：\n");
  for (int i = 0; i < kTableCount; ++i) {
    std::printf("    %-28s %lld\n", kTables[i], count_rows(s, kTables[i]));
  }
  const oj::db::ResultSet diff =
      s.run("SELECT difficulty, COUNT(*) AS n FROM problems GROUP BY difficulty ORDER BY difficulty");
  std::printf("    难度分布：");
  for (std::size_t i = 0; i < diff.size(); ++i) {
    std::printf("%s=%s ", diff.at(i).str(0).c_str(), diff.at(i).str(1).c_str());
  }
  std::printf("\n");
  const oj::db::ResultSet verdicts =
      s.run("SELECT COALESCE(verdict, 'NULL') AS v, COUNT(*) AS n FROM submissions "
            "GROUP BY v ORDER BY v");
  std::printf("    verdict 分布：");
  for (std::size_t i = 0; i < verdicts.size(); ++i) {
    std::printf("%s=%s ", verdicts.at(i).str(0).c_str(), verdicts.at(i).str(1).c_str());
  }
  std::printf("\n\n[oj-reset] 可直接注入 Cookie 的会话（sid=<token>）：\n");
  std::printf("    ZhangLing 主会话  sid=%s\n", fixed_token("zhangling-main").c_str());
  std::printf("    ZhangLing 第二会话 sid=%s   （改密踢人 WT-AC-06 用）\n",
              fixed_token("zhangling-second").c_str());
  std::printf("    admin 会话        sid=%s\n", fixed_token("admin-main").c_str());
  std::printf("    alice01 过期会话  sid=%s   （会话过期 WT-ED-14 / WT-MG-11 用）\n\n",
              fixed_token("alice01-expired").c_str());
  std::printf("[oj-reset] 账号：admin/admin123(D-37 admin)、ZhangLing/666666、"
              "alice01|erin05(零提交)、bob02|dave04(有提交)\n\n");
}

}  // namespace

int run(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  std::string env_path = ".env";
  std::string problems_dir;  // 空 = 取 .env 的 OJ_PROBLEMS_DIR
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dry-run") {
      g_dry_run = true;
    } else if (arg == "--regen-problems") {
      g_regen_problems = true;
    } else if (arg == "--env" && i + 1 < argc) {
      env_path = argv[++i];
    } else if (arg == "--problems-dir" && i + 1 < argc) {
      problems_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "用法：oj-reset [--env <.env 路径>] [--problems-dir <目录>] [--dry-run] "
          "[--regen-problems]\n"
          "  清空 §3.2 的 11 张表并按《web自动化测试文档.md》的需要重建数据。\n"
          "  --regen-problems  覆盖重写本工具生成的种子题文件（改了 tools/reset_db.cpp 的题"
          "定义后用）\n"
          "  --dry-run          只打印计划，不连库、不写文件\n"
          "  必须从项目根目录执行（.env 与题库目录都是相对路径）。\n");
      return 0;
    } else {
      std::fprintf(stderr, "[oj-reset] 未知参数：%s（--help 看用法）\n", arg.c_str());
      return 2;
    }
  }

  const oj::common::LoadResult config = oj::common::load_config(env_path);
  if (!config.ok) {
    std::fprintf(stderr, "[oj-reset] 配置校验未通过（%zu 条 ERROR），拒绝执行\n",
                 config.error_count());
    return 1;
  }
  oj::common::init_logging();
  if (problems_dir.empty()) problems_dir = oj::common::config().problems_dir;

  std::printf("[oj-reset] 配置 %s；题库目录 %s\n", env_path.c_str(), problems_dir.c_str());
  if (g_dry_run) {
    std::printf("[oj-reset] --dry-run：不连库、不写任何东西。计划：清 11 张表 → 补 "
                "%d 个题库文件 → 造 %d 个账号 → 造 %zu 条提交。\n",
                kSeedCount, kUserCount, sizeof(kSubmissions) / sizeof(kSubmissions[0]));
    return 0;
  }

  std::vector<std::string> db_problems;
  if (!oj::db::Pool::instance().init_from_config(&db_problems)) {
    for (const std::string& text : db_problems) std::fprintf(stderr, "[oj-reset] %s\n", text.c_str());
    return 1;
  }

  // ---- ① 清库 ----
  //   租约用 {} 圈起来，是因为它持有的是 [D-21] 的进程级全局互斥量：题库加载器
  //   （load_all）内部要自己取租约，而 acquire() 对"同线程重复取租约"是【主动拒绝】而非等待
  //   —— 不还掉就会拿到一条惰性失败的连接，题库同步随之报一个与真实原因无关的错。
  step("① 清空 %d 张表并归零自增", kTableCount);
  {
    oj::db::Lease lease = oj::db::Pool::instance().acquire();
    if (!lease.valid()) die("取不到数据库连接租约");
    wipe(lease.session());
  }

  // ---- ② 题库：补齐缺失的种子文件，再走正式的加载器入库 ----
  //   已存在的文件一律【不覆盖】：真人在录的题不该被造数工具改掉。
  step("② 检查题库目录 %s", problems_dir.c_str());
  for (int i = 0; i < kSeedCount; ++i) {
    const std::string path = problems_dir + "/" + kSeeds[i].slug + ".json";
    if (!g_regen_problems) {
      std::ifstream probe(path);
      if (probe.good()) {
        ++g_stats.problem_files_kept;
        continue;
      }
    }
    const json document = build_problem_json(kSeeds[i], i, 100 + i);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) die("无法写入题库文件 " + path + "（题库目录可写吗）");
    out << document.dump(2) << "\n";
    if (!out) die("写题库文件失败 " + path);
    ++g_stats.problem_files_written;
  }
  step("   写入 %d 个题库文件，保留 %d 个已存在的%s", g_stats.problem_files_written,
       g_stats.problem_files_kept, g_regen_problems ? "（--regen-problems）" : "");

  oj::problem::LoadOptions options;
  options.dir = problems_dir;
  options.write_to_db = true;
  const oj::problem::LoadReport report = oj::problem::load_all(options);
  if (!report.ok) die("题库同步失败：" + report.format());
  step("   %s", report.summary().c_str());
  if (report.skipped > 0) {
    std::fprintf(stderr, "[oj-reset] 注意：%zu 道题被跳过（详见 logs/app.log）\n", report.skipped);
  }

  // ---- ③~⑥ 造数：整个阶段共用一条租约 ----
  {
    oj::db::Lease lease = oj::db::Pool::instance().acquire();
    if (!lease.valid()) die("取不到数据库连接租约");

    const std::map<long long, ProblemRow> problems = load_problems(lease.session());
    if (problems.empty()) die("题库入库后 problems 表仍为空 —— 文件写进去了吗？");
    std::map<std::string, const ProblemRow*> by_slug;
    for (const auto& entry : problems) by_slug.emplace(entry.second.slug, &entry.second);

    // ---- 账号 ----
    step("③ 造 %d 个账号（bcrypt cost=%d，约 1 秒）", kUserCount, oj::web::kBcryptDefaultCost);
    const std::map<std::string, long long> user_ids = insert_users(lease.session());

    // ---- 会话 ----
    step("④ 造 4 条会话（3 条有效 + 1 条已过期）");
    insert_session(lease.session(), user_ids.at("ZhangLing"), "zhangling-main", 60 * 24 * 6);
    insert_session(lease.session(), user_ids.at("ZhangLing"), "zhangling-second", 60 * 24 * 6);
    insert_session(lease.session(), user_ids.at("admin"), "admin-main", 60 * 24 * 6);
    insert_session(lease.session(), user_ids.at("alice01"), "alice01-expired", -60);

    // ---- 收藏与进度行 ----
    //   user_problems 存的是"收藏"这一个比特；进度由 submissions 推导，不在这里写。
    step("⑤ 造收藏与进度行");
    {
      const char* favorite_slugs[] = {"two-sum", "array-sum", "is-palindrome", "reverse-string"};
      const char* progress_slugs[] = {"array-max", "binary-search", "count-distinct", "max-abs",
                                      "rotate-left", "second-max"};
      const std::string now = utc_stamp(60);
      for (const char* slug : favorite_slugs) {
        auto it = by_slug.find(slug);
        if (it == by_slug.end()) continue;
        must_run(lease.session(),
                 "INSERT INTO user_problems (user_id, problem_id, favorite, updated_at) "
                 "VALUES (?, ?, 1, ?)",
                 {Value::from_i64(user_ids.at("ZhangLing")), Value::from_i64(it->second->id),
                  Value::from_text(now)},
                 "插入收藏");
        ++g_stats.favorites;
      }
      for (const char* slug : progress_slugs) {
        auto it = by_slug.find(slug);
        if (it == by_slug.end()) continue;
        must_run(lease.session(),
                 "INSERT INTO user_problems (user_id, problem_id, favorite, updated_at) "
                 "VALUES (?, ?, 0, ?)",
                 {Value::from_i64(user_ids.at("ZhangLing")), Value::from_i64(it->second->id),
                  Value::from_text(now)},
                 "插入进度行");
      }
    }

    // ---- 提交 / 明细 / 编译诊断 ----
    step("⑥ 造 %zu 条提交 + 逐用例明细 + 编译诊断",
         sizeof(kSubmissions) / sizeof(kSubmissions[0]));
    {
      oj::db::Transaction transaction(lease.session());
      if (!transaction.ok()) die("开启事务失败");
      int serial = 0;
      for (const SubSeed& seed : kSubmissions) {
        const auto user_it = user_ids.find(seed.username);
        const auto problem_it = by_slug.find(seed.slug);
        if (user_it == user_ids.end() || problem_it == by_slug.end() ||
            problem_it->second->cases.empty()) {
          die(std::string("造数表引用了不存在的题或账号：") + seed.slug);
        }
        ++serial;
        insert_submission(lease.session(), seed, user_it->second, *problem_it->second, serial);
      }
      if (!transaction.commit()) die("提交造数事务失败：" + lease.session().error().format());
    }

    report_summary(lease.session());
  }

  oj::common::log_info(oj::common::LogChannel::kApp,
                       "数据库重置完成：题库文件新增 " +
                           std::to_string(g_stats.problem_files_written) + " 个，账号 " +
                           std::to_string(g_stats.users) + " 个，会话 " +
                           std::to_string(g_stats.sessions) + " 条，提交 " +
                           std::to_string(g_stats.submissions) + " 条，逐用例明细 " +
                           std::to_string(g_stats.case_rows) + " 行（web自动化测试文档 §3 数据底座）");

  std::printf("[oj-reset] 完成。**请重启 ./build/oj** —— 它持有的是进程内题库缓存，"
              "不重启会读到旧题库；重启后列表页/题目页即刻可测。\n");

  oj::db::Pool::instance().shutdown();
  oj::common::shutdown_logging();
  return 0;
}

// 兜底：造数工具的价值全在"跑完就有一份干净数据"，所以未捕获异常必须变成一行人话 + 非零退出码，
// 而不是 terminate 的 abort（那样连日志都没留下，回归报告里只会看到"造数脚本崩了"）。
int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[oj-reset] 未捕获异常：%s\n", error.what());
    return 1;
  }
}
