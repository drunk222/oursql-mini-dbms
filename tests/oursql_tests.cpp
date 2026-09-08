#include "oursql/ast/statement.h"
#include "oursql/catalog/catalog.h"
#include "oursql/common/status.h"
#include "oursql/common/value.h"
#include "oursql/parser/parser.h"

#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "[FAIL] " << message << '\n';
  }
  return condition;
}

bool TestValueComparison() {
  const oursql::Value lhs(42);
  const oursql::Value rhs(42);
  const oursql::Value text("42");
  return Check(lhs == rhs, "整数值应该相等") && Check(lhs != text, "整数和字符串不应该相等");
}

bool TestSchemaLookup() {
  const oursql::Schema schema{{"id", oursql::DataType::Int}, {"name", oursql::DataType::Varchar, 32}};
  auto found = schema.FindColumn("name");
  if (!Check(found.ok(), "应能找到 name 列")) {
    return false;
  }
  if (!Check(found.value() != nullptr, "name 列指针不能为空")) {
    return false;
  }
  if (!Check(found.value()->type == oursql::DataType::Varchar, "name 列类型应为 VARCHAR")) {
    return false;
  }

  auto missing = schema.FindColumn("age");
  return Check(!missing.ok(), "缺失列应返回错误") &&
         Check(missing.status().code() == oursql::ErrorCode::NotFound, "缺失列应返回 NotFound");
}

bool TestRidEquality() {
  const oursql::RID rid1{7, 3};
  const oursql::RID rid2{7, 3};
  const oursql::RID rid3{7, 4};
  return Check(rid1 == rid2, "相同 RID 应相等") && Check(rid1 != rid3, "不同 slot 的 RID 不应相等");
}

bool TestErrorReturns() {
  oursql::Catalog catalog;
  auto missing = catalog.FindTable("demo");
  if (!Check(!missing.ok(), "缺失表应返回错误")) {
    return false;
  }
  if (!Check(missing.status().code() == oursql::ErrorCode::NotFound, "缺失表应返回 NotFound")) {
    return false;
  }

  oursql::Parser parser;
  auto parsed = parser.Parse("SELECT 1;");
  return Check(!parsed.ok(), "非法 SELECT 应返回错误") &&
         Check(parsed.status().code() == oursql::ErrorCode::InvalidArgument,
               "语法错误应返回 InvalidArgument");
}

}  // namespace

int main() {
  int failures = 0;

  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) {
      std::cout << "[PASS] " << name << '\n';
      return;
    }
    ++failures;
  };

  run("Value comparison", &TestValueComparison);
  run("Schema lookup", &TestSchemaLookup);
  run("RID equality", &TestRidEquality);
  run("Error returns", &TestErrorReturns);

  if (failures == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }

  std::cerr << failures << " test(s) failed\n";
  return 1;
}
