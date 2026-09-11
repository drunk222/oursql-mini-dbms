#pragma once

#include "oursql/common/schema.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace oursql {

// 源码位置：offset 与 token 起始偏移一致，行列从 1 开始。
// 调用者：Lexer/Parser 与编译层诊断；作用：为 AST 节点记录出错位置。
struct Position {
  // offset 对应语句或 Token 的起始字节，line/column 供用户诊断和测试断言使用。
  std::size_t offset{0};
  std::size_t line{0};
  std::size_t column{0};
};

// 数据库层现有 Filter 只接受“列 = 常量”的等值谓词。Parser 在表达式恰好满足
// 该形态时额外生成 Predicate，使原有执行路径无需理解完整 CompileExpr。
struct Predicate {
  std::string column;
  Value value;

  Predicate() = default;

  // 调用者：解析器；作用：描述一个等值条件；返回：列名和值。
  Predicate(std::string column_name, Value literal) : column(std::move(column_name)), value(std::move(literal)) {}
};

// 编译层表达式树：用于解析、语义类型推导、常量折叠和诊断打印。
// 支持普通二元/一元表达式，以及 BETWEEN、IN 和 LIKE 谓词。
// 这些节点不会进入 Catalog、BufferPool 或 Executor；是否可执行由 Planner 决定。
enum class CompileLiteralKind { Int, String, Bool };

struct CompileExpr;
// 表达式以只读共享指针组成树。常量折叠会创建新节点，不修改已有节点，
// 因而 Parser 返回的 AST 可以被安全共享和重复检查。
using CompileExprPtr = std::shared_ptr<const CompileExpr>;

// 列引用只保存名称；列是否存在、属于哪种类型由 Planner 结合 Catalog 检查。
struct CompileColumnExpr {
  std::string name;
};

// 整数值放在 integer 中；字符串和布尔值放在 text 中，kind 决定解释方式。
struct CompileLiteralExpr {
  CompileLiteralKind kind{CompileLiteralKind::Int};
  std::int64_t integer{0};
  std::string text;
};

// op 保存解析后的运算符文本，例如 +、=、and；左右子树均可递归包含表达式。
struct CompileBinaryExpr {
  std::string op;
  CompileExprPtr left;
  CompileExprPtr right;
};

// 当前一元运算用于负号和 NOT。
struct CompileUnaryExpr {
  std::string op;
  CompileExprPtr operand;
};

// BETWEEN 比普通二元表达式多一个上界，因此单独保存 operand、lower 和 upper。
// negated 为 true 时表示 NOT BETWEEN。
struct CompileBetweenExpr {
  CompileExprPtr operand;
  CompileExprPtr lower;
  CompileExprPtr upper;
  bool negated{false};
};

// IN 的候选值数量不固定，使用 options 保存；negated 为 true 时表示 NOT IN。
struct CompileInExpr {
  CompileExprPtr operand;
  std::vector<CompileExprPtr> options;
  bool negated{false};
};

enum class CompileAggregateKind { Count, Sum, Avg, Max, Min };

// 聚合函数表达式。COUNT(*) 使用 star=true 且 argument 为空；其它聚合必须有
// argument。聚合函数只允许出现在 SELECT 投影和 HAVING 中。
struct CompileAggregateExpr {
  CompileAggregateKind kind{CompileAggregateKind::Count};
  bool star{false};
  CompileExprPtr argument;
};

// 使用 std::variant 表示固定表达式集合，配合 std::visit 强制所有访问点处理
// 每种节点类型，避免继承层次中遗漏 dynamic_cast 分支。
struct CompileExpr {
  using Data = std::variant<CompileColumnExpr, CompileLiteralExpr,
                            CompileBinaryExpr, CompileUnaryExpr,
                            CompileBetweenExpr, CompileInExpr,
                            CompileAggregateExpr>;
  explicit CompileExpr(Data expression_data)
      : data(std::move(expression_data)) {}
  Data data;
};

enum class OrderDirection { Asc, Desc };

// ORDER BY 的单个键。列名同样已在 Lexer 阶段规范化为小写。
struct OrderKey {
  std::string column;
  OrderDirection direction{OrderDirection::Asc};
};

// CREATE TABLE 只保存结构化 Schema，不在 Parser 中访问或修改 Catalog。
struct CreateTableStatement {
  std::string table_name;
  Schema schema;
  Position location;
};

// INSERT 支持可选的显式目标列和一组 VALUES 行。
// columns 为空表示按表定义顺序写入所有列；rows 至少包含一行，每行只保存
// 字面量，不支持 SELECT 子查询或表达式。
struct InsertStatement {
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  Position location;
};

// SELECT 的 where 与 compile_where 分别服务于简单执行路径和完整语义检查：
// - where：表达式可降级为“列 = 常量”时存在；
// - compile_where：所有 WHERE 都保留完整表达式，供类型推导和诊断使用。
// select_all 为 true 时 projection 应为空；二者由 Parser/Planner 保持一致。
// distinct 表示 SELECT DISTINCT；limit 存在时表示最多输出的行数。
// table_alias 保存 FROM ... AS alias；projection_aliases 与 projection 等长。
struct SelectStatement {
  std::string table_name;
  std::string table_alias;
  std::vector<std::string> projection;
  // 与 projection 等长，保存列引用或聚合函数等 SELECT 项表达式。
  std::vector<CompileExprPtr> projection_expressions;
  // 与 projection 等长；空字符串表示该列没有 AS 别名。
  std::vector<std::string> projection_aliases;
  bool select_all{false};
  std::optional<Predicate> where;
  CompileExprPtr compile_where;
  std::vector<std::string> group_by;
  CompileExprPtr having;
  std::vector<OrderKey> order_by;
  bool distinct{false};
  std::optional<std::size_t> limit;
  Position location;
};

// DROP TABLE 的编译层表示。Parser 只保存表名，Planner 负责检查表是否存在。
struct DropTableStatement {
  std::string table_name;
  Position location;
};

// CREATE [UNIQUE] INDEX ... ON ... (...).
struct CreateIndexStatement {
  std::string index_name;
  std::string table_name;
  std::string column_name;
  bool is_unique{false};
  Position location;
};

// DROP INDEX name。
struct DropIndexStatement {
  std::string index_name;
  Position location;
};

// EXPLAIN 当前只接受 SELECT，保存被解释查询的完整 AST。
struct ExplainStatement {
  SelectStatement select;
  Position location;
};

// DELETE 当前只支持可选等值 WHERE；复杂表达式会保留在 compile_where 中。
struct DeleteStatement {
  std::string table_name;
  std::optional<Predicate> where;
  CompileExprPtr compile_where;
  Position location;
};

// UPDATE 右值同时保留 CompileExpr 和可选的简单 Value：
// 复杂表达式用于语义分析，字面量形式可用于未来执行器直接消费。
struct UpdateAssignment {
  std::string column;
  CompileExprPtr expression;
  std::optional<Value> value;
};

// UPDATE 语句的完整编译层表示；执行器尚未实现 UpdatePlan。
struct UpdateStatement {
  std::string table_name;
  std::vector<UpdateAssignment> assignments;
  std::optional<Predicate> where;
  CompileExprPtr compile_where;
  Position location;
};

// 一条完整 SQL 语句的封闭集合。新增语句类型时，所有 std::visit 调用点都会
// 在编译期暴露出来，促使 Parser、Planner 和打印逻辑同步扩展。
using Statement = std::variant<CreateTableStatement, InsertStatement,
                                SelectStatement, DeleteStatement, DropTableStatement,
                                CreateIndexStatement, DropIndexStatement,
                                ExplainStatement, UpdateStatement>;

// 调用者：调试器或测试；作用：以稳定格式打印 AST；返回：AST 文本，不参与执行。
[[nodiscard]] std::string ToString(const Statement &statement);

}  // namespace oursql
