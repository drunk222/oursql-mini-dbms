#include "oursql/ast/statement.h"
#include "oursql/catalog/catalog.h"
#include "oursql/parser/lexer.h"
#include "oursql/parser/parser.h"
#include "oursql/plan/plan.h"
#include "oursql/planner/planner.h"

#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

static_assert(std::is_same_v<decltype(std::declval<oursql::SelectPlan>().root),
                             std::shared_ptr<const oursql::PlanNode>>);
static_assert(std::is_same_v<decltype(std::declval<oursql::FilterPlan>().child),
                             std::shared_ptr<const oursql::PlanNode>>);
static_assert(std::is_same_v<decltype(std::declval<oursql::ProjectPlan>().child),
                             std::shared_ptr<const oursql::PlanNode>>);

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

bool TestLexer() {
  oursql::Lexer lexer;
  auto result = lexer.Tokenize("SELECT id, name FROM t WHERE id = 1;");
  if (!Check(result.ok(), "Lexer 应成功处理 SELECT")) return false;
  const auto &tokens = result.value();
  const std::vector<oursql::TokenType> expected{
      oursql::TokenType::Select, oursql::TokenType::Identifier, oursql::TokenType::Comma,
      oursql::TokenType::Identifier, oursql::TokenType::From, oursql::TokenType::Identifier,
      oursql::TokenType::Where, oursql::TokenType::Identifier, oursql::TokenType::Equal,
      oursql::TokenType::Integer, oursql::TokenType::Semicolon, oursql::TokenType::EndOfFile};
  if (!Check(tokens.size() == expected.size(), "Token 数量应稳定")) return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (!Check(tokens[i].type == expected[i], "Token 类型应符合语法")) return false;
  }
  return Check(tokens[1].lexeme == "id" && tokens[1].line == 1 && tokens[1].column == 8,
               "标识符应规范化并记录起始位置");
}

bool TestLexerCommentsAndNumberErrors() {
  oursql::Lexer lexer;
  auto result = lexer.Tokenize(
      "SELECT id /* column comment */ FROM users -- line comment\n"
      "WHERE id = 1;");
  if (!Check(result.ok(), "注释不应阻止 Lexer 成功")) return false;
  bool has_select = false;
  bool has_where = false;
  for (const auto &token : result.value()) {
    if (token.type == oursql::TokenType::Select) has_select = true;
    if (token.type == oursql::TokenType::Where) has_where = true;
  }
  if (!Check(has_select && has_where, "注释内容不应生成 Token")) return false;

  auto bad_number = lexer.Tokenize("SELECT 1.2;");
  if (!Check(!bad_number.ok() &&
                 bad_number.status().message().find("非法数字") != std::string::npos,
             "非法数字应返回专门错误")) {
    return false;
  }
  auto unclosed = lexer.Tokenize("SELECT 1; /* no close");
  return Check(!unclosed.ok() &&
                   unclosed.status().message().find("未闭合块注释") != std::string::npos,
               "未闭合块注释应报错");
}

bool TestParserAndCaseInsensitiveMultiStatement() {
  oursql::Parser parser;
  auto result = parser.Parse(
      "CrEaTe TaBlE Users(ID INT, Name VARCHAR); INSERT INTO USERS VALUES(1, 'Alice'); "
      "SELECT ID, Name FROM users WHERE ID = 1 GROUP BY ID, Name ORDER BY ID DESC; "
      "DELETE FROM users WHERE id = 1;");
  if (!Check(result.ok() && result.value().size() == 4, "大小写不敏感多语句应解析成功")) return false;

  const auto &create = std::get<oursql::CreateTableStatement>(result.value()[0]);
  const auto &insert = std::get<oursql::InsertStatement>(result.value()[1]);
  const auto &select = std::get<oursql::SelectStatement>(result.value()[2]);
  const auto &delete_statement = std::get<oursql::DeleteStatement>(result.value()[3]);
  const bool fields = Check(create.table_name == "users" && create.schema.size() == 2,
                             "CREATE AST 核心字段应正确") &&
                      Check(create.schema.At(0).name == "id" && create.schema.At(0).type == oursql::DataType::Int,
                            "CREATE INT 列应正确") &&
                      Check(insert.table_name == "users" && insert.values.size() == 2 &&
                                insert.values[1].AsVarchar() == "Alice",
                            "INSERT AST 核心字段应正确") &&
                      Check(select.table_name == "users" && !select.select_all &&
                                select.projection == std::vector<std::string>{"id", "name"} &&
                                select.where.has_value() && select.where->value.AsInt() == 1,
                            "SELECT AST 核心字段应正确") &&
                      Check(select.group_by == std::vector<std::string>{"id", "name"} &&
                                select.order_by.size() == 1 &&
                                select.order_by[0].column == "id" &&
                                select.order_by[0].direction ==
                                    oursql::OrderDirection::Desc,
                            "GROUP BY/ORDER BY AST 字段应正确") &&
                      Check(select.location.line > 0 && select.location.column > 0,
                            "SELECT AST 应记录起始位置") &&
                      Check(delete_statement.table_name == "users" && delete_statement.where.has_value(),
                            "DELETE AST 核心字段应正确");
  if (!fields) return false;
  return Check(oursql::ToString(result.value()[2]) ==
                   "SelectStatement(table=users,projection=[id,name],where=id=1,"
                   "group_by=[id,name],order_by=[id DESC])",
               "AST 文本格式应稳定");
}

bool TestParserErrorsHavePositions() {
  oursql::Parser parser;
  const std::vector<std::string> invalid_sql{
      "CREATE TABLE t(id INT name VARCHAR);",
      "CREATE TABLE t(id INT;",
      "SELECT id FROM t",
      "UPSERT INTO t VALUES(1);",
  };
  for (const auto &sql : invalid_sql) {
    auto result = parser.Parse(sql);
    if (!Check(!result.ok(), "非法 SQL 应返回错误")) return false;
    if (!Check(result.status().message().find("位置") != std::string::npos,
               "Parser 错误应包含位置")) return false;
  }
  auto unterminated = parser.Parse("INSERT INTO t VALUES('Alice);");
  return Check(!unterminated.ok(), "未闭合字符串应返回错误") &&
         Check(unterminated.status().message().find("1:22") != std::string::npos,
               "未闭合字符串应包含起始位置");
}

oursql::Catalog MakeCatalog() {
  oursql::Catalog catalog;
  (void)catalog.CreateTable(oursql::TableInfo{
      "users", oursql::Schema{{"id", oursql::DataType::Int}, {"name", oursql::DataType::Varchar, 5}}});
  return catalog;
}

bool TestPlannerSemanticChecks() {
  oursql::Parser parser;
  auto table = parser.Parse("CREATE TABLE fresh(id INT, name VARCHAR);");
  if (!Check(table.ok(), "CREATE 应可解析")) return false;
  oursql::Catalog empty;
  oursql::Planner planner;
  auto create_plan = planner.Build(table.value()[0], empty);
  if (!Check(create_plan.ok() && std::holds_alternative<oursql::CreateTablePlan>(create_plan.value()),
             "CREATE 应生成 CreateTablePlan")) return false;

  auto catalog = MakeCatalog();
  const std::vector<std::pair<std::string, oursql::ErrorCode>> invalid{
      {"INSERT INTO missing VALUES(1, 'a');", oursql::ErrorCode::NotFound},
      {"SELECT unknown FROM users;", oursql::ErrorCode::NotFound},
      {"SELECT id FROM users ORDER BY missing;", oursql::ErrorCode::NotFound},
      {"SELECT id FROM users GROUP BY missing;", oursql::ErrorCode::NotFound},
      {"SELECT id, id FROM users;", oursql::ErrorCode::AlreadyExists},
      {"CREATE TABLE duplicate(id INT, id VARCHAR);", oursql::ErrorCode::AlreadyExists},
      {"INSERT INTO users VALUES(1);", oursql::ErrorCode::InvalidArgument},
      {"INSERT INTO users VALUES('1', 'Alice');", oursql::ErrorCode::TypeMismatch},
      {"SELECT id FROM users WHERE id = '1';", oursql::ErrorCode::TypeMismatch},
      {"INSERT INTO users VALUES(1, 'toolong');", oursql::ErrorCode::InvalidArgument},
  };
  for (const auto &item : invalid) {
    auto statements = parser.Parse(item.first);
    if (!Check(statements.ok(), "语义错误样例自身应先通过 Parser")) return false;
    auto plan = planner.Build(statements.value()[0], catalog);
    if (!Check(!plan.ok() && plan.status().code() == item.second, "语义错误类型应准确")) return false;
  }
  return true;
}

bool TestPlanTreeAndPrinting() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  auto statements = parser.Parse("SELECT id, name FROM users WHERE id = 1;");
  if (!Check(statements.ok(), "SELECT WHERE 应解析成功")) return false;
  oursql::Planner planner;
  auto plan_result = planner.Build(statements.value()[0], catalog);
  if (!Check(plan_result.ok(), "SELECT WHERE 应规划成功")) return false;
  const auto &select_plan = std::get<oursql::SelectPlan>(plan_result.value());
  if (!Check(select_plan.root != nullptr, "SELECT 计划应有根算子")) return false;
  const auto &project = std::get<oursql::ProjectPlan>(select_plan.root->operation);
  if (!Check(!project.select_all && project.columns == std::vector<std::string>{"id", "name"},
             "ProjectPlan 字段应正确")) return false;
  const auto &filter = std::get<oursql::FilterPlan>(project.child->operation);
  if (!Check(filter.predicate.column == "id" && filter.predicate.value.AsInt() == 1,
             "FilterPlan 条件应正确")) return false;
  const auto &scan = std::get<oursql::SeqScanPlan>(filter.child->operation);
  if (!Check(scan.table_name == "users", "SeqScanPlan 表名应正确")) return false;
  const auto plan_text = oursql::ToString(plan_result.value());
  if (!Check(plan_text.find("ProjectPlan") != std::string::npos &&
                 plan_text.find("FilterPlan") != std::string::npos &&
                 plan_text.find("SeqScanPlan") != std::string::npos,
             "Plan 文本应包含算子链")) {
    return false;
  }

  auto insert_statements = parser.Parse("INSERT INTO users VALUES(1, 'Alice');");
  auto insert_plan = planner.Build(insert_statements.value()[0], catalog);
  if (!Check(insert_plan.ok() && std::get<oursql::InsertPlan>(insert_plan.value()).values.size() == 2,
             "INSERT 应生成正确的 InsertPlan")) {
    return false;
  }
  auto delete_statements = parser.Parse("DELETE FROM users WHERE id = 1;");
  auto delete_plan = planner.Build(delete_statements.value()[0], catalog);
  if (!Check(delete_plan.ok() && std::get<oursql::DeletePlan>(delete_plan.value()).where.has_value(),
             "DELETE 应生成正确的 DeletePlan")) {
    return false;
  }
  auto select_all_statements = parser.Parse("SELECT * FROM users;");
  auto select_all_plan = planner.Build(select_all_statements.value()[0], catalog);
  if (!Check(select_all_plan.ok(), "SELECT * 应规划成功")) return false;
  const auto &select_all = std::get<oursql::SelectPlan>(select_all_plan.value());
  const auto &project_all = std::get<oursql::ProjectPlan>(select_all.root->operation);
  return Check(project_all.select_all && project_all.columns.empty(), "SELECT * 的 ProjectPlan 应保留全列标记");
}

bool TestUpdateCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  auto parsed = parser.Parse(
      "UPDATE users SET name = 'Bob', id = 2 WHERE id = 1;");
  if (!Check(parsed.ok(), "UPDATE 应可解析")) return false;
  const auto &update =
      std::get<oursql::UpdateStatement>(parsed.value()[0]);
  if (!Check(update.table_name == "users" &&
                 update.assignments.size() == 2 &&
                 update.assignments[0].column == "name" &&
                 update.assignments[1].value.has_value() &&
                 update.assignments[1].value->AsInt() == 2 &&
                 update.where.has_value(),
             "UPDATE AST 核心字段应正确")) {
    return false;
  }
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok() &&
                 std::holds_alternative<oursql::UpdatePlan>(plan.value()),
             "UPDATE 应生成 UpdatePlan")) {
    return false;
  }

  auto missing_table = parser.Parse("UPDATE ghost SET id = 1;");
  auto bad_table = planner.Build(missing_table.value()[0], catalog);
  auto missing_column =
      parser.Parse("UPDATE users SET missing = 1;");
  auto bad_column = planner.Build(missing_column.value()[0], catalog);
  auto wrong_type =
      parser.Parse("UPDATE users SET id = 'text';");
  auto type_status = planner.Build(wrong_type.value()[0], catalog);
  auto expression_update =
      parser.Parse("UPDATE users SET id = id + 1;");
  if (!Check(expression_update.ok(), "UPDATE SET 表达式应可解析")) {
    return false;
  }
  const auto &expression_assignment =
      std::get<oursql::UpdateStatement>(expression_update.value()[0])
          .assignments[0];
  if (!Check(expression_assignment.expression != nullptr &&
                 !expression_assignment.value.has_value(),
             "UPDATE SET 表达式 AST 应保留 CompileExpr")) {
    return false;
  }
  auto expression_status =
      planner.Build(expression_update.value()[0], catalog);
  return Check(!bad_table.ok() &&
                   bad_table.status().code() == oursql::ErrorCode::NotFound &&
                   !bad_column.ok() &&
                   bad_column.status().code() == oursql::ErrorCode::NotFound &&
                   !type_status.ok() &&
                   type_status.status().code() == oursql::ErrorCode::TypeMismatch &&
                   expression_status.ok() &&
                   std::holds_alternative<oursql::UpdatePlan>(
                       expression_status.value()),
               "UPDATE 语义错误应被 Planner 拒绝");
}

bool TestOrderGroupCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  const std::vector<std::string> accepted{
      "SELECT id FROM users ORDER BY id DESC;",
      "SELECT id FROM users GROUP BY id;",
      "SELECT id FROM users GROUP BY id ORDER BY id;",
  };
  for (const auto &sql : accepted) {
    auto parsed = parser.Parse(sql);
    if (!Check(parsed.ok(), "GROUP BY/ORDER BY 应可解析: " + sql)) return false;
    auto plan = planner.Build(parsed.value()[0], catalog);
    if (!Check(plan.ok() &&
                   std::holds_alternative<oursql::SelectPlan>(plan.value()),
               "GROUP BY/ORDER BY 应生成 SelectPlan: " + sql)) {
      return false;
    }
    const auto &select = std::get<oursql::SelectPlan>(plan.value());
    const auto *project =
        std::get_if<oursql::ProjectPlan>(&select.root->operation);
    if (!Check(project != nullptr, "SELECT 根节点应为 ProjectPlan")) {
      return false;
    }
    if (sql.find("ORDER BY") != std::string::npos) {
      if (!Check(std::holds_alternative<oursql::OrderByPlan>(
                     project->child->operation),
                 "ORDER BY 应生成 OrderByPlan")) {
        return false;
      }
    } else if (sql.find("GROUP BY") != std::string::npos) {
      if (!Check(std::holds_alternative<oursql::GroupByPlan>(
                     project->child->operation),
                 "GROUP BY 应生成 GroupByPlan")) {
        return false;
      }
    }
  }
  // 列存在性仍然先于"未实现"被检查，保证诊断精确。
  auto missing = parser.Parse("SELECT id FROM users ORDER BY missing;");
  auto missing_plan = planner.Build(missing.value()[0], catalog);
  return Check(!missing_plan.ok() &&
                   missing_plan.status().code() == oursql::ErrorCode::NotFound,
               "ORDER BY 列不存在时应先报 NotFound");
}

bool TestCompileExpressionParsing() {
  oursql::Parser parser;
  auto parsed = parser.Parse(
      "SELECT * FROM users WHERE age > 1 AND (name = 'Alice' OR id >= 2);");
  if (!Check(parsed.ok(), "复杂 WHERE 表达式应可解析")) return false;
  const auto &select = std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.compile_where != nullptr && !select.where.has_value(),
             "复杂表达式不应伪装成数据库等值谓词")) {
    return false;
  }
  const std::string text = oursql::ToString(parsed.value()[0]);
  if (!Check(text.find("where_expr=") != std::string::npos &&
                 text.find(">") != std::string::npos &&
                 text.find("and") != std::string::npos,
             "复杂表达式应进入 AST 文本")) {
    return false;
  }

  auto catalog = MakeCatalog();
  oursql::Planner planner;
  auto missing = parser.Parse(
      "SELECT * FROM users WHERE missing > 1;");
  auto missing_status = planner.Build(missing.value()[0], catalog);
  auto wrong_type = parser.Parse(
      "SELECT * FROM users WHERE id + 'text' > 1;");
  auto type_status = planner.Build(wrong_type.value()[0], catalog);
  return Check(!missing_status.ok() &&
                   missing_status.status().code() ==
                       oursql::ErrorCode::NotFound &&
                   !type_status.ok() &&
                   type_status.status().code() ==
                       oursql::ErrorCode::TypeMismatch,
               "复杂表达式也应执行列和类型语义检查");
}

bool TestCompileConstantFolding() {
  oursql::Parser parser;
  auto parsed = parser.Parse(
      "SELECT * FROM users WHERE id = 10 + 8;");
  if (!Check(parsed.ok(), "常量条件应可解析")) return false;
  const auto &select = std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.where.has_value() &&
                 select.where->value.AsInt() == 18,
             "id = 10 + 8 应折叠为 Predicate(id,18)")) {
    return false;
  }

  auto true_condition = parser.Parse(
      "SELECT * FROM users WHERE 1 = 1;");
  if (!Check(true_condition.ok(), "1 = 1 应可解析")) return false;
  const auto &constant_select =
      std::get<oursql::SelectStatement>(true_condition.value()[0]);
  const auto *literal =
      constant_select.compile_where
          ? std::get_if<oursql::CompileLiteralExpr>(
                &constant_select.compile_where->data)
          : nullptr;
  if (!Check(literal != nullptr &&
                 literal->kind == oursql::CompileLiteralKind::Bool &&
                 literal->text == "true",
             "1 = 1 应折叠为 TRUE 字面量")) {
    return false;
  }

  auto catalog = MakeCatalog();
  oursql::Planner planner;
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok(), "折叠后的等值条件应可规划")) return false;
  const auto &select_plan = std::get<oursql::SelectPlan>(plan.value());
  const auto *project =
      std::get_if<oursql::ProjectPlan>(&select_plan.root->operation);
  const auto *filter =
      project == nullptr || project->child == nullptr
          ? nullptr
          : std::get_if<oursql::FilterPlan>(&project->child->operation);
  return Check(filter != nullptr &&
                   filter->predicate.value.AsInt() == 18,
               "折叠后的 Predicate 应进入 FilterPlan");
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) std::cout << "[PASS] " << name << '\n';
    else ++failures;
  };
  run("Lexer tokens", &TestLexer);
  run("Lexer comments and number errors", &TestLexerCommentsAndNumberErrors);
  run("Parser AST and multi-statement", &TestParserAndCaseInsensitiveMultiStatement);
  run("Parser errors and positions", &TestParserErrorsHavePositions);
  run("Planner semantic checks", &TestPlannerSemanticChecks);
  run("Plan tree and printing", &TestPlanTreeAndPrinting);
  run("UPDATE compile-only", &TestUpdateCompileOnly);
  run("ORDER BY/GROUP BY compile-only", &TestOrderGroupCompileOnly);
  run("Compile expression parsing", &TestCompileExpressionParsing);
  run("Compile constant folding", &TestCompileConstantFolding);
  if (failures == 0) {
    std::cout << "All frontend tests passed\n";
    return 0;
  }
  std::cerr << failures << " frontend test(s) failed\n";
  return 1;
}
