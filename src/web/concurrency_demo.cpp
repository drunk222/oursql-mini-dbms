#include "oursql/web/concurrency_demo.h"

#include "oursql/execution/database_engine.h"
#include "oursql/transaction/lock_manager.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace oursql {
namespace {

using Clock = std::chrono::steady_clock;

struct TempDatabase {
  std::filesystem::path path;

  explicit TempDatabase(const std::string &name) {
    const auto stamp = Clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_web_concurrency_" + name + "_" + std::to_string(stamp) +
            ".oursql");
  }

  ~TempDatabase() {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".wal", error);
  }
};

class StartGate {
 public:
  explicit StartGate(int participants) : remaining_(participants) {}

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (--remaining_ == 0) {
      condition_.notify_all();
      return;
    }
    condition_.wait(lock, [this] { return remaining_ == 0; });
  }

 private:
  int remaining_{0};
  std::mutex mutex_;
  std::condition_variable condition_;
};

class EventLog {
 public:
  EventLog() : started_at_(Clock::now()) {}

  void Add(std::string session, std::string action, std::string detail,
           std::string status = "info") {
    const auto now = Clock::now();
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(now - started_at_)
            .count();
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back({{"offset_ms", micros / 1000.0},
                       {"session", std::move(session)},
                       {"action", std::move(action)},
                       {"detail", std::move(detail)},
                       {"status", std::move(status)}});
  }

  nlohmann::json ToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

 private:
  Clock::time_point started_at_;
  mutable std::mutex mutex_;
  nlohmann::json events_ = nlohmann::json::array();
};

using Metrics = std::vector<std::pair<std::string, std::string>>;
using ScenarioBody =
    std::function<bool(EventLog &, Metrics &)>;

nlohmann::json MetricsToJson(const Metrics &metrics) {
  auto result = nlohmann::json::array();
  for (const auto &[label, value] : metrics) {
    result.push_back({{"label", label}, {"value", value}});
  }
  return result;
}

nlohmann::json RunScenario(std::string id, std::string title,
                           std::string purpose, const ScenarioBody &body) {
  EventLog events;
  Metrics metrics;
  std::string error;
  bool passed = false;
  const auto started = Clock::now();
  try {
    passed = body(events, metrics);
  } catch (const std::exception &exception) {
    error = exception.what();
  }
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                           started)
          .count();
  nlohmann::json result{
      {"id", std::move(id)},
      {"title", std::move(title)},
      {"purpose", std::move(purpose)},
      {"passed", passed},
      {"duration_ms", elapsed_ms},
      {"metrics", MetricsToJson(metrics)},
      {"timeline", events.ToJson()},
  };
  if (!error.empty()) result["error"] = std::move(error);
  return result;
}

std::string StatusDetail(const Status &status) {
  return status.ok() ? "success" : status.ToString();
}

bool SetupAccounts(DatabaseEngine *engine, bool two_rows) {
  const std::string padding(2800, 'x');
  std::string sql =
      "CREATE TABLE accounts(id INT, status VARCHAR(32), padding VARCHAR(3000));"
      "INSERT INTO accounts VALUES(1, 'initial-1', '" +
      padding + "');";
  if (two_rows) {
    sql += "INSERT INTO accounts VALUES(2, 'initial-2', '" + padding + "');";
  }
  sql += "CREATE UNIQUE INDEX idx_accounts_id ON accounts(id);";
  return engine->ExecuteSql(sql).ok();
}

bool HasAccountValue(const ExecutionResult &result, std::int64_t id,
                     const std::string &value) {
  for (const auto &row : result.rows) {
    if (row.size() >= 2 && row[0].IsInt() && row[0].AsInt() == id &&
        row[1].IsVarchar() && row[1].AsVarchar() == value) {
      return true;
    }
  }
  return false;
}

bool RunParallelCommit(EventLog &events, Metrics &metrics) {
  TempDatabase temp("parallel_commit");
  DatabaseEngine engine(temp.path, 32);
  if (!engine.GetInitStatus().ok()) {
    events.Add("server", "初始化", engine.GetInitStatus().ToString(), "error");
    return false;
  }
  if (!engine.ExecuteSql(
           "CREATE TABLE first_account(id INT, status VARCHAR(32));"
           "CREATE TABLE second_account(id INT, status VARCHAR(32));")
           .ok()) {
    events.Add("server", "准备数据", "创建并发演示表失败", "error");
    return false;
  }

  auto first = engine.CreateSession();
  auto second = engine.CreateSession();
  if (!first.ok() || !second.ok() || !engine.ExecuteSql(first.value(), "BEGIN;").ok() ||
      !engine.ExecuteSql(second.value(), "BEGIN;").ok()) {
    events.Add("server", "创建会话", "两个会话启动失败", "error");
    return false;
  }
  events.Add("S1", "BEGIN", "事务 1 启动", "success");
  events.Add("S2", "BEGIN", "事务 2 启动", "success");

  std::array<Status, 2> update_status{
      Status::InternalError("update not finished"),
      Status::InternalError("update not finished")};
  StartGate update_gate(2);
  std::thread first_update([&] {
    update_gate.Wait();
    auto result = engine.ExecuteSql(
        first.value(), "INSERT INTO first_account VALUES(1, 'session-A');");
    update_status[0] = result.ok() ? Status::Ok() : result.status();
    events.Add("S1", "INSERT", "写入 first_account", result.ok() ? "success" : "error");
  });
  std::thread second_update([&] {
    update_gate.Wait();
    auto result = engine.ExecuteSql(
        second.value(), "INSERT INTO second_account VALUES(1, 'session-B');");
    update_status[1] = result.ok() ? Status::Ok() : result.status();
    events.Add("S2", "INSERT", "写入 second_account",
               result.ok() ? "success" : "error");
  });
  first_update.join();
  second_update.join();

  bool passed = update_status[0].ok() && update_status[1].ok();
  std::array<Status, 2> commit_status{
      Status::InternalError("commit not finished"),
      Status::InternalError("commit not finished")};
  if (passed) {
    StartGate commit_gate(2);
    std::thread first_commit([&] {
      commit_gate.Wait();
      auto result = engine.ExecuteSql(first.value(), "COMMIT;");
      commit_status[0] = result.ok() ? Status::Ok() : result.status();
      events.Add("S1", "COMMIT", "事务 1 提交", result.ok() ? "success" : "error");
    });
    std::thread second_commit([&] {
      commit_gate.Wait();
      auto result = engine.ExecuteSql(second.value(), "COMMIT;");
      commit_status[1] = result.ok() ? Status::Ok() : result.status();
      events.Add("S2", "COMMIT", "事务 2 提交", result.ok() ? "success" : "error");
    });
    first_commit.join();
    second_commit.join();
    passed = commit_status[0].ok() && commit_status[1].ok();
  }

  auto first_rows =
      engine.ExecuteSql(first.value(), "SELECT id, status FROM first_account;");
  auto second_rows =
      engine.ExecuteSql(second.value(), "SELECT id, status FROM second_account;");
  const bool final_ok =
      first_rows.ok() && second_rows.ok() &&
      HasAccountValue(first_rows.value(), 1, "session-A") &&
      HasAccountValue(second_rows.value(), 1, "session-B");
  events.Add("server", "SELECT", "读取两个会话的提交结果",
             final_ok ? "success" : "error");
  metrics.push_back({"并发 INSERT", update_status[0].ok() && update_status[1].ok()
                                         ? "2 成功"
                                         : "存在失败"});
  metrics.push_back({"并发 COMMIT", commit_status[0].ok() && commit_status[1].ok()
                                         ? "2 成功"
                                         : "存在失败"});
  metrics.push_back({"最终可见修改", final_ok ? "2 行" : "不一致"});

  (void)engine.CloseSession(first.value());
  (void)engine.CloseSession(second.value());
  passed = passed && final_ok && engine.Close().ok();
  return passed;
}

bool RunSameKeyWait(EventLog &events, Metrics &metrics) {
  TempDatabase temp("same_key_wait");
  DatabaseEngine engine(temp.path, 32);
  if (!engine.GetInitStatus().ok() || !SetupAccounts(&engine, false)) {
    events.Add("server", "准备数据", "初始化失败", "error");
    return false;
  }

  auto first = engine.CreateSession();
  auto second = engine.CreateSession();
  if (!first.ok() || !second.ok() || !engine.ExecuteSql(first.value(), "BEGIN;").ok() ||
      !engine.ExecuteSql(second.value(), "BEGIN;").ok()) {
    events.Add("server", "创建会话", "会话启动失败", "error");
    return false;
  }
  events.Add("S1", "BEGIN", "持有事务启动", "success");
  events.Add("S2", "BEGIN", "等待事务启动", "success");

  auto holding = engine.ExecuteSql(
      first.value(), "UPDATE accounts SET status = 'holder' WHERE id = 1;");
  events.Add("S1", "UPDATE", "获得 id=1 的写锁",
             holding.ok() ? "success" : "error");
  if (!holding.ok()) return false;

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool waiter_started = false;
  bool waiter_finished = false;
  Status waiter_status = Status::InternalError("waiter not finished");
  const auto waiter_started_at = Clock::now();
  std::thread waiter([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      waiter_started = true;
      state_changed.notify_all();
    }
    auto result = engine.ExecuteSql(
        second.value(), "UPDATE accounts SET status = 'waiter' WHERE id = 1;");
    waiter_status = result.ok() ? Status::Ok() : result.status();
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      waiter_finished = true;
      state_changed.notify_all();
    }
  });

  bool observed_wait = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    state_changed.wait(lock, [&] { return waiter_started; });
    observed_wait = !state_changed.wait_for(
        lock, std::chrono::milliseconds(75), [&] { return waiter_finished; });
  }
  events.Add("S2", "LOCK WAIT", "等待 id=1 的写锁", observed_wait ? "wait" : "error");

  auto commit_first = engine.ExecuteSql(first.value(), "COMMIT;");
  events.Add("S1", "COMMIT", "释放 id=1 写锁",
             commit_first.ok() ? "success" : "error");

  bool waiter_completed = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    waiter_completed = state_changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return waiter_finished; });
  }
  waiter.join();
  const auto waited_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                           waiter_started_at)
          .count();

  Status commit_second = Status::InternalError("second commit not attempted");
  if (waiter_completed && waiter_status.ok()) {
    auto result = engine.ExecuteSql(second.value(), "COMMIT;");
    commit_second = result.ok() ? Status::Ok() : result.status();
    events.Add("S2", "COMMIT", "等待事务完成提交",
               commit_second.ok() ? "success" : "error");
  }

  auto final_rows =
      engine.ExecuteSql(first.value(), "SELECT id, status FROM accounts;");
  const bool final_ok =
      final_rows.ok() && HasAccountValue(final_rows.value(), 1, "waiter");
  metrics.push_back({"等待事务", observed_wait ? "已阻塞" : "未阻塞"});
  metrics.push_back({"等待时间", std::to_string(waited_ms) + " ms"});
  metrics.push_back({"最终状态", final_ok ? "waiter" : "不一致"});

  (void)engine.CloseSession(first.value());
  (void)engine.CloseSession(second.value());
  (void)engine.Close();
  return observed_wait && waiter_completed && waiter_status.ok() &&
         commit_first.ok() && commit_second.ok() && final_ok;
}

bool RunDdlWait(EventLog &events, Metrics &metrics) {
  TempDatabase temp("ddl_wait");
  DatabaseEngine engine(temp.path, 16);
  if (!engine.GetInitStatus().ok() ||
      !engine.ExecuteSql("CREATE TABLE target(id INT);").ok()) {
    events.Add("server", "准备数据", "初始化失败", "error");
    return false;
  }

  auto dml = engine.CreateSession();
  auto ddl = engine.CreateSession();
  if (!dml.ok() || !ddl.ok() ||
      !engine.ExecuteSql(dml.value(), "BEGIN;").ok() ||
      !engine.ExecuteSql(dml.value(), "INSERT INTO target VALUES(1);").ok()) {
    events.Add("server", "创建会话", "DML 事务启动失败", "error");
    return false;
  }
  events.Add("DML", "BEGIN", "插入并持有表锁", "success");

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool ddl_started = false;
  bool ddl_finished = false;
  Status ddl_status = Status::InternalError("DDL not finished");
  std::thread ddl_worker([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      ddl_started = true;
      state_changed.notify_all();
    }
    auto result = engine.ExecuteSql(ddl.value(), "DROP TABLE target;");
    ddl_status = result.ok() ? Status::Ok() : result.status();
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      ddl_finished = true;
      state_changed.notify_all();
    }
  });

  bool observed_wait = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    state_changed.wait(lock, [&] { return ddl_started; });
    observed_wait = !state_changed.wait_for(
        lock, std::chrono::milliseconds(75), [&] { return ddl_finished; });
  }
  events.Add("DDL", "LOCK WAIT", "DROP TABLE 等待 DML 提交",
             observed_wait ? "wait" : "error");

  auto commit = engine.ExecuteSql(dml.value(), "COMMIT;");
  events.Add("DML", "COMMIT", "释放目标表锁", commit.ok() ? "success" : "error");

  bool ddl_completed = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    ddl_completed = state_changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return ddl_finished; });
  }
  ddl_worker.join();
  const bool table_gone =
      ddl_completed && !engine.ExecuteSql("SELECT * FROM target;").ok();
  events.Add("DDL", "DROP TABLE", table_gone ? "表已删除" : "表仍存在",
             table_gone ? "success" : "error");

  metrics.push_back({"DDL 等待", observed_wait ? "已阻塞" : "未阻塞"});
  metrics.push_back({"DML 提交", commit.ok() ? "成功" : "失败"});
  metrics.push_back({"最终表状态", table_gone ? "已删除" : "存在"});

  (void)engine.CloseSession(dml.value());
  (void)engine.CloseSession(ddl.value());
  (void)engine.Close();
  return observed_wait && ddl_completed && ddl_status.ok() && commit.ok() &&
         table_gone;
}

bool RunDeadlockDetection(EventLog &events, Metrics &metrics) {
  LockManager locks;
  Transaction first(9101, true);
  Transaction second(9102, true);
  constexpr page_id_t first_page = 101;
  constexpr page_id_t second_page = 102;

  if (!locks.LockPage(&first, first_page, LockMode::X).ok() ||
      !locks.LockPage(&second, second_page, LockMode::X).ok()) {
    events.Add("server", "准备锁", "初始排他锁失败", "error");
    return false;
  }
  events.Add("T1", "LOCK", "持有 page 101 的 X 锁", "success");
  events.Add("T2", "LOCK", "持有 page 102 的 X 锁", "success");

  StartGate gate(2);
  Status first_wait = Status::InternalError("first waiter not finished");
  Status second_wait = Status::InternalError("second waiter not finished");
  std::mutex waiter_mutex;
  std::condition_variable waiter_changed;
  bool first_finished = false;
  bool second_finished = false;
  std::thread first_waiter([&] {
    gate.Wait();
    first_wait = locks.LockPage(&first, second_page, LockMode::X);
    {
      std::lock_guard<std::mutex> lock(waiter_mutex);
      first_finished = true;
      waiter_changed.notify_all();
    }
  });
  std::thread second_waiter([&] {
    gate.Wait();
    second_wait = locks.LockPage(&second, first_page, LockMode::X);
    {
      std::lock_guard<std::mutex> lock(waiter_mutex);
      second_finished = true;
      waiter_changed.notify_all();
    }
  });

  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (locks.GetWaitingRequestCount() < 2 && Clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool cycle_ready = locks.GetWaitingRequestCount() >= 2;
  events.Add("server", "检测", cycle_ready ? "发现两个等待者" : "等待者不足",
             cycle_ready ? "wait" : "error");

  std::vector<txn_id_t> victims;
  if (cycle_ready) victims = locks.DetectDeadlocks();
  const bool victim_selected =
      victims.size() == 1 && victims[0] == second.id() &&
      second.state() == TransactionState::Failed;
  if (victim_selected) {
    events.Add("T2", "VICTIM", "检测器选择较年轻事务作为受害者", "error");
  }
  (void)locks.UnlockAll(&second);
  {
    std::unique_lock<std::mutex> lock(waiter_mutex);
    waiter_changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return first_finished; });
  }
  (void)locks.UnlockAll(&first);
  first_waiter.join();
  {
    std::unique_lock<std::mutex> lock(waiter_mutex);
    waiter_changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return second_finished; });
  }
  second_waiter.join();

  const bool survivor_ok = first_wait.ok();
  const bool victim_cancelled = !second_wait.ok();
  const bool waiters_cleared = locks.GetWaitingRequestCount() == 0;
  metrics.push_back({"检测到的受害者", victims.size() == 1
                                             ? "T" + std::to_string(victims[0])
                                             : "无"});
  metrics.push_back({"幸存事务", survivor_ok ? "T9101" : "失败"});
  metrics.push_back({"受害者等待请求", victim_cancelled ? "已取消" : "未取消"});
  return cycle_ready && victim_selected && survivor_ok && victim_cancelled &&
         waiters_cleared;
}

}  // namespace

Result<nlohmann::json> ConcurrencyDemoService::Run() {
  std::lock_guard<std::mutex> lock(run_mutex_);
  auto scenarios = nlohmann::json::array();
  scenarios.push_back(RunScenario(
      "parallel_commit", "多会话并发提交",
      "两个事务在不同表上并发执行并提交。", RunParallelCommit));
  scenarios.push_back(RunScenario(
      "same_key_wait", "同一行锁等待",
      "后发事务等待先发事务释放同一索引键和页面锁。", RunSameKeyWait));
  scenarios.push_back(RunScenario(
      "ddl_wait", "DDL 等待 DML",
      "DROP TABLE 等待目标表上的事务结束。", RunDdlWait));
  scenarios.push_back(RunScenario(
      "deadlock_victim", "死锁检测与回滚",
      "循环等待被识别，较年轻事务被选为受害者。", RunDeadlockDetection));

  bool passed = true;
  for (const auto &scenario : scenarios) {
    if (!scenario.value("passed", false)) {
      passed = false;
      break;
    }
  }
  return Result<nlohmann::json>(
      nlohmann::json{{"passed", passed}, {"scenarios", std::move(scenarios)}});
}

}  // namespace oursql
