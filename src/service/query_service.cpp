#include "oursql/service/query_service.h"

#include "oursql/parser/parser.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <string>
#include <thread>
#include <type_traits>

namespace oursql {
namespace {

constexpr std::size_t kMaxSqlBytes = 256U * 1024U;
constexpr std::size_t kMinConcurrentSessions = 2;
constexpr std::size_t kMaxConcurrentSessions = 6;

class StartGate {
 public:
  explicit StartGate(std::size_t participants) : remaining_(participants) {}

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (--remaining_ == 0) {
      condition_.notify_all();
      return;
    }
    condition_.wait(lock, [this] { return remaining_ == 0; });
  }

 private:
  std::size_t remaining_;
  std::mutex mutex_;
  std::condition_variable condition_;
};

enum class TransactionStatement { None, Begin, Finish };

TransactionStatement GetTransactionStatement(const Statement &statement) {
  return std::visit(
      [](const auto &value) {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, BeginStatement>) {
          return TransactionStatement::Begin;
        } else if constexpr (std::is_same_v<Type, CommitStatement> ||
                             std::is_same_v<Type, RollbackStatement>) {
          return TransactionStatement::Finish;
        } else {
          return TransactionStatement::None;
        }
      },
      statement);
}

Status ValidateTransactionBatch(const std::vector<Statement> &statements) {
  bool active = false;
  for (const auto &statement : statements) {
    switch (GetTransactionStatement(statement)) {
      case TransactionStatement::None:
        break;
      case TransactionStatement::Begin:
        if (active) {
          return Status::InvalidArgument(
              "a Web request cannot start nested transactions");
        }
        active = true;
        break;
      case TransactionStatement::Finish:
        if (!active) {
          return Status::NotFound(
              "COMMIT or ROLLBACK has no matching BEGIN in this request");
        }
        active = false;
        break;
    }
  }
  if (active) {
    return Status::NotImplemented(
        "cross-request transactions are not supported by the minimal Web demo");
  }
  return Status::Ok();
}

std::size_t GetStatementLine(const Statement &statement) {
  return std::visit(
      [](const auto &value) { return value.location.line; },
      statement);
}

SqlTraceStep MakeFailedStep(std::string name, std::string message) {
  SqlTraceStep step;
  step.name = std::move(name);
  step.summary = std::move(message);
  return step;
}

std::string TokenTraceEntry(const Token &token) {
  std::string entry = std::to_string(token.line) + ":" + std::to_string(token.column) +
                      "  " + TokenTypeName(token.type);
  if (!token.lexeme.empty()) entry += "  " + token.lexeme;
  return entry;
}

}  // namespace

QueryService::QueryService(DatabaseEngine *engine) noexcept : engine_(engine) {}

Status QueryService::CheckReady() const {
  if (engine_ == nullptr) {
    return Status::InvalidArgument("DatabaseEngine is not available");
  }
  if (!engine_->GetInitStatus().ok()) {
    return engine_->GetInitStatus();
  }
  return Status::Ok();
}

Result<std::vector<ExecutionResult>> QueryService::ExecuteSql(std::string_view sql) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) {
    return Result<std::vector<ExecutionResult>>(std::move(ready));
  }
  if (sql.empty()) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("SQL must not be empty"));
  }
  if (sql.size() > kMaxSqlBytes) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("SQL exceeds the maximum supported length"));
  }

  Parser parser;
  auto statements = parser.Parse(sql);
  if (!statements.ok()) {
    return Result<std::vector<ExecutionResult>>(statements.status());
  }
  const auto transaction_status = ValidateTransactionBatch(statements.value());
  if (!transaction_status.ok()) {
    return Result<std::vector<ExecutionResult>>(transaction_status);
  }
  auto results = engine_->ExecuteSqlBatch(sql);
  if (!results.ok()) return results;
  if (results.value().size() != statements.value().size()) {
    return Result<std::vector<ExecutionResult>>(Status::InternalError(
        "SQL execution result count does not match parsed statement count"));
  }
  for (std::size_t i = 0; i < results.value().size(); ++i) {
    results.value()[i].source_line = GetStatementLine(statements.value()[i]);
  }
  return results;
}

Result<SqlTrace> QueryService::TraceSql(std::string_view sql) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<SqlTrace>(std::move(ready));
  if (sql.empty()) {
    return Result<SqlTrace>(Status::InvalidArgument("SQL must not be empty"));
  }
  if (sql.size() > kMaxSqlBytes) {
    return Result<SqlTrace>(
        Status::InvalidArgument("SQL exceeds the maximum supported length"));
  }

  SqlTrace trace;
  Lexer lexer;
  auto tokens = lexer.Tokenize(sql);
  SqlTraceStep token_step;
  token_step.name = "Token";
  token_step.ok = tokens.ok();
  if (tokens.ok()) {
    token_step.summary = std::to_string(tokens.value().size()) + " tokens";
    token_step.entries.reserve(tokens.value().size());
    for (const auto &token : tokens.value()) token_step.entries.push_back(TokenTraceEntry(token));
  } else {
    token_step.summary = tokens.status().ToString();
  }
  trace.steps.push_back(std::move(token_step));

  if (!tokens.ok()) {
    trace.steps.push_back(MakeFailedStep("AST", "词法分析失败，未进入语法分析"));
    trace.steps.push_back(MakeFailedStep("语义检查", "语法树未生成"));
    trace.steps.push_back(MakeFailedStep("Plan", "语义检查未完成"));
    return Result<SqlTrace>(std::move(trace));
  }

  Parser parser;
  auto statements = parser.Parse(sql);
  SqlTraceStep ast_step;
  ast_step.name = "AST";
  ast_step.ok = statements.ok();
  if (statements.ok()) {
    ast_step.summary = std::to_string(statements.value().size()) + " statements";
    ast_step.entries.reserve(statements.value().size());
    for (const auto &statement : statements.value()) {
      ast_step.entries.push_back("第 " + std::to_string(GetStatementLine(statement)) +
                                 " 行: " + ToString(statement));
    }
  } else {
    ast_step.summary = statements.status().ToString();
  }
  trace.steps.push_back(std::move(ast_step));

  if (!statements.ok()) {
    trace.steps.push_back(MakeFailedStep("语义检查", "语法树未生成"));
    trace.steps.push_back(MakeFailedStep("Plan", "语义检查未完成"));
    return Result<SqlTrace>(std::move(trace));
  }

  SqlTraceStep semantic_step;
  semantic_step.name = "语义检查";
  semantic_step.ok = true;
  SqlTraceStep plan_step;
  plan_step.name = "Plan";
  plan_step.ok = true;
  std::size_t semantic_failures = 0;
  std::size_t plan_failures = 0;

  for (const auto &statement : statements.value()) {
    const std::size_t line = GetStatementLine(statement);
    auto plan = engine_->BuildPlan(statement);
    if (plan.ok()) {
      semantic_step.entries.push_back("第 " + std::to_string(line) + " 行: 通过");
      plan_step.entries.push_back("第 " + std::to_string(line) + " 行: " +
                                  ToString(plan.value()));
      continue;
    }

    if (plan.status().code() == ErrorCode::NotImplemented) {
      semantic_step.entries.push_back(
          "第 " + std::to_string(line) + " 行: 通过；" + plan.status().ToString());
    } else {
      semantic_step.ok = false;
      ++semantic_failures;
      semantic_step.entries.push_back(
          "第 " + std::to_string(line) + " 行: 失败；" + plan.status().ToString());
    }
    plan_step.ok = false;
    ++plan_failures;
    plan_step.entries.push_back("第 " + std::to_string(line) + " 行: 未生成");
  }

  semantic_step.summary = semantic_failures == 0
                              ? "语义检查通过"
                              : std::to_string(semantic_failures) + " 条语句语义检查失败";
  plan_step.summary = plan_failures == 0
                          ? "已生成 " + std::to_string(plan_step.entries.size()) + " 个计划"
                          : std::to_string(plan_failures) + " 条语句未生成计划";
  trace.steps.push_back(std::move(semantic_step));
  trace.steps.push_back(std::move(plan_step));
  return Result<SqlTrace>(std::move(trace));
}

Result<std::vector<ConcurrentSqlResult>> QueryService::ExecuteConcurrentSql(
    const std::vector<std::string> &sql_statements) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) {
    return Result<std::vector<ConcurrentSqlResult>>(std::move(ready));
  }
  if (sql_statements.size() < kMinConcurrentSessions ||
      sql_statements.size() > kMaxConcurrentSessions) {
    return Result<std::vector<ConcurrentSqlResult>>(Status::InvalidArgument(
        "concurrent SQL requires between 2 and 6 sessions"));
  }

  std::vector<ConcurrentSqlResult> outcomes(sql_statements.size());
  std::vector<std::shared_ptr<SessionContext>> sessions;
  sessions.reserve(sql_statements.size());
  for (std::size_t i = 0; i < sql_statements.size(); ++i) {
    auto session = engine_->CreateSession();
    if (!session.ok()) {
      for (const auto &created : sessions) {
        (void)engine_->CloseSession(created);
      }
      return Result<std::vector<ConcurrentSqlResult>>(session.status());
    }
    sessions.push_back(session.value());
  }

  StartGate start_gate(sql_statements.size());
  std::vector<std::thread> workers;
  workers.reserve(sql_statements.size());
  for (std::size_t i = 0; i < sql_statements.size(); ++i) {
    workers.emplace_back([&, i] {
      start_gate.Wait();
      const auto started_at = std::chrono::steady_clock::now();
      auto execution = [&]() {
        if (sql_statements[i].empty()) {
          return Result<std::vector<ExecutionResult>>(
              Status::InvalidArgument("SQL must not be empty"));
        }
        if (sql_statements[i].size() > kMaxSqlBytes) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "SQL exceeds the maximum supported length"));
        }
        return engine_->ExecuteSqlBatch(sessions[i], sql_statements[i]);
      }();
      outcomes[i].duration_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started_at)
              .count();
      if (execution.ok()) {
        outcomes[i].results = std::move(execution.value());
      } else {
        outcomes[i].status = execution.status();
      }
      (void)engine_->CloseSession(sessions[i]);
    });
  }
  for (auto &worker : workers) worker.join();
  return Result<std::vector<ConcurrentSqlResult>>(std::move(outcomes));
}

Result<std::vector<TableMetadata>> QueryService::ListTables() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<std::vector<TableMetadata>>(std::move(ready));
  return Result<std::vector<TableMetadata>>(engine_->ListTables());
}

Result<TableMetadata> QueryService::GetTableMetadata(std::string_view table_name) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<TableMetadata>(std::move(ready));
  return engine_->GetTableMetadata(table_name);
}

Result<DatabaseStatistics> QueryService::GetStatistics() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<DatabaseStatistics>(std::move(ready));
  return Result<DatabaseStatistics>(engine_->GetStatistics());
}

Result<std::vector<FrameSnapshot>> QueryService::GetBufferSnapshots() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<std::vector<FrameSnapshot>>(std::move(ready));
  return Result<std::vector<FrameSnapshot>>(engine_->GetBufferSnapshots());
}

Result<std::vector<page_id_t>> QueryService::GetEvictionLog() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<std::vector<page_id_t>>(std::move(ready));
  return Result<std::vector<page_id_t>>(engine_->GetEvictionLog());
}

Status QueryService::Flush() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return ready;
  return engine_->Flush();
}

}  // namespace oursql
