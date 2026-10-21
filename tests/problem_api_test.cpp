// =============================================================================
// 题目列表 / 详情 API（include/web/problem_api.h + src/web/problem_api.cpp）的单元测试
// 被测：SPEC.md §9 M3 T3.6（列表查询）+ T3.17（下一题建议）
//      契约 §6.3 题目组三个端点，口径 §6.4 / §6.5，规则 §6.6 + §6.6.1
// 运行：ctest（`ctest -R ProblemApi` 可单跑本文件）
//
// 【分两段，理由与 admin_test.cpp / mysql_pool_test.cpp 相同】
//   A 段（纯逻辑，永远会跑）：查询串解析、校验、序列化契约、推荐打分函数。
//     这一段恰好覆盖了本模块里【最容易在重构中被悄悄改掉】的东西：枚举取值、
//     1001 的 field 名、camelCase 键名、以及 [D-68] 的主标签选取与并列规则。
//   B 段（真库，连不上整体跳过）：筛选、分页、排序、进度/收藏 JOIN、详情组装、
//     nextRecommendation 的两条 SQL。
//
// 【探针数据策略：不建独立库，只在配置的库里造带前缀的行并在 TearDown 删净】
//   与 admin_test.cpp 相同：建库需要 CREATE DATABASE 权限，而 oj 账号按 §6.7.3
//   只有库内权限。users 侧的 submissions / user_problems / sessions 由 CASCADE
//   带走，problems 侧的 problem_tags / problem_params / problem_cases 同样级联。
//   ⚠️ 断言一律用【增量或限定集合】而不是绝对值：库里本来就题库数据
//     （T1.2 同步进来的 two-sum），写死绝对值会让本文件在题库一变就红。
//     所以除一个"未筛选列表含库内既有题"的用例外，所有筛选用例都带
//     keyword=探针 或探针标签名，把结果收敛到探针集合内。
//   ⚠️ gtest_discover_tests 把每个 TEST_F 注册成一条独立的 ctest 用例，
//     即每个用例都是【新进程】跑一遍 SetUp/TearDown —— 探针数据的清理因此是
//     "每次都干净"，但也意味着每条用例都要重新造一遍种子数据（毫秒级）。
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "db/mysql_pool.h"
#include "web/problem_api.h"

namespace {

using namespace oj;
using namespace oj::db;
using namespace oj::web;

constexpr const char* kUserPrefix = "ojprb-u-";
constexpr const char* kProblemPrefix = "ojprb-p-";
constexpr const char* kTagPrefix = "ojprb-t-";
constexpr const char* kSourcePrefix = "ojprb-s-";
constexpr const char* kKeyword = "探针";   // 所有探针题的 title 都含它

std::string env_or(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  return (value != nullptr && *value != '\0') ? std::string(value) : std::string(fallback);
}

db::PoolOptions base_options() {
  db::PoolOptions o;
  o.host = env_or("MYSQL_HOST", "127.0.0.1");
  o.port = static_cast<unsigned int>(std::atoi(env_or("MYSQL_PORT", "3306").c_str()));
  o.user = env_or("MYSQL_USER", "oj");
  o.password = env_or("MYSQL_PASSWORD", "oj");
  o.database = env_or("MYSQL_DB", "oj");
  return o;
}

// =============================================================================
// A 段 · 纯逻辑：不碰 DB，永远会跑
// =============================================================================

// ---- 查询串解析 -----------------------------------------------------------------

TEST(ProblemApiParse, DefaultsMatchTheEndpointContract) {
  // §6.3 端点表：page/size 未给时用这里的默认值；全部筛选默认"不过滤"。
  // size=20 与 submissions 列表一致（[D-61]），page=1 是 1-based。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("", &query, &error)) << error.reason;
  EXPECT_TRUE(query.keyword.empty());
  EXPECT_FALSE(query.has_difficulty);
  EXPECT_TRUE(query.tags.empty());
  EXPECT_EQ(query.progress, Progress::kNone);
  EXPECT_FALSE(query.favorite_only);
  EXPECT_EQ(query.sort, ProblemSort::kDefault);
  EXPECT_EQ(query.page, 1);
  EXPECT_EQ(query.size, 20);
}

TEST(ProblemApiParse, ReadsEveryDocumentedParameter) {
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query(
      "keyword=%E4%B8%8D%E5%BC%82&difficulty=Hard&tags=%E6%95%B0%E7%BB%84,%E5%8E%BB%E9%87%8D"
      "&progress=solved&favorite=1&sort=hardest&page=3&size=50",
      &query, &error))
      << error.reason;
  EXPECT_EQ(query.keyword, "不异");
  EXPECT_TRUE(query.has_difficulty);
  EXPECT_EQ(query.difficulty, model::Difficulty::kHard);   // 大小写不敏感
  EXPECT_EQ(query.tags, (std::vector<std::string>{"数组", "去重"}));
  EXPECT_EQ(query.progress, Progress::kSolved);
  EXPECT_TRUE(query.favorite_only);
  EXPECT_EQ(query.sort, ProblemSort::kHardest);
  EXPECT_EQ(query.page, 3);
  EXPECT_EQ(query.size, 50);
}

TEST(ProblemApiParse, TrimsKeywordAndTags) {
  // 搜索框里几乎总会带空格；不 trim 的话 " 两数 " 永远搜不到东西，而用户看不出原因。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("keyword=%20two%20sum%20&tags=%20%E6%95%B0%E7%BB%84%20,", &query,
                               &error))
      << error.reason;
  EXPECT_EQ(query.keyword, "two sum");
  ASSERT_EQ(query.tags.size(), 1u);
  EXPECT_EQ(query.tags[0], "数组");   // 末尾多一个逗号不该报错，也不该产生空标签
}

TEST(ProblemApiParse, PlusIsSpace) {
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("keyword=a+b", &query, &error)) << error.reason;
  EXPECT_EQ(query.keyword, "a b");
}

TEST(ProblemApiParse, DuplicateTagsAreCollapsed) {
  // 重复标签会变成两个 EXISTS：结果不变，但白跑一次子查询，且 WHERE 变得没法一眼看懂。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("tags=%E6%95%B0%E7%BB%84,%E6%95%B0%E7%BB%84,%E5%8E%BB%E9%87%8D",
                               &query, &error))
      << error.reason;
  EXPECT_EQ(query.tags, (std::vector<std::string>{"数组", "去重"}));
}

TEST(ProblemApiParse, SameKeyAppearingTwiceKeepsTheLast) {
  // 说明 ③：httplib 的 params 是 multimap，摊平后天然是"最后一个生效"。
  // 钉死它是为了让路由层不必再发明一条规则（否则两处规则不同 = 两种结果）。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("page=2&page=7", &query, &error)) << error.reason;
  EXPECT_EQ(query.page, 7);
}

TEST(ProblemApiParse, UnknownParametersAreIgnored) {
  // 说明 ②：前端新增一个筛选参数不该让旧后端整体 400。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("keyword=x&foo=bar&utm_source=wechat", &query, &error))
      << error.reason;
  EXPECT_EQ(query.keyword, "x");
}

TEST(ProblemApiParse, EmptyValueMeansNoFilter) {
  // 前端清空筛选栏时通常发空值而不是不发这个参数。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("difficulty=&tags=&progress=&sort=&favorite=&page=&size=",
                               &query, &error))
      << error.reason;
  EXPECT_FALSE(query.has_difficulty);
  EXPECT_TRUE(query.tags.empty());
  EXPECT_EQ(query.progress, Progress::kNone);
  EXPECT_EQ(query.sort, ProblemSort::kDefault);
  EXPECT_FALSE(query.favorite_only);
  EXPECT_EQ(query.page, 1);
  EXPECT_EQ(query.size, 20);
}

TEST(ProblemApiParse, SegmentsWithoutEqualAreTreatedAsEmptyValue) {
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("keyword", &query, &error)) << error.reason;
  EXPECT_TRUE(query.keyword.empty());
}

TEST(ProblemApiParse, RejectsIllegalEnumValuesAndNamesTheField) {
  // 说明 ①：枚举非法一律 1001 而不是回退默认 —— 静默回退会让用户拿到一个
  // "看起来按我选的筛了、其实没筛"的列表，没有任何报错信号。
  struct Case {
    const char* query;
    const char* field;
  };
  for (const Case& c : {Case{"difficulty=impossible", "difficulty"},
                         Case{"progress=passed", "progress"},
                         Case{"sort=random", "sort"},
                         Case{"favorite=maybe", "favorite"}}) {
    ListQuery query;
    QueryParseError error;
    EXPECT_FALSE(parse_list_query(c.query, &query, &error)) << c.query;
    EXPECT_EQ(error.field, c.field) << c.query;
    EXPECT_FALSE(error.reason.empty()) << c.query;
  }
}

TEST(ProblemApiParse, RejectsNonNumericPaging) {
  // strtoll 会把 "12abc" 解析成 12 而不报错 —— 那就是静默地改了用户的输入。
  for (const char* query : {"page=1abc", "page=-1", "page=0", "page=1.5", "size=0", "size=abc",
                            "size=-5", "page=99999999999999999999"}) {
    ListQuery parsed;
    QueryParseError error;
    EXPECT_FALSE(parse_list_query(query, &parsed, &error)) << query;
    EXPECT_FALSE(error.reason.empty()) << query;
  }
}

TEST(ProblemApiParse, OversizedSizeIsClampedToTheFence) {
  // 护栏（kMaxPageSize）：一次 size=100000 的查询在串行化的 DB 上会把同期请求全拖住。
  // 夹取而不是 1001 —— 响应的 size 会回显实际生效值，前端据此画分页器。
  ListQuery query;
  QueryParseError error;
  ASSERT_TRUE(parse_list_query("size=100000", &query, &error)) << error.reason;
  EXPECT_EQ(query.size, kMaxPageSize);
}

TEST(ProblemApiParse, RejectsOversizedKeywordAndTooManyTags) {
  ListQuery query;
  QueryParseError error;
  EXPECT_FALSE(parse_list_query("keyword=" + std::string(201, 'a'), &query, &error));
  EXPECT_EQ(error.field, "keyword");

  std::string many_tags;
  for (int i = 0; i < 21; ++i) many_tags += (i == 0 ? "" : ",") + std::string("tag") + std::to_string(i);
  EXPECT_FALSE(parse_list_query("tags=" + many_tags, &query, &error));
  EXPECT_EQ(error.field, "tags");
}

TEST(ProblemApiParse, RejectsBrokenUrlEscape) {
  // 半解码比不解码更坏：含字面量 '%' 的查询串会被悄悄改写。
  for (const char* query : {"keyword=%", "keyword=%4", "keyword=%ZZ", "keyword=%4Z"}) {
    ListQuery parsed;
    QueryParseError error;
    EXPECT_FALSE(parse_list_query(query, &parsed, &error)) << query;
  }
}

TEST(ProblemApiParse, FailureLeavesOutUntouched) {
  // 不变式：解析失败时 *out 一个字节都不改。调用方常在解析失败后仍拿着旧 query
  // 去打日志或重试，静默半更新会让"这次请求用了哪个筛选"变得无法复盘。
  ListQuery query;
  query.keyword = "上一次的关键词";
  query.page = 5;
  QueryParseError error;
  ASSERT_FALSE(parse_list_query("page=abc", &query, &error));
  EXPECT_EQ(query.keyword, "上一次的关键词");
  EXPECT_EQ(query.page, 5);
}

TEST(ProblemApiUrlDecode, HandlesPlusPercentAndNul) {
  std::string out;
  ASSERT_TRUE(url_decode("a+b%20c%2Fd%00e", &out));
  // 显式给长度：std::string 的构造会在第一个 '\0' 处截断，而解码结果里真的可能有 NUL
  EXPECT_EQ(out, std::string("a b c/d\0e", 9));
  EXPECT_EQ(out.size(), 9u);
  EXPECT_FALSE(url_decode("%2", &out));
  EXPECT_FALSE(url_decode("%GG", &out));
}

// ---- 匿名与个人筛选 -------------------------------------------------------------

TEST(ProblemApiValidate, AnonymousCannotFilterByPersonalState) {
  // §6.3 的三个端点 Auth=否（[D-43]），但 progress/favorite 是【个人数据】的筛选。
  // 静默忽略会让用户拿到"看起来筛了其实没筛"的列表；返回空列表更糟 ——
  // 用户会读成"题库里没有这类题"。故 1001 并指名 field。
  const ViewerContext anonymous;   // user_id = 0
  ListQuery query;
  query.progress = Progress::kSolved;
  QueryParseError error;
  EXPECT_FALSE(validate_list_query(&anonymous, query, &error));
  EXPECT_EQ(error.field, "progress");

  ListQuery favorite_query;
  favorite_query.favorite_only = true;
  error = QueryParseError{};
  EXPECT_FALSE(validate_list_query(&anonymous, favorite_query, &error));
  EXPECT_EQ(error.field, "favorite");

  ListQuery plain;   // 无个人筛选：匿名可以查
  EXPECT_TRUE(validate_list_query(&anonymous, plain, &error));

  // 递进来的会话若 user_id=0，按匿名处理（T3.3 不该造出这种会话）
  EXPECT_FALSE(validate_list_query(nullptr, query, &error));
}

TEST(ProblemApiValidate, LoggedInMayUseEveryFilter) {
  const ViewerContext alice{7, "alice"};
  ListQuery query;
  query.progress = Progress::kAttempted;
  query.favorite_only = true;
  QueryParseError error;
  EXPECT_TRUE(validate_list_query(&alice, query, &error)) << error.reason;
}

// ---- 推荐打分（纯函数，[D-68] + [D-59]）----------------------------------------

// 造一条候选：算法标签写成 {tag_id, name}。
RecommendationCandidate candidate(int id, const char* difficulty, int sort_no,
                                  std::vector<std::pair<long long, std::string>> tags) {
  RecommendationCandidate c;
  c.id = id;
  c.slug = "candidate-" + std::to_string(id);
  c.title = "候选 " + std::to_string(id);
  c.difficulty = difficulty;
  c.sort_no = sort_no;
  c.algorithm_tags = std::move(tags);
  return c;
}

TEST(ProblemApiPick, EmptyCandidateSetMeansNoRecommendation) {
  // 两种情形都落到这里：全部通过（§7.4 的 🎉 完成度卡）、题库里没有 algorithm 标签。
  const std::vector<TagStat> stats = {{1, "数组", 10, 10}};
  EXPECT_FALSE(pick_recommendation(stats, {}).has_value());
  EXPECT_FALSE(pick_recommendation({}, {}).has_value());
}

TEST(ProblemApiPick, PrimaryTagIsTheLowestPassRateOneAndDrivesTheReason) {
  // [D-68]：主标签 = 该题 algorithm 标签中通过率最低者。理由文案用【同一个标签】，
  // 故不会出现"说的标签和算分的标签不是同一个"（AC-34 核对的正是这一点）。
  //
  // 注意这里的方向容易反着理解：score = 1000/(1+薄弱度)，所以"只挂你最擅长的标签"
  // 的题分数【最低】；真正该被推去补的是"它有一个你很弱的标签"的题。
  const std::vector<TagStat> stats = {
      {1, "数组", 10, 8},      // 8/10 通过 → 薄弱度 0.2 → score 833
      {2, "哈希表", 10, 3},    // 3/10 通过 → 薄弱度 0.7 → score 588
      {3, "动态规划", 10, 1},  // 1/10 通过 → 薄弱度 0.9 → score 526（最弱，得分最低）
  };
  const std::vector<RecommendationCandidate> candidates = {
      candidate(1, "easy", 1, {{1, "数组"}, {2, "哈希表"}}),   // 主标签 = 哈希表
      candidate(2, "hard", 9, {{3, "动态规划"}}),                // 主标签 = 动态规划
  };
  const auto picked = pick_recommendation(stats, candidates);
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(picked->id, 1);
  EXPECT_EQ(picked->tag, "哈希表");     // 而不是并列中 id 更小的"数组"
  EXPECT_EQ(picked->tag_solved, 3);
  EXPECT_EQ(picked->tag_total, 10);
  EXPECT_NE(picked->reason.find("哈希表"), std::string::npos);
  EXPECT_NE(picked->reason.find("3/10"), std::string::npos);
  // 1000 / (1 + 0.7)
  EXPECT_NEAR(picked->score, 1000.0 / 1.7, 1e-9);
}

TEST(ProblemApiPick, EqualPassRatesPickTheSmallerTagId) {
  // §6.6.1 的第二行：并列取 tags.id 最小者，"保证结果稳定可复现"。
  // 不钉这一条，两个并列标签的取舍就取决于 JOIN 顺序，AC-46 无法核对。
  const std::vector<TagStat> stats = {
      {7, "链表", 5, 2},     // 并列
      {3, "双指针", 5, 2},   // 并列，id 更小 ⇒ 主标签
  };
  const std::vector<RecommendationCandidate> candidates = {
      candidate(1, "easy", 1, {{7, "链表"}, {3, "双指针"}}),
  };
  const auto picked = pick_recommendation(stats, candidates);
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(picked->tag, "双指针");
  EXPECT_EQ(picked->tag_total, 5);
  EXPECT_EQ(picked->tag_solved, 2);
}

TEST(ProblemApiPick, ScoreTiesBreakByDifficultyThenSortNoThenId) {
  // §6.6 的同分规则：difficulty ASC → sort_no ASC。id ASC 是本模块补的最后一层
  // 兜底（SPEC 没写），理由是前两者都重复时仍要有一个确定结果。
  const std::vector<TagStat> stats = {{1, "数组", 4, 2}};   // 所有候选共用同一个薄弱度
  const std::vector<RecommendationCandidate> candidates = {
      candidate(10, "hard", 1, {{1, "数组"}}),
      candidate(11, "easy", 9, {{1, "数组"}}),
      candidate(12, "easy", 3, {{1, "数组"}}),
      candidate(13, "easy", 3, {{1, "数组"}}),   // 与 12 全并列 ⇒ 比 id
  };
  const auto picked = pick_recommendation(stats, candidates);
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(picked->id, 12);
}

TEST(ProblemApiPick, IsReproducibleForTheSameInput) {
  // AC-46 的核心：连跑 5 次同一输入必须得同一题。这里刻意让三个候选的薄弱度相等，
  // 逼着实现走到并列判定那条路上（如果并列判定是"谁先来谁赢"，输入顺序一变就变）。
  const std::vector<TagStat> stats = {{1, "数组", 4, 2}, {2, "哈希表", 4, 2}};
  std::vector<RecommendationCandidate> candidates = {
      candidate(20, "medium", 2, {{1, "数组"}, {2, "哈希表"}}),
      candidate(21, "easy", 2, {{2, "哈希表"}, {1, "数组"}}),
      candidate(22, "easy", 1, {{1, "数组"}}),
  };
  for (int round = 0; round < 5; ++round) {
    // 每轮打乱候选顺序：若结果依赖输入顺序而不是规则，这一轮就会露馅
    std::vector<RecommendationCandidate> shuffled = candidates;
    std::rotate(shuffled.begin(), shuffled.begin() + (round % 3), shuffled.end());
    const auto picked = pick_recommendation(stats, shuffled);
    ASSERT_TRUE(picked.has_value()) << "第 " << round << " 轮";
    EXPECT_EQ(picked->id, 22) << "第 " << round << " 轮";
  }
}

TEST(ProblemApiPick, CandidatesWhoseTagsHaveNoStatsAreSkipped) {
  // 防御性：标签统计覆盖全部 algorithm 标签，若某题的标签查不到统计就跳过，
  // 而不是拿 total=0 去算分（那会除出 1.0 的"最薄弱"，推荐出一堆噪声）。
  const std::vector<TagStat> stats = {{1, "数组", 4, 2}};
  const std::vector<RecommendationCandidate> candidates = {
      candidate(30, "easy", 1, {{99, "查无此标签"}}),
      candidate(31, "easy", 2, {{1, "数组"}}),
  };
  const auto picked = pick_recommendation(stats, candidates);
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(picked->id, 31);
}

TEST(ProblemApiPick, ZeroTotalTagIsSkipped) {
  const std::vector<TagStat> stats = {{1, "空标签", 0, 0}, {2, "数组", 4, 2}};
  const std::vector<RecommendationCandidate> candidates = {
      candidate(40, "easy", 1, {{1, "空标签"}}),
  };
  EXPECT_FALSE(pick_recommendation(stats, candidates).has_value());
}

// ---- 序列化契约 -----------------------------------------------------------------

TEST(ProblemApiJson, TagGroupsShape) {
  TagGroups tags;
  tags.algorithm = {"数组", "哈希表"};
  tags.source = {"高频面试题"};
  const nlohmann::json parsed = nlohmann::json::parse(to_json(tags));
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed["algorithm"], nlohmann::json({"数组", "哈希表"}));
  EXPECT_EQ(parsed["source"], nlohmann::json({"高频面试题"}));
  // 空库时也必须是空数组：前端筛选栏直接 map()，null 会让页面白屏
  const nlohmann::json empty = nlohmann::json::parse(to_json(TagGroups{}));
  ASSERT_TRUE(empty["algorithm"].is_array());
  ASSERT_TRUE(empty["source"].is_array());
  EXPECT_EQ(empty["algorithm"].size(), 0u);
}

TEST(ProblemApiJson, ListUsesCamelCaseAndKeepsArraysArrays) {
  ProblemList list;
  list.total = 42;
  list.page = 3;
  list.size = 20;
  ProblemListItem item;
  item.id = 7;
  item.slug = "two-sum";
  item.title = "两数之和";
  item.difficulty = "medium";
  item.my_progress = Progress::kSolved;
  item.is_favorite = true;
  item.case_count = 2;
  item.tags = {{"数组", "algorithm"}, {"高频面试题", "source"}};
  list.list.push_back(item);

  const nlohmann::json parsed = nlohmann::json::parse(to_json(list));
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.size(), 4u);
  EXPECT_EQ(parsed["total"], 42);
  EXPECT_EQ(parsed["page"], 3);
  EXPECT_EQ(parsed["size"], 20);
  ASSERT_TRUE(parsed["list"].is_array());
  ASSERT_EQ(parsed["list"].size(), 1u);
  const nlohmann::json& row = parsed["list"][0];
  for (const char* key : {"id", "slug", "title", "difficulty", "tags", "myProgress", "isFavorite",
                          "timeLimitMs", "memLimitKb", "caseCount", "isFeatured", "sortNo"}) {
    EXPECT_TRUE(row.contains(key)) << "list[] 缺少字段 " << key;
  }
  EXPECT_EQ(row["myProgress"], "solved");
  EXPECT_EQ(row["isFavorite"], true);
  EXPECT_EQ(row["caseCount"], 2);
  ASSERT_EQ(row["tags"].size(), 2u);
  EXPECT_EQ(row["tags"][0]["name"], "数组");
  EXPECT_EQ(row["tags"][1]["category"], "source");

  const nlohmann::json empty = nlohmann::json::parse(to_json(ProblemList{}));
  ASSERT_TRUE(empty["list"].is_array());
  EXPECT_EQ(empty["list"].size(), 0u);
  EXPECT_EQ(empty["total"], 0);
}

TEST(ProblemApiJson, DetailOmitsNextRecommendationForAnonymous) {
  // AC-25：未登录时响应里【不含】 nextRecommendation。前端据此【整张卡不渲染】
  // （T4.22），而不是渲染一张空的卡。
  ProblemDetail detail;
  detail.authenticated = false;
  detail.next_recommendation = NextRecommendation{};   // 即使有值也不下发
  const nlohmann::json parsed = nlohmann::json::parse(to_json(detail));
  EXPECT_FALSE(parsed.contains("nextRecommendation"));
  // 其余字段必须齐全 —— 匿名用户照样要能读完整题面（[D-43]）
  for (const char* key : {"id", "slug", "title", "difficulty", "statementMd", "hint_md",
                          "signature", "methodName", "returnType", "codeTemplate", "params",
                          "tags", "timeLimitMs", "memLimitKb", "caseCount", "myProgress",
                          "isFavorite", "recentSubmissions"}) {
    EXPECT_TRUE(parsed.contains(key)) << "detail 缺少字段 " << key;
  }
  ASSERT_TRUE(parsed["recentSubmissions"].is_array());
}

TEST(ProblemApiJson, DetailSendsNullNextRecommendationWhenNothingToRecommend) {
  // 已登录但全部通过 ⇒ 字段存在且为 null，前端渲染「🎉 题库 N/N 全部通过」（§7.4）。
  // 与"未登录时字段缺失"是两种不同的形状，这里分别钉死。
  ProblemDetail detail;
  detail.authenticated = true;
  const nlohmann::json none = nlohmann::json::parse(to_json(detail));
  ASSERT_TRUE(none.contains("nextRecommendation"));
  EXPECT_TRUE(none["nextRecommendation"].is_null());

  ProblemDetail recommended = detail;
  recommended.next_recommendation = NextRecommendation{7, "reverse-linked-list", "反转链表",
                                                       "easy", "链表", 5, 2, "「链表」标签你通过 2/5，本题尚未通过", 666.6};
  const nlohmann::json some = nlohmann::json::parse(to_json(recommended));
  ASSERT_TRUE(some["nextRecommendation"].is_object());
  for (const char* key : {"slug", "title", "difficulty", "tag", "tagSolved", "tagTotal", "reason"}) {
    EXPECT_TRUE(some["nextRecommendation"].contains(key)) << "nextRecommendation 缺少字段 " << key;
  }
  EXPECT_EQ(some["nextRecommendation"]["tag"], "链表");
  EXPECT_EQ(some["nextRecommendation"]["tagSolved"], 2);
  EXPECT_FALSE(some["nextRecommendation"]["reason"].get<std::string>().empty());
}

TEST(ProblemApiJson, HintKeyKeepsTheSpecSpellingAndNullMeansNoHintTab) {
  // §6.3 端点表把这一项写作 hint_md（snake），与相邻 camelCase 字段不同名。
  // T4.20/AC-26：空 ⇒ 不渲染提示 tab。键名漂了会让"有提示的题不显示提示"，
  // 而且前端读到 undefined 时多半会静默当作"没有提示"—— 症状像丢数据。
  ProblemDetail with_hint;
  with_hint.hint_md = "先画个草图";
  EXPECT_EQ(nlohmann::json::parse(to_json(with_hint))["hint_md"], "先画个草图");
  EXPECT_EQ(nlohmann::json::parse(to_json(ProblemDetail{}))["hint_md"].is_null(), true);
}

TEST(ProblemApiJson, RecentSubmissionNullMetricsStayNull) {
  // pending 提交没有 verdict / 汇总列（§3.2 允许 NULL）。序列化成 0 会让前端显示
  // 「0ms 通过 0/0」这种【看起来像结果】的东西，而不是"还没出结论"。
  ProblemDetail detail;
  RecentSubmission entry;
  entry.id = 9;
  entry.status = "pending";
  detail.recent_submissions.push_back(entry);
  const nlohmann::json parsed = nlohmann::json::parse(to_json(detail));
  ASSERT_EQ(parsed["recentSubmissions"].size(), 1u);
  const nlohmann::json& row = parsed["recentSubmissions"][0];
  EXPECT_TRUE(row["verdict"].is_null());
  EXPECT_TRUE(row["message"].is_null());
  EXPECT_TRUE(row["casePassed"].is_null());
  EXPECT_TRUE(row["caseTotal"].is_null());
  EXPECT_TRUE(row["totalMs"].is_null());
  EXPECT_TRUE(row["maxMemKb"].is_null());
  EXPECT_EQ(row["status"], "pending");

  RecentSubmission done;
  done.id = 10;
  done.status = "done";
  done.verdict = "AC";
  done.has_case_passed = true;
  done.case_passed = 3;
  done.has_case_total = true;
  done.case_total = 3;
  ProblemDetail finished;
  finished.recent_submissions.push_back(done);
  const nlohmann::json done_row = nlohmann::json::parse(to_json(finished))["recentSubmissions"][0];
  EXPECT_EQ(done_row["verdict"], "AC");
  EXPECT_EQ(done_row["casePassed"], 3);
  EXPECT_EQ(done_row["caseTotal"], 3);
}

TEST(ProblemApiJson, AwkwardStringsSurviveTheRoundTrip) {
  // 题面里有引号、反斜杠、换行与中文 emoji 是常态（Markdown 里的代码块尤其多）。
  // 自己 sprintf 拼 JSON 的人一定会在这里翻车，这是"为什么用 nlohmann"的回归。
  ProblemDetail detail;
  detail.statement_md = "```cpp\nvector<int> a{\"x\", \"y\\z\"};\n``` 🙃\ttab";
  detail.title = "引号\"与\\反斜杠";
  detail.recent_submissions.clear();
  const nlohmann::json parsed = nlohmann::json::parse(to_json(detail));
  EXPECT_EQ(parsed["statementMd"].get<std::string>(), detail.statement_md);
  EXPECT_EQ(parsed["title"].get<std::string>(), detail.title);
}

// =============================================================================
// B 段 · 真库：筛选、分页、排序、进度/收藏 JOIN、详情、推荐
// =============================================================================

class ProblemApiDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_ = base_options();
    Pool probe;
    if (!probe.init(options_)) {
      GTEST_SKIP() << "无法连接 MySQL " << options_.host << ":" << options_.port << "/"
                   << options_.database << "，跳过依赖真实数据库的用例（设置 MYSQL_* 可指定）";
    }
    probe.shutdown();

    pool_.reset(new Pool());
    ASSERT_TRUE(pool_->init(options_)) << pool_->describe();

    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    // 表必须已经由 §2.1-A 的启动建表建好；没有就跳过而不是报错 ——
    // "还没跑到 T0.3" 与 "本模块的 SQL 有问题" 是两件事，不该共用一个红色。
    const bool tables_ready = tables_exist(lease.session());
    if (tables_ready) clear_probe_rows(lease.session());
    // ⚠️ 必须先归还租约再 shutdown() / seed()：
    //   ① shutdown() 会等待在途租约归还，在持有租约时调用它就是自己等自己；
    //   ② seed() 里的每个助手自己取租约，而同一线程持有租约时再 acquire() 会被
    //      db::Pool 的自死锁防护【主动拒绝】（见 db/mysql_pool.h 的 acquire）。
    lease = Lease();
    if (!tables_ready) {
      pool_->shutdown();
      pool_.reset();
      GTEST_SKIP() << "当前库缺少 §3.2 的表，请先执行 ./build/oj 完成启动建表后再跑本组用例";
    }
    seed();
  }

  void TearDown() override {
    if (pool_ != nullptr && pool_->ready()) {
      {
        Lease lease = pool_->acquire();
        if (lease.valid()) clear_probe_rows(lease.session());
      }   // 租约必须在 shutdown() 之前归还 —— 它会等待在途租约，见上
      pool_->shutdown();
    }
    pool_.reset();
  }

  // ---- 环境 ----
  static bool tables_exist(db::Session& session) {
    const db::ResultSet r = session.run(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE()"
        " AND table_name IN ('users','problems','problem_cases','problem_params','problem_tags',"
        "                   'submissions','user_problems','tags')");
    return !r.failed() && !r.empty() && r.at(0).u64(0) == 8;
  }

  static void clear_probe_rows(db::Session& session) {
    // users 侧的 submissions / user_problems 由 CASCADE 带走（§3.6），故只需删 users。
    session.run(std::string("DELETE FROM users WHERE username LIKE '") + kUserPrefix + "%'");
    session.run(std::string("DELETE FROM problems WHERE slug LIKE '") + kProblemPrefix + "%'");
    session.run(std::string("DELETE FROM tags WHERE name LIKE '") + kTagPrefix + "%'");
    session.run(std::string("DELETE FROM tags WHERE name LIKE '") + kSourcePrefix + "%'");
  }

  // ---- 造数据 ----
  unsigned long long add_user(const std::string& suffix) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r = lease->run(
        "INSERT INTO users (username, password_hash, role, created_at)"
        " VALUES (?, 'x', 'user', UTC_TIMESTAMP())",
        {db::Value::from_text(std::string(kUserPrefix) + suffix)});
    EXPECT_FALSE(r.failed()) << r.error().format();
    return r.insert_id();
  }

  long long add_tag(const std::string& name, const std::string& category) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r =
        lease->run("INSERT INTO tags (name, category) VALUES (?, ?)",
                   {db::Value::from_text(name), db::Value::from_text(category)});
    EXPECT_FALSE(r.failed()) << r.error().format();
    return static_cast<long long>(r.insert_id());
  }

  long long add_problem(const std::string& slug_suffix, const std::string& title,
                        const std::string& difficulty, int sort_no, bool featured = false,
                        int case_count = 0, bool with_hint = false) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    // ⚠️ 参数顺序必须与 SQL 文本里 ? 的出现顺序逐字对应：
    //   slug → title → difficulty → hint_md → signature → case_count → is_featured → sort_no
    // 写错顺序不会报"参数个数不符"（个数对上了），而是报 1366（int 塞进 TINYINT）
    // 或 3988（int 绑到 MEDIUMTEXT）—— 那两种报错完全指不到真正错的那一行。
    std::vector<db::Value> params = {
        db::Value::from_text(std::string(kProblemPrefix) + slug_suffix),
        db::Value::from_text(title),
        db::Value::from_text(difficulty),
        with_hint ? db::Value::from_text("提示：先画个草图")
                  : db::Value::null(),   // §4.1 脚注：空串落库为 NULL
        db::Value::from_text("class Solution { public: std::vector<int> twoSum(std::vector<int>&,"
                             " int); };   // 形参：nums, target"),
        db::Value::from_i64(case_count),
        db::Value::from_bool(featured),
        db::Value::from_i64(sort_no),
    };
    const db::ResultSet r = lease->run(
        "INSERT INTO problems (slug, title, difficulty, statement_md, hint_md, signature,"
        " method_name, return_type, code_template, case_count, is_featured, sort_no,"
        " created_at, updated_at)"
        " VALUES (?, ?, ?, '# 题面', ?, ?, 'twoSum', 'std::vector<int>', '', ?, ?, ?,"
        "         UTC_TIMESTAMP(), UTC_TIMESTAMP())",
        params);
    EXPECT_FALSE(r.failed()) << r.error().format();
    const unsigned long long id = r.insert_id();
    for (int i = 0; i < case_count; ++i) {
      const db::ResultSet c = lease->run(
          "INSERT INTO problem_cases (problem_id, case_idx, args_json, expected_json)"
          " VALUES (?, ?, '[]', '[]')",
          {db::Value::from_u64(id), db::Value::from_i64(i)});
      EXPECT_FALSE(c.failed()) << c.error().format();
    }
    return static_cast<long long>(id);
  }

  void link_tag(long long problem_id, long long tag_id) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r =
        lease->run("INSERT INTO problem_tags (problem_id, tag_id) VALUES (?, ?)",
                   {db::Value::from_i64(problem_id), db::Value::from_i64(tag_id)});
    EXPECT_FALSE(r.failed()) << r.error().format();
  }

  void add_param(long long problem_id, int idx, const std::string& name,
                 const std::string& cpp_type) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r = lease->run(
        "INSERT INTO problem_params (problem_id, param_idx, name, cpp_type) VALUES (?, ?, ?, ?)",
        {db::Value::from_i64(problem_id), db::Value::from_i64(idx), db::Value::from_text(name),
         db::Value::from_text(cpp_type)});
    EXPECT_FALSE(r.failed()) << r.error().format();
  }

  // verdict 空串 = NULL：pending/judging 的提交就是 NULL（§3.2）
  void add_submission(unsigned long long user_id, long long problem_id, const std::string& status,
                      const std::string& verdict) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    std::vector<db::Value> params = {db::Value::from_u64(user_id),
                                     db::Value::from_i64(problem_id),
                                     db::Value::from_text(status)};
    if (verdict.empty()) {
      params.emplace_back(db::Value::null());
    } else {
      params.emplace_back(db::Value::from_text(verdict));
    }
    const db::ResultSet r = lease->run(
        "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict,"
        " created_at) VALUES (?, ?, 'cpp', 'class Solution{};', ?, ?, UTC_TIMESTAMP())",
        params);
    EXPECT_FALSE(r.failed()) << r.error().format();
  }

  void set_favorite(unsigned long long user_id, long long problem_id, bool favorite) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r = lease->run(
        "INSERT INTO user_problems (user_id, problem_id, favorite, updated_at)"
        " VALUES (?, ?, ?, UTC_TIMESTAMP())"
        " ON DUPLICATE KEY UPDATE favorite = VALUES(favorite)",
        {db::Value::from_u64(user_id), db::Value::from_i64(problem_id),
         db::Value::from_bool(favorite)});
    EXPECT_FALSE(r.failed()) << r.error().format();
  }

  // ---- 查询助手（每次自己取租约，模拟真实调用路径的 [D-21] 串行化）----
  ProblemList list_for(const ViewerContext& viewer, const ListQuery& query) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    ProblemList list;
    std::string error;
    EXPECT_TRUE(query_problems(lease.session(), &viewer, query, &list, &error)) << error;
    return list;
  }

  // 只查探针题：keyword=探针 命中全部探针题，且【不】命中库内既有题（题库题的标题
  // 里没有"探针"二字）。这是本文件所有筛选断言的收敛条件。
  ListQuery probe_query() {
    ListQuery query;
    query.keyword = kKeyword;
    return query;
  }

  const ProblemListItem* find(const ProblemList& list, const std::string& slug) {
    for (const ProblemListItem& item : list.list) {
      if (item.slug == slug) return &item;
    }
    return nullptr;
  }

  // ---- 种子：4 道探针题 + 3 个 algorithm 标签 + 1 个 source 标签 + 2 个用户 ----
  //  题目            难度    sort_no  featured  标签
  //  alpha(甲)       easy    1        是        arr, hash
  //  beta(乙)        medium  2        否        hash
  //  gamma(丙)       hard    3        否        dp, source
  //  delta(丁)       easy    4        否        （无标签）
  void seed() {
    alice_ = add_user("alice");
    bob_ = add_user("bob");
    tag_arr_ = add_tag(std::string(kTagPrefix) + "arr", "algorithm");
    tag_hash_ = add_tag(std::string(kTagPrefix) + "hash", "algorithm");
    tag_dp_ = add_tag(std::string(kTagPrefix) + "dp", "algorithm");
    tag_source_ = add_tag(std::string(kSourcePrefix) + "huigao", "source");

    p_alpha_ = add_problem("alpha", "探针甲 Two Sum", "easy", 1, true, 2, /*with_hint=*/true);
    p_beta_ = add_problem("beta", "探针乙 百分比 100%", "medium", 2);
    p_gamma_ = add_problem("gamma", "探针丙 动态规划", "hard", 3);
    p_delta_ = add_problem("delta", "探针丁 无标签", "easy", 4);

    link_tag(p_alpha_, tag_arr_);
    link_tag(p_alpha_, tag_hash_);
    link_tag(p_beta_, tag_hash_);
    link_tag(p_gamma_, tag_dp_);
    link_tag(p_gamma_, tag_source_);

    add_param(p_alpha_, 0, "nums", "std::vector<int>&");
    add_param(p_alpha_, 1, "target", "int");

    // alice：alpha 已 AC、beta 只有编译错误、gamma 还在 pending
    add_submission(alice_, p_alpha_, "done", "AC");
    add_submission(alice_, p_beta_, "done", "CE");
    add_submission(alice_, p_gamma_, "pending", "");
    set_favorite(alice_, p_gamma_, true);
    // bob：alpha 答错过 ⇒ attempted（不是 solved）
    add_submission(bob_, p_alpha_, "done", "WA");
  }

  db::PoolOptions options_;
  std::unique_ptr<db::Pool> pool_;
  unsigned long long alice_ = 0;
  unsigned long long bob_ = 0;
  long long p_alpha_ = 0, p_beta_ = 0, p_gamma_ = 0, p_delta_ = 0;
  long long tag_arr_ = 0, tag_hash_ = 0, tag_dp_ = 0, tag_source_ = 0;
};

TEST_F(ProblemApiDbTest, ReturnsTheStoredLibraryForUnfilteredRequests) {
  // 未登录也能查全库（[D-43]：三个端点 Auth=否）。这一条同时是"探针前缀没有
  // 误伤库内既有题"的证据 —— 下面所有用例都靠这个前提收敛断言。
  const ViewerContext anonymous;
  ListQuery all;
  const ProblemList list = list_for(anonymous, all);
  EXPECT_GT(list.total, 4);
  EXPECT_GE(list.list.size(), 4u);
  for (const ProblemListItem& item : list.list) {
    EXPECT_EQ(item.my_progress, Progress::kNone) << item.slug;
    EXPECT_FALSE(item.is_favorite) << item.slug;
  }
}

TEST_F(ProblemApiDbTest, KeywordMatchesTitleAndSlugCaseInsensitively) {
  const ViewerContext anonymous;
  ListQuery query;
  query.keyword = "two sum";
  ProblemList list = list_for(anonymous, query);
  EXPECT_EQ(list.total, 1);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "alpha");   // slug 命中
  EXPECT_TRUE(find(list, std::string(kProblemPrefix) + "alpha") != nullptr);

  query.keyword = "动态规划";   // 中文 title 命中；utf8mb4_0900_ai_ci 大小写不敏感
  list = list_for(anonymous, query);
  EXPECT_EQ(list.total, 1);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "gamma");
}

TEST_F(ProblemApiDbTest, KeywordTreatsLikeWildcardsLiterally) {
  // 用户搜 "100%" 若不转义，% 会变成通配符 → 命中一堆无关题（本题库里的 % 是题目标题的一部分）。
  const ViewerContext anonymous;
  ListQuery query;
  query.keyword = "100%";
  const ProblemList list = list_for(anonymous, query);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "beta");

  query.keyword = "%";   // 单独一个 % 只能命中标题里真的有 % 的那题
  EXPECT_EQ(list_for(anonymous, query).list.size(), 1u);

  query.keyword = "探针_甲";   // _ 是单字符通配符，转义后不该命中
  EXPECT_EQ(list_for(anonymous, query).total, 0);
}

TEST_F(ProblemApiDbTest, DifficultyFilter) {
  const ViewerContext anonymous;
  ListQuery query = probe_query();
  query.has_difficulty = true;
  query.difficulty = model::Difficulty::kEasy;
  ProblemList list = list_for(anonymous, query);
  EXPECT_EQ(list.total, 2);   // alpha + delta
  for (const ProblemListItem& item : list.list) EXPECT_EQ(item.difficulty, "easy");

  query.difficulty = model::Difficulty::kHard;
  list = list_for(anonymous, query);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "gamma");
}

TEST_F(ProblemApiDbTest, TagFilterRequiresAllSelectedTags) {
  const ViewerContext anonymous;
  ListQuery query = probe_query();
  query.tags = {std::string(kTagPrefix) + "arr", std::string(kTagPrefix) + "hash"};
  ProblemList list = list_for(anonymous, query);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "alpha");

  // alpha 与 gamma 没有共同的 algorithm 标签 ⇒ AND 语义下无结果
  query.tags = {std::string(kTagPrefix) + "arr", std::string(kTagPrefix) + "dp"};
  EXPECT_EQ(list_for(anonymous, query).total, 0);

  // source 类标签同样可筛（§6.3 的 tags 不区分 category）
  query = probe_query();
  query.tags = {std::string(kSourcePrefix) + "huigao"};
  list = list_for(anonymous, query);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "gamma");

  query.tags = {"这个标签不存在"};
  EXPECT_EQ(list_for(anonymous, query).total, 0);
}

TEST_F(ProblemApiDbTest, TagsAreSortedDeterministicallyAndEmptyIsEmptyArray) {
  // JSON_ARRAYAGG 不保证顺序（见 src 的文件头），顺序由 C++ 侧定死。
  // 否则同一次查询两次返回的 JSON 不同，"标签顺序"这种断言会随机红。
  const ViewerContext anonymous;
  const ProblemList list = list_for(anonymous, probe_query());
  const ProblemListItem* alpha = find(list, std::string(kProblemPrefix) + "alpha");
  const ProblemListItem* gamma = find(list, std::string(kProblemPrefix) + "gamma");
  std::size_t delta_index = list.list.size();
  for (std::size_t i = 0; i < list.list.size(); ++i) {
    if (list.list[i].slug == std::string(kProblemPrefix) + "delta") delta_index = i;
  }
  ASSERT_TRUE(alpha != nullptr);
  ASSERT_TRUE(gamma != nullptr);
  ASSERT_NE(delta_index, list.list.size());

  // alpha 挂 arr + hash 两个 algorithm 标签：组内按 name 升序
  ASSERT_EQ(alpha->tags.size(), 2u);
  EXPECT_EQ(alpha->tags[0].category, "algorithm");
  EXPECT_EQ(alpha->tags[1].category, "algorithm");
  EXPECT_LT(alpha->tags[0].name, alpha->tags[1].name);

  // gamma 挂 dp(algorithm) + 高频(source)：按 (category, name) 升序 ⇒ algorithm 在前
  ASSERT_EQ(gamma->tags.size(), 2u);
  EXPECT_EQ(gamma->tags[0].category, "algorithm");
  EXPECT_EQ(gamma->tags[1].category, "source");

  // 无标签题不能出现 {"name":null} 的假标签（src 的文件头解释了那个 CASE WHEN 的来历）
  EXPECT_TRUE(list.list[delta_index].tags.empty());
  const nlohmann::json parsed = nlohmann::json::parse(to_json(list));
  ASSERT_EQ(parsed["list"].size(), list.list.size());
  const nlohmann::json& delta_row = parsed["list"][delta_index];
  ASSERT_TRUE(delta_row["tags"].is_array());
  EXPECT_EQ(delta_row["tags"].size(), 0u);
}

TEST_F(ProblemApiDbTest, ProgressIsDerivedFromSubmissions) {
  // §6.4 的三态。beta 只有一条 CE 提交 ⇒ attempted（AC-11：「仅编译错误的题标记尝试过」）。
  const ViewerContext alice_viewer{alice_, "alice"};
  const ProblemList list = list_for(alice_viewer, probe_query());
  ASSERT_EQ(list.list.size(), 4u);
  const ProblemListItem* alpha = find(list, std::string(kProblemPrefix) + "alpha");
  const ProblemListItem* beta = find(list, std::string(kProblemPrefix) + "beta");
  const ProblemListItem* gamma = find(list, std::string(kProblemPrefix) + "gamma");
  const ProblemListItem* delta = find(list, std::string(kProblemPrefix) + "delta");
  ASSERT_TRUE(alpha && beta && gamma && delta);
  EXPECT_EQ(alpha->my_progress, Progress::kSolved);
  EXPECT_EQ(beta->my_progress, Progress::kAttempted);
  EXPECT_EQ(gamma->my_progress, Progress::kAttempted);   // pending 也算"做过"
  EXPECT_EQ(delta->my_progress, Progress::kNone);

  // 别人的提交不算我的进度
  const ViewerContext bob_viewer{bob_, "bob"};
  const ProblemList bob_list = list_for(bob_viewer, probe_query());
  EXPECT_EQ(find(bob_list, std::string(kProblemPrefix) + "alpha")->my_progress, Progress::kAttempted);
  EXPECT_EQ(find(bob_list, std::string(kProblemPrefix) + "gamma")->my_progress, Progress::kNone);
}

TEST_F(ProblemApiDbTest, ProgressFilterIsMutuallyExclusiveWithSolved) {
  const ViewerContext alice_viewer{alice_, "alice"};
  ListQuery query = probe_query();

  query.progress = Progress::kSolved;
  ProblemList list = list_for(alice_viewer, query);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "alpha");

  // attempted = 做过但没通过（AC-11）。alpha 已通过 ⇒ 不出现在 attempted 里。
  // 写成"有过任意提交"会让两个筛选重叠，筛选栏里的三个选项就不再互斥。
  query.progress = Progress::kAttempted;
  list = list_for(alice_viewer, query);
  EXPECT_EQ(list.total, 2);
  for (const ProblemListItem& item : list.list) {
    EXPECT_NE(item.slug, std::string(kProblemPrefix) + "alpha");
  }

  query.progress = Progress::kNone;
  EXPECT_EQ(list_for(alice_viewer, query).total, 4);
}

TEST_F(ProblemApiDbTest, FavoriteFilterAndFlagComeFromUserProblems) {
  const ViewerContext alice_viewer{alice_, "alice"};
  ListQuery query = probe_query();
  query.favorite_only = true;
  const ProblemList list = list_for(alice_viewer, query);
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "gamma");
  EXPECT_TRUE(list.list[0].is_favorite);

  // 收藏是 per-user 的：bob 没收藏 alpha
  const ViewerContext bob_viewer{bob_, "bob"};
  query.favorite_only = true;
  EXPECT_EQ(list_for(bob_viewer, query).total, 0);
  const ProblemList bob_list = list_for(bob_viewer, probe_query());
  EXPECT_FALSE(find(bob_list, std::string(kProblemPrefix) + "gamma")->is_favorite);
}

TEST_F(ProblemApiDbTest, AnonymousPersonalFiltersAreRejectedAndLeaveOutUntouched) {
  const ViewerContext anonymous;
  ListQuery query = probe_query();
  query.progress = Progress::kSolved;

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ProblemList list;
  list.total = 999;
  std::string error;
  EXPECT_FALSE(query_problems(lease.session(), &anonymous, query, &list, &error));
  EXPECT_NE(error.find("progress"), std::string::npos);
  EXPECT_EQ(list.total, 999);   // 失败时 out 不变（与 admin.cpp 同一不变式）
}

TEST_F(ProblemApiDbTest, PaginationCoversEveryRowExactlyOnce) {
  const ViewerContext anonymous;
  ListQuery query = probe_query();
  query.size = 1;

  std::vector<std::string> slugs;
  for (int page = 1; page <= 4; ++page) {
    query.page = page;
    const ProblemList list = list_for(anonymous, query);
    EXPECT_EQ(list.total, 4) << "第 " << page << " 页的 total 不该随翻页变化";
    EXPECT_EQ(list.page, page);
    EXPECT_EQ(list.size, 1);
    ASSERT_EQ(list.list.size(), 1u);
    slugs.push_back(list.list[0].slug);
  }
  std::sort(slugs.begin(), slugs.end());
  EXPECT_EQ(std::adjacent_find(slugs.begin(), slugs.end()), slugs.end()) << "翻页出现重复";

  query.page = 99;   // 越界页：服务端夹回真实最大页（P1-6），前端跟着回显走
  const ProblemList beyond = list_for(anonymous, query);
  EXPECT_EQ(beyond.page, 4);
  EXPECT_EQ(beyond.total, 4);
  ASSERT_EQ(beyond.list.size(), 1u);   // 夹页后返回末页内容，不再空页死局
}

TEST_F(ProblemApiDbTest, SortModesAreOrderedAndDeterministic) {
  const ViewerContext anonymous;
  ListQuery query = probe_query();

  query.sort = ProblemSort::kDefault;   // sort_no ASC
  ProblemList list = list_for(anonymous, query);
  ASSERT_EQ(list.list.size(), 4u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "alpha");   // sort_no=1
  EXPECT_EQ(list.list[3].slug, std::string(kProblemPrefix) + "delta");   // sort_no=4

  query.sort = ProblemSort::kFeatured;   // 精选置顶，其余仍按 sort_no
  list = list_for(anonymous, query);
  EXPECT_TRUE(list.list[0].is_featured);

  query.sort = ProblemSort::kEasiest;    // easy < medium < hard（ENUM 声明序）
  list = list_for(anonymous, query);
  ASSERT_EQ(list.list.size(), 4u);
  EXPECT_EQ(list.list[0].difficulty, "easy");
  EXPECT_EQ(list.list[3].difficulty, "hard");

  query.sort = ProblemSort::kHardest;
  list = list_for(anonymous, query);
  EXPECT_EQ(list.list[0].difficulty, "hard");

  query.sort = ProblemSort::kEasiest;    // 同一模式连查两次顺序必须一致
  list = list_for(anonymous, query);
  const ProblemList again = list_for(anonymous, query);
  ASSERT_EQ(again.list.size(), list.list.size());
  ASSERT_EQ(list.list.size(), 4u);
  for (std::size_t i = 0; i < list.list.size(); ++i) {
    EXPECT_EQ(list.list[i].slug, again.list[i].slug) << "同一模式两次查询顺序不一致";
  }
}

TEST_F(ProblemApiDbTest, TagGroupsSplitByCategoryAndAreSorted) {
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  TagGroups groups;
  std::string error;
  ASSERT_TRUE(load_tag_groups(lease.session(), &groups, &error)) << error;

  // 探针标签按 (category, name) 落位；组内按 name 升序
  const auto has = [](const std::vector<std::string>& group, const std::string& name) {
    return std::find(group.begin(), group.end(), name) != group.end();
  };
  EXPECT_TRUE(has(groups.algorithm, std::string(kTagPrefix) + "arr"));
  EXPECT_TRUE(has(groups.algorithm, std::string(kTagPrefix) + "dp"));
  EXPECT_TRUE(has(groups.source, std::string(kSourcePrefix) + "huigao"));
  for (std::size_t i = 1; i < groups.algorithm.size(); ++i) {
    EXPECT_LE(groups.algorithm[i - 1], groups.algorithm[i]);
  }
  for (std::size_t i = 1; i < groups.source.size(); ++i) {
    EXPECT_LE(groups.source[i - 1], groups.source[i]);
  }
  // algorithm / source 两组不得互相串味（§6.3 的响应就是靠这个分类渲染筛选栏）
  EXPECT_FALSE(has(groups.source, std::string(kTagPrefix) + "arr"));
  EXPECT_FALSE(has(groups.algorithm, std::string(kSourcePrefix) + "huigao"));
}

TEST_F(ProblemApiDbTest, DetailReturnsStatementHintTagsAndParams) {
  const ViewerContext alice_viewer{alice_, "alice"};
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());

  ProblemDetail detail;
  std::string error;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "alpha",
                                &alice_viewer, &detail, &error),
            DetailStatus::kFound)
      << error;
  EXPECT_EQ(detail.title, "探针甲 Two Sum");
  EXPECT_EQ(detail.statement_md, "# 题面");
  EXPECT_EQ(detail.hint_md, "提示：先画个草图");
  EXPECT_EQ(detail.case_count, 2);
  EXPECT_TRUE(detail.is_featured);
  EXPECT_EQ(detail.my_progress, Progress::kSolved);
  EXPECT_EQ(detail.method_name, "twoSum");
  EXPECT_EQ(detail.return_type, "std::vector<int>");
  EXPECT_EQ(detail.time_limit_ms, 3000u);
  EXPECT_EQ(detail.mem_limit_kb, 131072u);

  ASSERT_EQ(detail.tags.size(), 2u);
  EXPECT_EQ(detail.tags[0].category, "algorithm");
  EXPECT_EQ(detail.tags[1].category, "algorithm");

  ASSERT_EQ(detail.params.size(), 2u);
  EXPECT_EQ(detail.params[0].name, "nums");     // 按 param_idx 升序（决定调用顺序）
  EXPECT_EQ(detail.params[0].cpp_type, "std::vector<int>&");
  EXPECT_EQ(detail.params[1].name, "target");

  const nlohmann::json parsed = nlohmann::json::parse(to_json(detail));
  EXPECT_EQ(parsed["hint_md"], "提示：先画个草图");
  EXPECT_EQ(parsed["caseCount"], 2);
  EXPECT_EQ(parsed["myProgress"], "solved");
  EXPECT_EQ(parsed["methodName"], "twoSum");
  ASSERT_EQ(parsed["params"].size(), 2u);
  EXPECT_EQ(parsed["params"][0]["cppType"], "std::vector<int>&");
}

TEST_F(ProblemApiDbTest, DetailReportsMissingSlugAs404) {
  const ViewerContext anonymous;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ProblemDetail detail;
  detail.id = 12345;   // 不变式素材
  std::string error;
  EXPECT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "不存在", &anonymous,
                                &detail, &error),
            DetailStatus::kNotFound);
  EXPECT_EQ(detail.id, 12345);   // 404 也不算失败：out 不该被改
  EXPECT_EQ(error_code_of(DetailStatus::kNotFound), 1004);
  EXPECT_EQ(error_code_of(DetailStatus::kFound), 0);
  EXPECT_EQ(error_code_of(DetailStatus::kError), 1008);
}

TEST_F(ProblemApiDbTest, DetailOfHintlessProblemSerializesHintAsNull) {
  const ViewerContext anonymous;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ProblemDetail detail;
  std::string error;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "delta", &anonymous,
                                &detail, &error),
            DetailStatus::kFound)
      << error;
  EXPECT_TRUE(detail.hint_md.empty());   // 库里是 NULL
  EXPECT_TRUE(nlohmann::json::parse(to_json(detail))["hint_md"].is_null());
  EXPECT_TRUE(detail.tags.empty());
  EXPECT_TRUE(detail.params.empty());
}

TEST_F(ProblemApiDbTest, RecentSubmissionsAreLimitedFiveAndOwnOnly) {
  const ViewerContext alice_viewer{alice_, "alice"};
  const ViewerContext bob_viewer{bob_, "bob"};
  // alice 在 gamma 上连投 6 条 ⇒ 只回最近 5 条（§6.3 的 recentSubmissions）
  for (int i = 0; i < 6; ++i) add_submission(alice_, p_gamma_, "done", "WA");
  // bob 在同一题上的提交不该出现
  add_submission(bob_, p_gamma_, "done", "AC");

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ProblemDetail detail;
  std::string error;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "gamma",
                                &alice_viewer, &detail, &error),
            DetailStatus::kFound)
      << error;
  ASSERT_EQ(detail.recent_submissions.size(), kRecentSubmissionLimit);
  for (const RecentSubmission& entry : detail.recent_submissions) {
    EXPECT_EQ(entry.status, "done");
    EXPECT_EQ(entry.verdict, "WA");   // 别人的 AC 不在里面
    EXPECT_EQ(entry.has_case_passed, false);
  }
  // 按 id 倒序（最新在前）
  for (std::size_t i = 1; i < detail.recent_submissions.size(); ++i) {
    EXPECT_GT(detail.recent_submissions[i - 1].id, detail.recent_submissions[i].id);
  }
}

TEST_F(ProblemApiDbTest, AnonymousDetailOmitsPersonalFieldsEntirely) {
  const ViewerContext anonymous;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ProblemDetail detail;
  std::string error;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "gamma", &anonymous,
                                &detail, &error),
            DetailStatus::kFound)
      << error;
  EXPECT_FALSE(detail.authenticated);
  EXPECT_EQ(detail.my_progress, Progress::kNone);
  EXPECT_FALSE(detail.is_favorite);
  EXPECT_TRUE(detail.recent_submissions.empty());
  EXPECT_FALSE(detail.next_recommendation.has_value());
  const nlohmann::json parsed = nlohmann::json::parse(to_json(detail));
  EXPECT_FALSE(parsed.contains("nextRecommendation"));   // AC-25
  EXPECT_TRUE(parsed.contains("recentSubmissions"));     // 但数组仍存在（前端直接 map）
  EXPECT_EQ(parsed["hint_md"].is_null(), true);
  EXPECT_EQ(parsed["statementMd"], "# 题面");            // 题面照常可读（[D-43]）
}

TEST_F(ProblemApiDbTest, RecommendationPicksTheWeakestTagAndIsExplainable) {
  // 种子下的通过率：
  //   arr  : 题目 {alpha}          alice AC {alpha}      ⇒ 1/1，薄弱度 0
  //   hash : 题目 {alpha, beta}     alice AC {alpha}      ⇒ 1/2，薄弱度 0.5
  //   dp   : 题目 {gamma}           alice 无 AC           ⇒ 0/1，薄弱度 1.0
  // 候选（未通过且有 algorithm 标签）：beta(hash)、gamma(dp)、delta(无标签，不进候选)
  // ⇒ 最高分是 gamma（薄弱度 1.0 → score 500，比 beta 的 666.7 低）
  //    等等 —— score = 1000/(1+薄弱度)，薄弱度越小分越高 ⇒ beta(0.5→666.7) 胜出。
  const ViewerContext alice_viewer{alice_, "alice"};
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ProblemDetail detail;
  std::string error;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "alpha",
                                &alice_viewer, &detail, &error),
            DetailStatus::kFound)
      << error;
  ASSERT_TRUE(detail.next_recommendation.has_value())
      << "未全部通过时必须有推荐：" << error;
  EXPECT_EQ(detail.next_recommendation->slug, std::string(kProblemPrefix) + "beta");
  EXPECT_EQ(detail.next_recommendation->tag, std::string(kTagPrefix) + "hash");
  EXPECT_EQ(detail.next_recommendation->tag_solved, 1);
  EXPECT_EQ(detail.next_recommendation->tag_total, 2);
  EXPECT_NE(detail.next_recommendation->reason.find(kTagPrefix),
            std::string::npos);   // 理由与主标签同源（[D-59]/AC-34）
  EXPECT_NEAR(detail.next_recommendation->score, 1000.0 / 1.5, 1e-9);
}

TEST_F(ProblemApiDbTest, RecommendationIsNullWhenEverythingIsSolved) {
  // §7.4 的「🎉 题库 N/N 全部通过」：全部通过 ⇒ nextRecommendation 为 null（字段仍在）。
  //
  // ⚠️ "全部"必须覆盖【库里的每一道题】，包括 T1.2 从 problems/*.json 同步进来的题库题
  //   （two-sum）。只给探针题补 AC 的话，推荐会落到那道题库题上 —— 那不是规则错了，
  //   是这条用例的前提没写全。故这里先 SELECT 出全部 problem id 再逐个补 AC。
  const unsigned long long carol = add_user("carol");
  std::vector<long long> every_problem;
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    const db::ResultSet r = lease->run("SELECT id FROM problems ORDER BY id");
    ASSERT_FALSE(r.failed()) << r.error().format();
    for (const db::Row& row : r.rows()) every_problem.push_back(row.i64(0));
  }
  ASSERT_FALSE(every_problem.empty());
  for (const long long problem_id : every_problem) {
    add_submission(carol, problem_id, "done", "AC");
  }

  const ViewerContext carol_viewer{carol, "carol"};
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::optional<NextRecommendation> recommendation;
  std::string error;
  ASSERT_TRUE(
      compute_next_recommendation(lease.session(), carol_viewer, &recommendation, &error))
      << error;
  EXPECT_FALSE(recommendation.has_value());

  // 详情端点里同样表现为"字段存在但为 null"，而不是字段缺失（后者是未登录的形状）
  ProblemDetail detail;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "alpha",
                                &carol_viewer, &detail, &error),
            DetailStatus::kFound)
      << error;
  ASSERT_TRUE(detail.authenticated);
  EXPECT_FALSE(detail.next_recommendation.has_value());
  const nlohmann::json parsed = nlohmann::json::parse(to_json(detail));
  ASSERT_TRUE(parsed.contains("nextRecommendation"));
  EXPECT_TRUE(parsed["nextRecommendation"].is_null());
}

TEST_F(ProblemApiDbTest, RecommendationIgnoresProblemsWithoutAlgorithmTags) {
  // [D-68]：只有 source 类标签的题没有主标签 ⇒ 不进候选集。
  // delta（无标签）与 gamma（只有 dp 是 algorithm，dp 也算）—— 这里另造一道
  // "只有 source 标签"的题，验证它永远不会被推荐。
  const long long only_source = add_problem("only-source", "探针戊 纯来源标签", "easy", 5);
  link_tag(only_source, tag_source_);
  set_favorite(alice_, only_source, true);

  const ViewerContext alice_viewer{alice_, "alice"};
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::optional<NextRecommendation> recommendation;
  std::string error;
  ASSERT_TRUE(
      compute_next_recommendation(lease.session(), alice_viewer, &recommendation, &error))
      << error;
  ASSERT_TRUE(recommendation.has_value());
  EXPECT_NE(recommendation->slug, std::string(kProblemPrefix) + "only-source");
  EXPECT_NE(recommendation->slug, std::string(kProblemPrefix) + "delta");   // 无标签题也不进
}

TEST_F(ProblemApiDbTest, RecommendationUsesTheLowestPassRateTagNotTheSmallestId) {
  // [D-68] 的数据库侧验证：主标签取【用户通过率最低】的那个标签，
  // 而不是 tags.id 最小的那个 —— 下面刻意让"id 小的标签"恰好是"通过率高的那个"，
  // 于是按 id 取的实现会选错标签，理由字符串也会跟着错（[D-59]/AC-34）。
  //
  // 造的数据（dave 是个新用户，先把他通过【库里已有的全部题】，
  // 让候选集只剩本用例造的三道 —— 否则题库题与探针题会混进候选，推荐就不可核对）：
  //   tag_strong(id 小) 挂在 p_strong 与 p_target 上，dave 已通过 p_strong ⇒ 1/2，薄弱度 0.5
  //   tag_weak  (id 大) 挂在 p_target 与 p_other  上，dave 一道没通过 ⇒ 0/2，薄弱度 1.0
  //   p_target(easy, sort_no 1) 挂两个标签 ⇒ 主标签必是 tag_weak ⇒ score 500
  //   p_other (hard, sort_no 2) 只挂 tag_weak     ⇒ score 500（同分）
  //   同分先比难度 ⇒ p_target 胜出
  const unsigned long long dave = add_user("dave");
  std::vector<long long> existing_problems;
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    const db::ResultSet r = lease->run("SELECT id FROM problems ORDER BY id");
    ASSERT_FALSE(r.failed()) << r.error().format();
    for (const db::Row& row : r.rows()) existing_problems.push_back(row.i64(0));
  }   // 租约必须在调用助手【之前】归还：同一线程持有租约时 acquire() 会被主动拒绝
  for (const long long problem_id : existing_problems) {
    add_submission(dave, problem_id, "done", "AC");
  }

  const long long tag_strong = add_tag(std::string(kTagPrefix) + "strong", "algorithm");
  const long long tag_weak = add_tag(std::string(kTagPrefix) + "weak", "algorithm");
  ASSERT_LT(tag_strong, tag_weak) << "本用例的前提：strong 的 id 更小";

  const long long p_strong = add_problem("strong", "探针巳 强标签题", "medium", 1);
  link_tag(p_strong, tag_strong);
  add_submission(dave, p_strong, "done", "AC");      // tag_strong 通过率因此是 1/2

  const long long p_target = add_problem("target", "探针午 目标题", "easy", 2);
  link_tag(p_target, tag_strong);
  link_tag(p_target, tag_weak);

  const long long p_other = add_problem("other", "探针未 陪跑题", "hard", 3);
  link_tag(p_other, tag_weak);

  const ViewerContext dave_viewer{dave, "dave"};
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::optional<NextRecommendation> recommendation;
  std::string error;
  ASSERT_TRUE(
      compute_next_recommendation(lease.session(), dave_viewer, &recommendation, &error))
      << error;
  ASSERT_TRUE(recommendation.has_value());
  EXPECT_EQ(recommendation->slug, std::string(kProblemPrefix) + "target");
  // 关键断言：主标签是通过率最低的 weak（0/2），不是 id 更小的 strong
  EXPECT_EQ(recommendation->tag, std::string(kTagPrefix) + "weak");
  EXPECT_EQ(recommendation->tag_solved, 0);
  EXPECT_EQ(recommendation->tag_total, 2);
  EXPECT_NE(recommendation->reason.find(kTagPrefix), std::string::npos);
  EXPECT_NE(recommendation->reason.find("weak"), std::string::npos);
  EXPECT_EQ(recommendation->reason.find("strong"), std::string::npos)
      << "理由里出现了非主标签 strong，可解释性与算分不同源（[D-59]）";
  EXPECT_NEAR(recommendation->score, 500.0, 1e-9);
}

TEST_F(ProblemApiDbTest, FailedQueryLeavesOutUntouched) {
  // 默认构造的 Session 没有连接，run() 是【惰性失败】（见 db/mysql_pool.h），
  // 因此不接真库也能走通失败路径 —— 而失败路径才是最容易写出"半成品"的地方。
  db::Session dead;
  const ViewerContext anonymous;
  ListQuery query;
  ProblemList list;
  list.total = 5;
  std::string error;
  EXPECT_FALSE(query_problems(dead, &anonymous, query, &list, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(list.total, 5);

  ProblemDetail detail;
  detail.id = 7;
  EXPECT_EQ(load_problem_detail(dead, "whatever", &anonymous, &detail, &error),
            DetailStatus::kError);
  EXPECT_EQ(error_code_of(DetailStatus::kError), 1008);
  EXPECT_EQ(detail.id, 7);

  TagGroups tags;
  tags.algorithm = {"已加载"};
  EXPECT_FALSE(load_tag_groups(dead, &tags, &error));
  EXPECT_EQ(tags.algorithm.size(), 1u);
}

TEST_F(ProblemApiDbTest, InvalidPagingConstructedByHandIsRejected) {
  // parse_list_query 已经夹过一遍，直接构造 ListQuery 的调用方（将来的内部复用）
  // 也必须被拦住：size=0 会让 LIMIT 0 返回空列表（看起来像"库里没题"）。
  const ViewerContext anonymous;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ListQuery query;
  query.size = 0;
  ProblemList list;
  std::string error;
  EXPECT_FALSE(query_problems(lease.session(), &anonymous, query, &list, &error));
  EXPECT_NE(error.find("size"), std::string::npos);
}

TEST_F(ProblemApiDbTest, OversizedSizeConstructedByHandIsClamped) {
  // 与上面那条互补：size 上限不能只靠解析层。内部调用方直接构造 ListQuery 时
  // 若带着 size=100000 进来，串行化的 DB 上这一次查询会把同期的所有请求一起拖住
  // （§1.4 的 P95 < 200ms）。夹取后回显的 size 是【实际生效值】，前端据此画分页器。
  const ViewerContext anonymous;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  ListQuery query;
  query.keyword = kKeyword;
  query.size = 100000;
  ProblemList list;
  std::string error;
  ASSERT_TRUE(query_problems(lease.session(), &anonymous, query, &list, &error)) << error;
  EXPECT_EQ(list.size, kMaxPageSize);
  EXPECT_EQ(nlohmann::json::parse(to_json(list))["size"], kMaxPageSize);
}

TEST_F(ProblemApiDbTest, HarnessPreludeIsNullUnlessTheProblemNeedsIt) {
  // §4.2 的 __expr 逃生舱：prelude 空 = 库里 NULL = 序列化为 null；
  // 非空则原样下发（判题侧与前端都要用它，所以本模块不改写它的内容）。
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  const db::ResultSet r = lease->run(
      "UPDATE problems SET harness_prelude = ? WHERE slug = ?",
      {db::Value::from_text("struct TreeNode{int val; TreeNode* left;};  // 逃生舱前向声明"),
       db::Value::from_text(std::string(kProblemPrefix) + "gamma")});
  ASSERT_FALSE(r.failed()) << r.error().format();

  const ViewerContext anonymous;
  ProblemDetail detail;
  std::string error;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "gamma", &anonymous,
                                &detail, &error),
            DetailStatus::kFound)
      << error;
  EXPECT_NE(detail.harness_prelude.find("TreeNode"), std::string::npos);
  EXPECT_EQ(nlohmann::json::parse(to_json(detail))["harnessPrelude"].get<std::string>(),
            detail.harness_prelude);

  // delta 没有任何 prelude ⇒ 仍是 null
  ProblemDetail plain;
  ASSERT_EQ(load_problem_detail(lease.session(), std::string(kProblemPrefix) + "delta", &anonymous,
                                &plain, &error),
            DetailStatus::kFound)
      << error;
  EXPECT_TRUE(plain.harness_prelude.empty());
  EXPECT_TRUE(nlohmann::json::parse(to_json(plain))["harnessPrelude"].is_null());
}

// =============================================================================
// C 段 · 单例池路径：四个"自己取 [D-21] 租约"的重载 + 它们的失败分支
//
// 【为什么要单独一组】上面 B 段的全部用例都走"调用方递进来的 Session"，
// 而生产路径（路由层）走的是【不带 Session 的重载】—— 那是四个会自己 acquire() 的入口，
// 它们各自有一句"连接池未就绪"的分支。这四条分支之前完全没有用例，
// 而它们的失败方式（返回 false / kError）是路由层映射 1008 的依据。
// 单例池按 db/mysql_pool.h 的说明只由 Pool::instance() 提供，故本组必须驱动它。
// ⚠️ TearDown 里先关单例再关自建池：shutdown() 会等待在途租约归还，交叉等待会挂。
// =============================================================================

class ProblemApiPoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_ = base_options();
    Pool probe;
    if (!probe.init(options_)) {
      GTEST_SKIP() << "无法连接 MySQL " << options_.host << ":" << options_.port << "/"
                   << options_.database << "，跳过依赖真实数据库的用例（设置 MYSQL_* 可指定）";
    }
    probe.shutdown();

    // 造数走自建池，被测入口走进程级单例（生产路径）。两个 Pool 共用同一把
    // 全局互斥量（[D-21]），但本组是单线程串行调用，不会互相等。
    pool_.reset(new Pool());
    ASSERT_TRUE(pool_->init(options_)) << pool_->describe();
    {
      Lease lease = pool_->acquire();
      ASSERT_TRUE(lease.valid());
      clear_probe_rows(lease.session());
      const db::ResultSet r = lease->run(
          "INSERT INTO users (username, password_hash, role, created_at)"
          " VALUES (?, 'x', 'user', UTC_TIMESTAMP())",
          {db::Value::from_text(std::string(kUserPrefix) + "pool")});
      ASSERT_FALSE(r.failed()) << r.error().format();
      user_ = r.insert_id();
      const db::ResultSet p = lease->run(
          "INSERT INTO problems (slug, title, difficulty, statement_md, signature, method_name,"
          " return_type, code_template, case_count, sort_no, created_at, updated_at)"
          " VALUES (?, '探针池 单例路径', 'easy', '# s', 'class Solution{};', 'f', 'int', '', 0, 1,"
          "         UTC_TIMESTAMP(), UTC_TIMESTAMP())",
          {db::Value::from_text(std::string(kProblemPrefix) + "pool")});
      ASSERT_FALSE(p.failed()) << p.error().format();
      problem_id_ = static_cast<long long>(p.insert_id());

      // 让这个用户通过【库里已有的每一道题】（含 T1.2 同步进来的题库题）。
      // 于是推荐候选集只剩本组造的那道"没有 algorithm 标签"的题 ⇒ [D-68] 判定无候选。
      // 不这么做的话，题库里的 two-sum 会成为候选，推荐结果就依赖题库状态而不可核对。
      const db::ResultSet ids = lease->run("SELECT id FROM problems ORDER BY id");
      ASSERT_FALSE(ids.failed()) << ids.error().format();
      for (const db::Row& row : ids.rows()) {
        const db::ResultSet s = lease->run(
            "INSERT INTO submissions (user_id, problem_id, language, code, status, verdict,"
            " created_at) VALUES (?, ?, 'cpp', 'class Solution{};', 'done', 'AC',"
            "                       UTC_TIMESTAMP())",
            {db::Value::from_u64(user_), db::Value::from_u64(row.u64(0))});
        ASSERT_FALSE(s.failed()) << s.error().format();
      }
    }
    ASSERT_TRUE(Pool::instance().init(options_));
    singleton_ready_ = true;
  }

  void TearDown() override {
    if (singleton_ready_ && Pool::instance().ready()) Pool::instance().shutdown();
    if (pool_ != nullptr && pool_->ready()) {
      {
        Lease lease = pool_->acquire();
        if (lease.valid()) clear_probe_rows(lease.session());
      }
      pool_->shutdown();
    }
    pool_.reset();
  }

  static void clear_probe_rows(db::Session& session) {
    session.run(std::string("DELETE FROM users WHERE username LIKE '") + kUserPrefix + "%'");
    session.run(std::string("DELETE FROM problems WHERE slug LIKE '") + kProblemPrefix + "%'");
  }

  db::PoolOptions options_;
  std::unique_ptr<db::Pool> pool_;
  bool singleton_ready_ = false;
  unsigned long long user_ = 0;
  long long problem_id_ = 0;
};

TEST_F(ProblemApiPoolTest, LeaseTakingOverloadsWorkOnTheSingletonPool) {
  // 四个入口各调一次：它们平时不会被别的用例走到（生产代码走它们，单测走 Session 重载），
  // 而"路由层接上来之后第一个崩的就是这里"。
  TagGroups tags;
  std::string error;
  ASSERT_TRUE(load_tag_groups(&tags, &error)) << error;
  // 本组只造了一个用户和一道题、没造标签 ⇒ 结果里不得出现任何探针标签。
  // 断言用"前缀不存在"而不是"两个数组都空"：库里本来就有题库自带的标签
  // （T1.2 从 problems/*.json 同步进来），断言空数组会在题库一变就红。
  for (const std::string& name : tags.algorithm) {
    EXPECT_EQ(name.rfind(kTagPrefix, 0), std::string::npos) << name;
  }
  for (const std::string& name : tags.source) {
    EXPECT_EQ(name.rfind(kSourcePrefix, 0), std::string::npos) << name;
  }

  const ViewerContext anonymous;
  ListQuery query;
  query.keyword = kKeyword;
  ProblemList list;
  ASSERT_TRUE(query_problems(&anonymous, query, &list, &error)) << error;
  ASSERT_EQ(list.list.size(), 1u);
  EXPECT_EQ(list.list[0].slug, std::string(kProblemPrefix) + "pool");

  ProblemDetail detail;
  EXPECT_EQ(load_problem_detail(std::string(kProblemPrefix) + "pool", &anonymous, &detail, &error),
            DetailStatus::kFound)
      << error;
  EXPECT_EQ(detail.title, "探针池 单例路径");
  EXPECT_EQ(load_problem_detail("这个 slug 不存在", &anonymous, &detail, &error),
            DetailStatus::kNotFound);

  const ViewerContext viewer{user_, "pool"};
  std::optional<NextRecommendation> recommendation;
  ASSERT_TRUE(compute_next_recommendation(viewer, &recommendation, &error)) << error;
  // 种子题没有 algorithm 标签 ⇒ 无候选 ⇒ 无推荐（[D-68] 的另一侧）
  EXPECT_FALSE(recommendation.has_value());
}

TEST_F(ProblemApiPoolTest, RecommendationRefusesAnAnonymousContext) {
  // 调用方（T3.17 之外的未来入口）递进来 user_id=0 时必须【失败】而不是"给匿名用户
  // 算一份推荐"：那种结果在页面上会显示成一张真实存在的建议卡，内容却毫无依据。
  const ViewerContext anonymous;
  std::optional<NextRecommendation> recommendation;
  recommendation = NextRecommendation{};   // 不变式素材
  std::string error;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  EXPECT_FALSE(compute_next_recommendation(lease.session(), anonymous, &recommendation, &error));
  EXPECT_NE(error.find("匿名"), std::string::npos);
  EXPECT_TRUE(recommendation.has_value()) << "失败时 out 不该被改写";
}

TEST_F(ProblemApiPoolTest, EveryEntryFailsGracefullyWhenThePoolIsNotReady) {
  // 路由层会在【任何时候】收到请求，包括服务还没连上 DB、或刚刚 shutdown 的那几秒。
  // 这时四个入口必须返回可映射的失败（1008 / kError），而不是崩在空指针上 ——
  // 惰性失败正是 db::Pool 的设计（见 mysql_pool.h 的 Lease 注释）。
  if (Pool::instance().ready()) Pool::instance().shutdown();

  std::string error;

  TagGroups tags;
  tags.algorithm = {"原值"};
  EXPECT_FALSE(load_tag_groups(&tags, &error));
  EXPECT_EQ(tags.algorithm.size(), 1u);
  EXPECT_NE(error.find("连接池未就绪"), std::string::npos);

  const ViewerContext anonymous;
  ListQuery query;
  ProblemList list;
  list.total = 42;
  EXPECT_FALSE(query_problems(&anonymous, query, &list, &error));
  EXPECT_EQ(list.total, 42);

  ProblemDetail detail;
  detail.id = 7;
  EXPECT_EQ(load_problem_detail("whatever", &anonymous, &detail, &error), DetailStatus::kError);
  EXPECT_EQ(detail.id, 7);

  std::optional<NextRecommendation> recommendation;
  EXPECT_FALSE(compute_next_recommendation(anonymous, &recommendation, &error));
  EXPECT_FALSE(recommendation.has_value());
}

// ---- 枚举表与错误文案（纯逻辑，永远会跑）----------------------------------------

TEST(ProblemApiEnum, ProgressAndSortRoundTrip) {
  // 枚举 ↔ 文本的对应关系是 API 契约的一部分（前端直接拿这些字符串渲染徽标与下拉框）。
  // 顺带钉住 parse_* 的"未命中不写出参"约定：调用点若忘了判返回值，就会拿着
  // 上一次的旧值继续用，而那种错不会立刻崩。
  for (Progress value : {Progress::kNone, Progress::kAttempted, Progress::kSolved}) {
    Progress parsed = Progress::kSolved;
    ASSERT_TRUE(parse_progress(to_string(value), &parsed)) << to_string(value);
    EXPECT_EQ(parsed, value);
  }
  Progress untouched = Progress::kAttempted;
  EXPECT_FALSE(parse_progress("passed", &untouched));
  EXPECT_EQ(untouched, Progress::kAttempted);
  EXPECT_FALSE(parse_progress("", &untouched));
  EXPECT_EQ(untouched, Progress::kAttempted);
  // 大小写不敏感（前端拼错大小写不该让筛选整页失效）
  Progress insensitive = Progress::kNone;
  ASSERT_TRUE(parse_progress(" SOLVED ", &insensitive));
  EXPECT_EQ(insensitive, Progress::kSolved);

  for (ProblemSort value : {ProblemSort::kDefault, ProblemSort::kFeatured, ProblemSort::kEasiest,
                            ProblemSort::kHardest}) {
    ProblemSort parsed = ProblemSort::kHardest;
    ASSERT_TRUE(parse_problem_sort(to_string(value), &parsed)) << to_string(value);
    EXPECT_EQ(parsed, value);
  }
  ProblemSort sort_untouched = ProblemSort::kFeatured;
  EXPECT_FALSE(parse_problem_sort("random", &sort_untouched));
  EXPECT_EQ(sort_untouched, ProblemSort::kFeatured);
  // 出参为 nullptr 时不应崩（调用点可能只想校验不取值）
  EXPECT_NO_THROW(parse_progress("solved", nullptr));
  EXPECT_NO_THROW(parse_problem_sort("easiest", nullptr));
}

TEST(ProblemApiEnum, DetailStatusMapsToTheErrorCodeTable) {
  // §6.3 的错误码表：404/500 的业务码在这里定死，中文文案一并断掉 ——
  // 文案漂了不会让任何代码崩，只会在某天让人看到一句错话。
  struct Row {
    DetailStatus status;
    int code;
    const char* message;
  };
  for (const Row& row : {Row{DetailStatus::kFound, 0, "ok"},
                         Row{DetailStatus::kNotFound, 1004, "题目不存在"},
                         Row{DetailStatus::kError, 1008, "服务器内部错误"}}) {
    EXPECT_EQ(error_code_of(row.status), row.code);
    EXPECT_STREQ(error_message_of(row.status), row.message);
  }
}

}  // namespace
