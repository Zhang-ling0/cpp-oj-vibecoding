// harness 生成：ProblemBundle → solution_decl.h / tester_util.h / tester.cpp
#pragma once

#include <string>

#include "model/problem.h"

namespace oj {
namespace judge {

struct GenerateResult {
  bool ok = false;
  std::string error;  // 非空 = 题库配置有误 → 调用方判 JE（§4.2.1）
  std::string solution_decl;
  std::string tester_util;
  std::string tester_cpp;
};

// 纯函数：不碰文件系统。失败时 ok=false 且不产出任何文件内容。
GenerateResult generate_harness(const model::ProblemBundle& problem);

// JSON 值 → C++ 字面量。失败（深度不符 / 无法解析）返回空串且 reason 非空。
std::string json_to_cpp_literal(const nlohmann::json& value, const std::string& cpp_type,
                                std::string* reason);

}  // namespace judge
}  // namespace oj
