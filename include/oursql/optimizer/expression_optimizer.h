#pragma once

#include "oursql/ast/statement.h"

#include <optional>

// 表达式级优化与 Predicate 规范化接口。
//
// 这些函数只处理 CompileExpr，不访问 Catalog、BufferPool 或 Executor。
// FoldCompileExpr 返回新的只读表达式树；ExtractPredicateFromCompileExpr 把
// 可安全执行的简单条件转换成数据库层 Predicate。
namespace oursql {

// 编译层表达式常量折叠：
//   10 + 8 -> 18
//   1 = 1 -> TRUE
// 该函数只处理编译层 CompileExpr，不访问 Catalog 或执行器。
[[nodiscard]] CompileExprPtr FoldCompileExpr(const CompileExprPtr &expr);

// 将折叠后的简单等值表达式转换为数据库层 Predicate。
// 例如 id = 10 + 8 折叠成 id = 18 后，可继续走原有等值 WHERE 路径。
[[nodiscard]] std::optional<Predicate> ExtractPredicateFromCompileExpr(
    const CompileExprPtr &expr);

// 提取整数/字符串常量。布尔常量不转换为数据库 Value。
[[nodiscard]] std::optional<Value> ExtractLiteralValue(
    const CompileExprPtr &expr);

}  // namespace oursql
