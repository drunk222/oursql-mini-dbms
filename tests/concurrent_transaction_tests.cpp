#include "oursql/execution/database_engine.h"
#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"
#include "oursql/transaction/lock_manager.h"
#include "oursql/transaction/transaction_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

class StartGate {
 public:
  explicit StartGate(std::size_t participants) : participants_(participants) {}

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ == participants_) {
      open_ = true;
      condition_.notify_all();
      return;
    }
    condition_.wait(lock, [&] { return open_; });
  }

 private:
  std::size_t participants_{0};
  std::size_t arrived_{0};
  bool open_{false};
  std::mutex mutex_;
  std::condition_variable condition_;
};

struct TempFiles {
  std::filesystem::path database;

  TempFiles() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    database = std::filesystem::temp_directory_path() /
               ("oursql_concurrent_transaction_tests_" + std::to_string(stamp) + ".oursql");
  }

  ~TempFiles() {
    std::error_code error;
    std::filesystem::remove(database, error);
    std::filesystem::remove(database.string() + ".wal", error);
  }
};

bool TestConcurrentBegin() {
  TempFiles files;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(files.database).ok(), "concurrent begin database should open") ||
      !Check(log.Open(files.database).ok(), "concurrent begin WAL should open")) {
    return false;
  }
  oursql::TransactionManager manager(&log, nullptr, &disk);
  StartGate gate(2);
  std::vector<std::shared_ptr<oursql::Transaction>> transactions(2);
  std::vector<oursql::Status> statuses(2, oursql::Status::InternalError("not started"));
  std::vector<std::thread> workers;
  for (std::size_t i = 0; i < 2; ++i) {
    workers.emplace_back([&, i] {
      gate.Wait();
      auto result = manager.Begin();
      if (result.ok()) transactions[i] = result.value();
      statuses[i] = result.ok() ? oursql::Status::Ok() : result.status();
    });
  }
  for (auto &worker : workers) worker.join();

  const bool started = Check(statuses[0].ok() && statuses[1].ok(),
                             "two sessions should begin independently") &&
                       Check(transactions[0]->id() != transactions[1]->id(),
                             "concurrent transactions should have unique ids") &&
                       Check(manager.ActiveTransactionCount() == 2,
                             "transaction manager should retain both active transactions") &&
                       Check(manager.HasAnyTransactionInProgress(),
                             "global transaction check should see multi-session transactions") &&
                       Check(!manager.ValidateCheckpointAllowed().ok(),
                             "checkpoint should reject multi-session transactions");
  const bool aborted = Check(manager.Abort(transactions[0]).ok(), "first transaction abort") &&
                       Check(manager.Abort(transactions[1]).ok(), "second transaction abort");
  const bool closed = Check(log.Close().ok() && disk.Close().ok(),
                            "concurrent begin files should close") &&
                      Check(!manager.HasAnyTransactionInProgress(),
                            "global transaction check should clear after abort");
  return started && aborted && closed;
}

bool TestLockCompatibilityAndFairness() {
  oursql::LockManager locks;
  oursql::Transaction reader(1, true);
  oursql::Transaction writer(2, true);
  oursql::Transaction later_reader(3, true);

  if (!Check(locks.LockTable(&reader, 7, oursql::TableLockMode::S).ok(),
             "table S lock should be granted") ||
      !Check(locks.LockTable(&writer, 7, oursql::TableLockMode::IS).ok(),
             "table IS should be compatible with S")) {
    return false;
  }

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool writer_started = false;
  bool writer_finished = false;
  oursql::Status writer_status = oursql::Status::InternalError("writer not finished");
  std::thread writer_thread([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      writer_started = true;
      state_changed.notify_all();
    }
    writer_status = locks.LockTable(&writer, 7, oursql::TableLockMode::X);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      writer_finished = true;
      state_changed.notify_all();
    }
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 2s, [&] { return writer_started; }),
               "writer should enter the lock wait path")) {
      lock.unlock();
      (void)locks.UnlockAll(&reader);
      writer_thread.join();
      return false;
    }
    if (!Check(!writer_finished, "incompatible table writer should wait")) {
      lock.unlock();
      (void)locks.UnlockAll(&reader);
      writer_thread.join();
      return false;
    }
  }

  bool later_reader_finished = false;
  oursql::Status later_reader_status = oursql::Status::InternalError("reader not finished");
  std::thread later_reader_thread([&] {
    later_reader_status = locks.LockTable(&later_reader, 7, oursql::TableLockMode::S);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      later_reader_finished = true;
      state_changed.notify_all();
    }
  });
  std::this_thread::yield();
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(!later_reader_finished,
               "a reader behind a waiting writer must not bypass the writer")) {
      lock.unlock();
      (void)locks.UnlockAll(&reader);
      writer_thread.join();
      later_reader_thread.join();
      return false;
    }
  }

  (void)locks.UnlockAll(&reader);
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 2s, [&] { return writer_finished; }),
               "writer should proceed after the reader releases")) {
      lock.unlock();
      writer_thread.join();
      later_reader_thread.join();
      return false;
    }
  }
  const bool writer_ok = Check(writer_status.ok(), "waiting table writer should succeed");
  (void)locks.UnlockAll(&writer);
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    const bool reader_done = state_changed.wait_for(lock, 2s, [&] {
      return later_reader_finished;
    });
    if (!Check(reader_done, "reader should proceed after the writer releases")) {
      lock.unlock();
      later_reader_thread.join();
      writer_thread.join();
      return false;
    }
  }
  writer_thread.join();
  later_reader_thread.join();
  const bool reader_ok = Check(later_reader_status.ok(),
                               "fairness reader should eventually succeed");
  (void)locks.UnlockAll(&later_reader);

  oursql::Transaction page_reader(4, true);
  oursql::Transaction page_writer(5, true);
  const bool page_lock = Check(locks.LockPage(&page_reader, 11, oursql::LockMode::S).ok(),
                               "page S lock should be granted");
  bool page_writer_finished = false;
  oursql::Status page_writer_status = oursql::Status::InternalError("page writer not finished");
  std::thread page_writer_thread([&] {
    page_writer_status = locks.LockPage(&page_writer, 11, oursql::LockMode::X);
    std::lock_guard<std::mutex> lock(state_mutex);
    page_writer_finished = true;
    state_changed.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 2s, [&] { return locks.GetWaitingRequestCount() != 0; }),
               "page X request should wait behind page S")) {
      lock.unlock();
      (void)locks.UnlockAll(&page_reader);
      page_writer_thread.join();
      return false;
    }
  }
  const bool page_waited = Check(!page_writer_finished,
                                 "page X request must not bypass an existing S lock");
  (void)locks.UnlockAll(&page_reader);
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 2s, [&] { return page_writer_finished; }),
               "page X request should proceed after page S release")) {
      lock.unlock();
      page_writer_thread.join();
      return false;
    }
  }
  page_writer_thread.join();
  const bool page_writer_ok = Check(page_writer_status.ok(), "page X lock should eventually succeed");
  (void)locks.UnlockAll(&page_writer);
  return writer_ok && reader_ok && page_lock && page_waited && page_writer_ok;
}

bool TestLockUpgradeAndDeadlockVictim() {
  oursql::LockManager locks;
  oursql::Transaction first(10, true);
  oursql::Transaction second(20, true);
  if (!Check(locks.LockPage(&first, 21, oursql::LockMode::S).ok(),
             "first S lock for upgrade should succeed") ||
      !Check(locks.LockPage(&second, 21, oursql::LockMode::S).ok(),
             "second S lock for upgrade should succeed")) {
    return false;
  }

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool upgrade_started = false;
  bool upgrade_finished = false;
  oursql::Status upgrade_status = oursql::Status::InternalError("upgrade not finished");
  std::thread upgrade([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      upgrade_started = true;
      state_changed.notify_all();
    }
    upgrade_status = locks.LockPage(&first, 21, oursql::LockMode::X);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      upgrade_finished = true;
      state_changed.notify_all();
    }
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 2s, [&] { return upgrade_started; }),
               "upgrade should start")) {
      lock.unlock();
      (void)locks.UnlockAll(&second);
      upgrade.join();
      return false;
    }
  }
  std::this_thread::yield();
  const bool waiting_upgrade = Check(!upgrade_finished, "S to X upgrade should wait for readers");
  (void)locks.UnlockAll(&second);
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    const bool finished = state_changed.wait_for(lock, 2s, [&] { return upgrade_finished; });
    if (!Check(finished, "S to X upgrade should finish after the other reader releases")) {
      lock.unlock();
      upgrade.join();
      return false;
    }
  }
  upgrade.join();
  const bool upgrade_ok = Check(upgrade_status.ok(), "S to X upgrade should succeed");
  (void)locks.UnlockAll(&first);

  oursql::Transaction deadlock_a(30, true);
  oursql::Transaction deadlock_b(40, true);
  if (!Check(locks.LockPage(&deadlock_a, 31, oursql::LockMode::X).ok(),
             "deadlock A first lock") ||
      !Check(locks.LockPage(&deadlock_b, 32, oursql::LockMode::X).ok(),
             "deadlock B first lock")) {
    return false;
  }
  oursql::Status wait_a = oursql::Status::InternalError("A not finished");
  oursql::Status wait_b = oursql::Status::InternalError("B not finished");
  std::mutex deadlock_mutex;
  std::condition_variable deadlock_changed;
  int waiters = 0;
  std::thread thread_a([&] {
    {
      std::lock_guard<std::mutex> lock(deadlock_mutex);
      ++waiters;
      deadlock_changed.notify_all();
    }
    wait_a = locks.LockPage(&deadlock_a, 32, oursql::LockMode::X);
  });
  std::thread thread_b([&] {
    {
      std::lock_guard<std::mutex> lock(deadlock_mutex);
      ++waiters;
      deadlock_changed.notify_all();
    }
    wait_b = locks.LockPage(&deadlock_b, 31, oursql::LockMode::X);
  });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (locks.GetWaitingRequestCount() < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  if (!Check(locks.GetWaitingRequestCount() >= 2,
             "both deadlock waiters should be registered")) {
    (void)locks.UnlockAll(&deadlock_a);
    (void)locks.UnlockAll(&deadlock_b);
    thread_a.join();
    thread_b.join();
    return false;
  }
  const auto victims = locks.DetectDeadlocks();
  const bool victim_ok = Check(victims.size() == 1 && victims[0] == deadlock_b.id(),
                               "deadlock detector should choose the youngest transaction") &&
                         Check(deadlock_b.state() == oursql::TransactionState::Failed,
                               "deadlock victim should be marked Failed");
  thread_b.join();
  (void)locks.UnlockAll(&deadlock_b);
  thread_a.join();
  const bool survivor_ok = Check(wait_b.code() == oursql::ErrorCode::InvalidArgument,
                                 "deadlock victim waiter should return promptly") &&
                           Check(wait_a.ok(), "surviving transaction should continue");
  (void)locks.UnlockAll(&deadlock_a);

  // An upgrade request is represented by a granted S request plus an X
  // request waiting on the same queue. A deadlock victim must remove that
  // combined request before another transaction can use the page.
  oursql::Transaction upgrade_victim(70, true);
  oursql::Transaction upgrade_holder(60, true);
  if (!Check(locks.LockPage(&upgrade_victim, 41, oursql::LockMode::X).ok(),
             "upgrade victim first page lock") ||
      !Check(locks.LockPage(&upgrade_victim, 42, oursql::LockMode::S).ok(),
             "upgrade victim shared page lock") ||
      !Check(locks.LockPage(&upgrade_holder, 42, oursql::LockMode::S).ok(),
             "upgrade holder shared page lock")) {
    return false;
  }
  oursql::Status upgrade_victim_status =
      oursql::Status::InternalError("upgrade victim waiter not finished");
  oursql::Status upgrade_holder_status =
      oursql::Status::InternalError("upgrade holder waiter not finished");
  std::thread upgrade_holder_thread([&] {
    upgrade_holder_status = locks.LockPage(&upgrade_holder, 41, oursql::LockMode::X);
  });
  while (locks.GetWaitingRequestCount() < 1) std::this_thread::yield();
  std::thread upgrade_victim_thread([&] {
    upgrade_victim_status = locks.LockPage(&upgrade_victim, 42, oursql::LockMode::X);
  });
  while (locks.GetWaitingRequestCount() < 2) std::this_thread::yield();

  const auto upgrade_victim_ids = locks.DetectDeadlocks();
  const bool upgrade_victim_marked =
      Check(upgrade_victim_ids.size() == 1 && upgrade_victim_ids[0] == upgrade_victim.id(),
            "deadlock detector should choose the upgrade victim") &&
      Check(upgrade_victim.state() == oursql::TransactionState::Failed,
            "upgrade deadlock victim should be marked Failed");
  upgrade_victim_thread.join();
  (void)locks.UnlockAll(&upgrade_victim);
  upgrade_holder_thread.join();
  const bool upgrade_victim_cleaned =
      Check(upgrade_victim_status.code() == oursql::ErrorCode::InvalidArgument,
            "failed upgrade waiter should return promptly") &&
      Check(upgrade_holder_status.ok(),
            "transaction waiting on the victim should proceed: " +
                upgrade_holder_status.message());
  (void)locks.UnlockAll(&upgrade_holder);
  return waiting_upgrade && upgrade_ok && victim_ok && survivor_ok &&
         upgrade_victim_marked && upgrade_victim_cleaned &&
         Check(locks.GetWaitingRequestCount() == 0,
               "failed upgrade cleanup should leave no waiting request");
}

bool TestTransactionManagerDeadlockResolution() {
  TempFiles files;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(files.database).ok(), "deadlock resolution database open") ||
      !Check(log.Open(files.database).ok(), "deadlock resolution WAL open")) {
    return false;
  }
  oursql::BufferPoolManager pool(2, &disk);
  oursql::LockManager locks;
  if (!Check(pool.SetLogManager(&log).ok(), "deadlock resolution WAL binding") ||
      !Check(pool.SetLockManager(&locks).ok(), "deadlock resolution lock binding")) {
    return false;
  }
  oursql::TransactionManager manager(&log, &pool, &disk, &locks);
  auto first = manager.Begin();
  auto second = manager.Begin();
  auto first_page = disk.AllocatePage();
  auto second_page = disk.AllocatePage();
  if (!Check(first.ok() && second.ok() && first_page.ok() && second_page.ok(),
             "deadlock resolution setup")) {
    return false;
  }

  auto first_write = pool.FetchPageWrite(first_page.value(), first.value().get());
  auto second_write = pool.FetchPageWrite(second_page.value(), second.value().get());
  if (!Check(first_write.ok() && second_write.ok(), "deadlock resolution initial writes")) {
    return false;
  }
  first_write.value().Data()[0] = std::byte{0x11};
  second_write.value().Data()[0] = std::byte{0x22};
  if (!Check(first_write.value().MarkDirty().ok() && first_write.value().Release().ok() &&
                 second_write.value().MarkDirty().ok() && second_write.value().Release().ok(),
             "deadlock resolution write releases")) {
    return false;
  }

  oursql::Status first_wait = oursql::Status::InternalError("first waiter not finished");
  oursql::Status second_wait = oursql::Status::InternalError("second waiter not finished");
  std::thread first_thread([&] {
    first_wait = locks.LockPage(first.value().get(), second_page.value(), oursql::LockMode::X);
  });
  std::thread second_thread([&] {
    second_wait = locks.LockPage(second.value().get(), first_page.value(), oursql::LockMode::X);
  });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (locks.GetWaitingRequestCount() < 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool waiters_ready = Check(locks.GetWaitingRequestCount() >= 2,
                                   "deadlock resolution should observe two waiters");
  const auto resolved = manager.ResolveDeadlocksOnce();
  first_thread.join();
  second_thread.join();
  const bool victim_closed =
      Check(resolved.ok(), "deadlock resolution should abort the victim") &&
      Check(second.value()->state() == oursql::TransactionState::Aborted,
            "deadlock victim should finish in Aborted state") &&
      Check(manager.GetTransaction(second.value()->id()) == nullptr,
            "deadlock victim should leave the active transaction map") &&
      Check(second_wait.code() == oursql::ErrorCode::InvalidArgument,
            "deadlock victim waiter should be cancelled") &&
      Check(first_wait.ok(), "surviving waiter should acquire the lock") &&
      Check(locks.GetWaitingRequestCount() == 0,
            "deadlock resolution should leave no waiter behind");
  const bool survivor_committed = Check(manager.Commit(first.value()).ok(),
                                        "surviving transaction should commit") &&
                                  Check(pool.FlushAllPages().ok(),
                                        "surviving transaction page flush");

  bool victim_data_undone = false;
  {
    auto restored = pool.FetchPage(second_page.value());
    victim_data_undone = Check(restored.ok() && restored.value().Data()[0] == std::byte{0x00},
                                "deadlock victim page should be undone");
  }
  auto records = log.Scan();
  bool abort_logged = false;
  if (records.ok()) {
    for (const auto &record : records.value()) {
      if (record.txn_id == second.value()->id() &&
          record.type == oursql::LogRecordType::Abort) {
        abort_logged = true;
        break;
      }
    }
  }
  const bool wal_closed = Check(abort_logged, "deadlock victim Abort should be in WAL") &&
                          Check(pool.Close().ok(), "deadlock resolution pool close") &&
                          Check(log.Close().ok(), "deadlock resolution WAL close") &&
                          Check(disk.Close().ok(), "deadlock resolution database close");
  return waiters_ready && victim_closed && survivor_committed && victim_data_undone && wal_closed;
}

bool TestAutomaticDeadlockDetector() {
  TempFiles files;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(files.database).ok(), "automatic deadlock database open") ||
      !Check(log.Open(files.database).ok(), "automatic deadlock WAL open")) {
    return false;
  }
  oursql::BufferPoolManager pool(2, &disk);
  oursql::LockManager locks;
  if (!Check(pool.SetLogManager(&log).ok(), "automatic deadlock WAL binding") ||
      !Check(pool.SetLockManager(&locks).ok(), "automatic deadlock lock binding")) {
    return false;
  }
  oursql::TransactionManager manager(&log, &pool, &disk, &locks);
  if (!Check(manager.StartDeadlockDetector().ok(), "automatic deadlock detector start")) {
    return false;
  }
  if (!Check(manager.StartDeadlockDetector().ok(),
             "repeated automatic deadlock detector start")) {
    (void)manager.StopDeadlockDetector();
    return false;
  }
  auto first = manager.Begin();
  auto second = manager.Begin();
  if (!Check(first.ok() && second.ok() && first.value()->id() < second.value()->id(),
             "automatic deadlock transaction ids")) {
    (void)manager.StopDeadlockDetector();
    return false;
  }
  if (!Check(locks.LockPage(first.value().get(), 31, oursql::LockMode::X).ok(),
             "automatic deadlock T1 first lock") ||
      !Check(locks.LockPage(second.value().get(), 32, oursql::LockMode::X).ok(),
             "automatic deadlock T2 first lock")) {
    (void)locks.UnlockAll(first.value().get());
    (void)locks.UnlockAll(second.value().get());
    (void)manager.Abort(first.value());
    (void)manager.Abort(second.value());
    (void)manager.StopDeadlockDetector();
    return false;
  }

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool first_finished = false;
  bool second_finished = false;
  oursql::Status first_status = oursql::Status::InternalError("automatic T1 not finished");
  oursql::Status second_status = oursql::Status::InternalError("automatic T2 not finished");
  std::thread first_waiter([&] {
    first_status = locks.LockPage(first.value().get(), 32, oursql::LockMode::X);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      first_finished = true;
    }
    state_changed.notify_all();
  });
  std::thread second_waiter([&] {
    second_status = locks.LockPage(second.value().get(), 31, oursql::LockMode::X);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      second_finished = true;
    }
    state_changed.notify_all();
  });

  bool both_finished = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    both_finished = state_changed.wait_for(lock, 2s, [&] {
      return first_finished && second_finished;
    });
  }
  if (!both_finished) {
    // Cleanup only releases lock waiters on a test failure; the test never
    // invokes ResolveDeadlocksOnce manually.
    (void)locks.UnlockAll(first.value().get());
    (void)locks.UnlockAll(second.value().get());
  }
  first_waiter.join();
  second_waiter.join();

  const bool victim_aborted =
      Check(both_finished, "automatic deadlock detector resolves within two seconds") &&
      Check(second.value()->state() == oursql::TransactionState::Aborted,
            "automatic detector aborts the larger txn_id victim") &&
      Check(first_status.ok(), "surviving transaction continues after automatic abort") &&
      Check(second_status.code() == oursql::ErrorCode::InvalidArgument,
            "automatic detector cancels victim lock request") &&
      Check(manager.GetTransaction(second.value()->id()) == nullptr,
            "automatic victim leaves active transaction map") &&
      Check(locks.GetWaitingRequestCount() == 0,
            "automatic detector leaves no waiting lock request");
  const bool survivor_committed = Check(manager.Commit(first.value()).ok(),
                                        "automatic detector survivor commit");
  const bool stopped = Check(manager.StopDeadlockDetector().ok(),
                             "automatic detector stop") &&
                       Check(manager.StopDeadlockDetector().ok(),
                             "repeated automatic detector stop");
  const bool closed = Check(pool.Close().ok(), "automatic deadlock pool close") &&
                      Check(log.Close().ok(), "automatic deadlock WAL close") &&
                      Check(disk.Close().ok(), "automatic deadlock database close");
  return victim_aborted && survivor_committed && stopped && closed;
}

bool TestDatabaseEngineRepeatedClose() {
  TempFiles files;
  oursql::DatabaseEngine engine(files.database, 2);
  if (!Check(engine.GetInitStatus().ok(), "repeated close engine open")) return false;
  return Check(engine.Close().ok(), "first DatabaseEngine Close") &&
         Check(engine.Close().ok(), "repeated DatabaseEngine Close");
}

bool TestBufferPoolPageLockIntegration() {
  TempFiles files;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(files.database).ok(), "buffer page-lock database should open") ||
      !Check(log.Open(files.database).ok(), "buffer page-lock WAL should open")) {
    return false;
  }

  oursql::BufferPoolManager pool(2, &disk);
  oursql::LockManager locks;
  if (!Check(pool.SetLogManager(&log).ok(), "buffer page-lock WAL binding") ||
      !Check(pool.SetLockManager(&locks).ok(), "buffer page-lock manager binding")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }

  auto created = pool.NewPage();
  if (!Check(created.ok(), "buffer page-lock fixture allocation")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }
  const auto page_id = created.value().PageId();
  auto fixture_guard = std::move(created.value());
  if (!Check(fixture_guard.Release().ok(), "buffer page-lock fixture release")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }

  oursql::Transaction first(101, true);
  oursql::Transaction second(102, true);
  auto first_result = pool.FetchPageWrite(page_id, &first);
  if (!Check(first_result.ok(), "first transaction should acquire page X lock")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }
  auto first_guard = std::move(first_result.value());

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool second_started = false;
  bool second_finished = false;
  oursql::Status second_status = oursql::Status::InternalError("second transaction not finished");
  std::thread waiter([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      second_started = true;
      state_changed.notify_all();
    }
    auto result = pool.FetchPageWrite(page_id, &second);
    second_status = result.ok() ? oursql::Status::Ok() : result.status();
    if (result.ok()) {
      auto guard = std::move(result.value());
      second_status = guard.Release();
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      second_finished = true;
      state_changed.notify_all();
    }
  });

  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 2s, [&] { return second_started; }),
               "second transaction should start its page request")) {
      lock.unlock();
      (void)first_guard.Release();
      (void)locks.UnlockAll(&first);
      waiter.join();
      (void)locks.UnlockAll(&second);
      (void)pool.Close();
      (void)log.Close();
      (void)disk.Close();
      return false;
    }
  }

  const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
  while (locks.GetWaitingRequestCount() == 0 &&
         std::chrono::steady_clock::now() < wait_deadline) {
    std::this_thread::yield();
  }
  const bool blocked = Check(locks.GetWaitingRequestCount() != 0,
                             "second transaction should wait for page X lock");
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!Check(!second_finished, "page lock wait must not finish early")) {
      // Continue cleanup below after recording the failed assertion.
    }
  }

  const bool first_released = Check(first_guard.Release().ok(),
                                    "first page guard should release cleanly");
  const bool first_unlocked = Check(locks.UnlockAll(&first).ok(),
                                    "first transaction should release page lock");
  bool second_done = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    second_done = state_changed.wait_for(lock, 2s, [&] { return second_finished; });
  }
  waiter.join();
  const bool second_ok = Check(second_done && second_status.ok(),
                               "second transaction should continue after unlock");
  const bool second_unlocked = Check(locks.UnlockAll(&second).ok(),
                                     "second transaction should release page lock");
  const bool close_ok = Check(pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
                              "buffer page-lock resources should close");
  return blocked && first_released && first_unlocked && second_ok && second_unlocked && close_ok;
}

bool TestInterleavedWalChains() {
  TempFiles files;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(files.database).ok(), "interleaved WAL database should open") ||
      !Check(log.Open(files.database).ok(), "interleaved WAL should open")) {
    return false;
  }
  oursql::BufferPoolManager pool(2, &disk);
  if (!Check(pool.SetLogManager(&log).ok(), "interleaved WAL binding")) return false;

  auto first_page_result = pool.NewPage();
  auto second_page_result = pool.NewPage();
  if (!Check(first_page_result.ok() && second_page_result.ok(),
             "interleaved WAL fixture pages")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }
  const auto first_page_id = first_page_result.value().PageId();
  const auto second_page_id = second_page_result.value().PageId();
  auto first_fixture = std::move(first_page_result.value());
  auto second_fixture = std::move(second_page_result.value());
  if (!Check(first_fixture.Release().ok() && second_fixture.Release().ok(),
             "interleaved WAL fixture release")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }
  if (!Check(pool.FlushAllPages().ok(), "interleaved WAL fixture flush")) return false;

  oursql::TransactionManager manager(&log, &pool, &disk);
  auto first_transaction = manager.Begin();
  auto second_transaction = manager.Begin();
  if (!Check(first_transaction.ok() && second_transaction.ok(),
             "interleaved WAL transactions should begin")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }

  StartGate gate(2);
  std::vector<oursql::Status> statuses(2,
                                      oursql::Status::InternalError("WAL worker not finished"));
  std::thread first_worker([&] {
    gate.Wait();
    auto result = pool.FetchPageWrite(first_page_id, first_transaction.value().get());
    if (!result.ok()) {
      statuses[0] = result.status();
      return;
    }
    auto guard = std::move(result.value());
    guard.Data()[0] = std::byte{0x31};
    statuses[0] = guard.MarkDirty();
    const auto release = guard.Release();
    if (statuses[0].ok()) statuses[0] = release;
  });
  std::thread second_worker([&] {
    gate.Wait();
    auto result = pool.FetchPageWrite(second_page_id, second_transaction.value().get());
    if (!result.ok()) {
      statuses[1] = result.status();
      return;
    }
    auto guard = std::move(result.value());
    guard.Data()[0] = std::byte{0x42};
    statuses[1] = guard.MarkDirty();
    const auto release = guard.Release();
    if (statuses[1].ok()) statuses[1] = release;
  });
  first_worker.join();
  second_worker.join();
  if (!Check(statuses[0].ok() && statuses[1].ok(),
             "interleaved WAL page updates should succeed")) {
    (void)manager.Abort(first_transaction.value());
    (void)manager.Abort(second_transaction.value());
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }

  auto records = log.Scan();
  if (!Check(records.ok(), "interleaved WAL should scan")) return false;
  std::unordered_map<oursql::txn_id_t, std::vector<oursql::lsn_t>> chains;
  std::unordered_map<oursql::lsn_t, const oursql::LogRecord *> by_lsn;
  for (const auto &record : records.value()) {
    by_lsn.emplace(record.lsn, &record);
    if (record.txn_id != oursql::kInvalidTxnId) chains[record.txn_id].push_back(record.lsn);
  }
  bool chains_ok = true;
  for (const auto &transaction : {first_transaction.value(), second_transaction.value()}) {
    const auto found = chains.find(transaction->id());
    if (found == chains.end() || found->second.size() < 2) {
      chains_ok = false;
      continue;
    }
    for (std::size_t i = 1; i < found->second.size(); ++i) {
      const auto &record = *by_lsn.at(found->second[i]);
      if (record.prev_lsn != found->second[i - 1]) chains_ok = false;
    }
  }
  const bool chain_check = Check(chains_ok, "each transaction WAL chain should be contiguous");
  const bool abort_first = Check(manager.Abort(first_transaction.value()).ok(),
                                 "first interleaved WAL transaction abort");
  const bool abort_second = Check(manager.Abort(second_transaction.value()).ok(),
                                  "second interleaved WAL transaction abort");
  const bool close_ok = Check(pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
                              "interleaved WAL resources should close");
  return chain_check && abort_first && abort_second && close_ok;
}

bool TestConcurrentWalFlush() {
  TempFiles files;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(files.database).ok(), "concurrent flush database should open") ||
      !Check(log.Open(files.database).ok(), "concurrent flush WAL should open")) {
    return false;
  }

  oursql::LogRecord first;
  first.type = oursql::LogRecordType::Begin;
  first.txn_id = 901;
  auto first_lsn = log.Append(first);
  oursql::LogRecord second;
  second.type = oursql::LogRecordType::Begin;
  second.txn_id = 902;
  auto second_lsn = log.Append(second);
  if (!Check(first_lsn.ok() && second_lsn.ok(),
             "concurrent flush WAL fixture append") ||
      !Check(second_lsn.value() > first_lsn.value(),
             "concurrent flush target should be the latest LSN")) {
    (void)log.Close();
    (void)disk.Close();
    return false;
  }

  constexpr std::size_t kFlushers = 6;
  StartGate gate(kFlushers);
  std::vector<oursql::Status> statuses(
      kFlushers, oursql::Status::InternalError("flush worker not finished"));
  std::vector<std::thread> workers;
  workers.reserve(kFlushers);
  for (std::size_t i = 0; i < kFlushers; ++i) {
    workers.emplace_back([&, i] {
      gate.Wait();
      statuses[i] = log.Flush(second_lsn.value());
    });
  }
  for (auto &worker : workers) worker.join();

  bool all_ok = true;
  for (const auto &status : statuses) all_ok = all_ok && status.ok();
  const bool flushed =
      Check(all_ok, "concurrent WAL flush callers should all complete") &&
      Check(log.GetDurableLsn() >= second_lsn.value(),
            "concurrent WAL flush should advance durable LSN");

  const bool closed = Check(log.Close().ok() && disk.Close().ok(),
                            "concurrent flush files should close");
  return flushed && closed;
}

bool TestSessionLifecycleAndAutocommit() {
  TempFiles files;
  oursql::DatabaseEngine engine(files.database, 8);
  if (!Check(engine.GetInitStatus().ok(), "session test engine should open") ||
      !Check(engine.ExecuteSql(
                 "CREATE TABLE first_table(id INT, name VARCHAR(32));"
                 "CREATE TABLE second_table(id INT, name VARCHAR(32));")
                 .ok(),
             "session test table setup")) {
    return false;
  }

  auto first = engine.CreateSession();
  auto second = engine.CreateSession();
  if (!Check(first.ok() && second.ok(), "two sessions should be created") ||
      !Check(first.value()->session_id != second.value()->session_id,
             "sessions should have unique ids")) {
    return false;
  }

  std::vector<oursql::Status> begin_status(
      2, oursql::Status::InternalError("session begin not finished"));
  StartGate begin_gate(2);
  std::thread first_begin([&] {
    begin_gate.Wait();
    auto result = engine.ExecuteSql(first.value(), "BEGIN;");
    begin_status[0] = result.ok() ? oursql::Status::Ok() : result.status();
  });
  std::thread second_begin([&] {
    begin_gate.Wait();
    auto result = engine.ExecuteSql(second.value(), "BEGIN;");
    begin_status[1] = result.ok() ? oursql::Status::Ok() : result.status();
  });
  first_begin.join();
  second_begin.join();
  if (!Check(begin_status[0].ok() && begin_status[1].ok(),
             "two sessions should begin independently") ||
      !Check(first.value()->current_transaction != nullptr &&
                 second.value()->current_transaction != nullptr &&
                 first.value()->current_transaction->id() !=
                     second.value()->current_transaction->id(),
             "two sessions should own distinct transactions")) {
    return false;
  }

  auto nested = engine.ExecuteSql(first.value(), "BEGIN;");
  if (!Check(!nested.ok() &&
                 nested.status().code() == oursql::ErrorCode::AlreadyExists,
             "same session nested BEGIN should be rejected")) {
    return false;
  }
  if (!Check(engine.ExecuteSql(first.value(), "ROLLBACK;").ok() &&
                 engine.ExecuteSql(second.value(), "ROLLBACK;").ok(),
             "session transactions should roll back")) {
    return false;
  }

  // Hold the session execution mutex while another worker calls ExecuteSql.
  // This verifies that one session cannot run two SQL statements in parallel.
  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool started = false;
  bool finished = false;
  oursql::Result<oursql::ExecutionResult> serialized =
      oursql::Result<oursql::ExecutionResult>(
          oursql::Status::InternalError("serialized query not finished"));
  std::unique_lock<std::mutex> session_guard(first.value()->execute_mutex);
  std::thread serialized_worker([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      started = true;
      state_changed.notify_all();
    }
    serialized = engine.ExecuteSql(first.value(), "SELECT * FROM first_table;");
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      finished = true;
      state_changed.notify_all();
    }
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 1s, [&] { return started; }),
               "serialized session worker should start")) {
      session_guard.unlock();
      serialized_worker.join();
      return false;
    }
    const bool finished_early =
        state_changed.wait_for(lock, 50ms, [&] { return finished; });
    if (!Check(!finished_early,
               "same-session SQL must wait for execute_mutex")) {
      session_guard.unlock();
      serialized_worker.join();
      return false;
    }
  }
  session_guard.unlock();
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 1s, [&] { return finished; }),
               "serialized session worker should finish after unlock")) {
      serialized_worker.join();
      return false;
    }
  }
  serialized_worker.join();
  if (!Check(serialized.ok(), "serialized session SELECT should succeed")) {
    return false;
  }

  std::vector<oursql::Status> write_status(
      2, oursql::Status::InternalError("autocommit write not finished"));
  StartGate write_gate(2);
  std::thread first_write([&] {
    write_gate.Wait();
    write_status[0] =
        engine.ExecuteSql(first.value(),
                          "INSERT INTO first_table VALUES(1, 'first');")
                .ok()
            ? oursql::Status::Ok()
            : oursql::Status::InternalError("first autocommit failed");
  });
  std::thread second_write([&] {
    write_gate.Wait();
    write_status[1] =
        engine.ExecuteSql(second.value(),
                          "INSERT INTO second_table VALUES(1, 'second');")
                .ok()
            ? oursql::Status::Ok()
            : oursql::Status::InternalError("second autocommit failed");
  });
  first_write.join();
  second_write.join();
  const bool writes_ok =
      Check(write_status[0].ok() && write_status[1].ok(),
            "autocommit writes to different tables should both finish") &&
      Check(first.value()->current_transaction == nullptr &&
                second.value()->current_transaction == nullptr,
            "autocommit should clear each session transaction");
  const auto first_rows =
      engine.ExecuteSql(first.value(), "SELECT * FROM first_table;");
  const auto second_rows =
      engine.ExecuteSql(second.value(), "SELECT * FROM second_table;");
  const bool rows_ok =
      Check(first_rows.ok() && first_rows.value().rows.size() == 1,
            "first autocommit row should be visible") &&
      Check(second_rows.ok() && second_rows.value().rows.size() == 1,
            "second autocommit row should be visible");
  const bool closed = Check(engine.CloseSession(first.value()).ok() &&
                                engine.CloseSession(second.value()).ok(),
                            "sessions should close") &&
                      Check(engine.Close().ok(), "session test engine close");
  return writes_ok && rows_ok && closed;
}

bool TestFailedSessionAndAutocommitRollback() {
  TempFiles files;
  oursql::DatabaseEngine engine(files.database, 8);
  if (!Check(engine.GetInitStatus().ok(), "failed-session engine should open") ||
      !Check(engine.ExecuteSql(
                 "CREATE TABLE student(id INT, name VARCHAR(32));"
                 "INSERT INTO student VALUES(1, 'Alice');"
                 "CREATE UNIQUE INDEX idx_student_id ON student(id);")
                 .ok(),
             "failed-session setup")) {
    return false;
  }
  auto session = engine.CreateSession();
  if (!Check(session.ok(), "failed-session create")) return false;

  if (!Check(engine.ExecuteSql(session.value(), "BEGIN;").ok(),
             "failed-session begin")) {
    return false;
  }
  auto bad = engine.ExecuteSql(session.value(), "SELECT FROM student;");
  if (!Check(!bad.ok(), "parser error should fail the session transaction") ||
      !Check(session.value()->current_transaction->state() ==
                 oursql::TransactionState::Failed,
             "failed-session state should be Failed")) {
    return false;
  }
  const auto select_failed =
      engine.ExecuteSql(session.value(), "SELECT * FROM student;");
  const auto insert_failed = engine.ExecuteSql(
      session.value(), "INSERT INTO student VALUES(2, 'blocked');");
  const auto ddl_failed =
      engine.ExecuteSql(session.value(), "CREATE TABLE blocked(id INT);");
  const auto commit_failed = engine.ExecuteSql(session.value(), "COMMIT;");
  const auto begin_failed = engine.ExecuteSql(session.value(), "BEGIN;");
  if (!Check(!select_failed.ok() && !insert_failed.ok() && !ddl_failed.ok() &&
                 !commit_failed.ok() && !begin_failed.ok(),
             "Failed transaction should reject SQL, DDL, COMMIT and BEGIN") ||
      !Check(engine.ExecuteSql(session.value(), "ROLLBACK;").ok(),
             "Failed transaction should allow ROLLBACK")) {
    return false;
  }

  auto duplicate = engine.ExecuteSql(
      session.value(), "INSERT INTO student VALUES(1, 'duplicate');");
  if (!Check(!duplicate.ok() &&
                 duplicate.status().code() == oursql::ErrorCode::AlreadyExists,
             "autocommit index failure should be reported") ||
      !Check(session.value()->current_transaction == nullptr,
             "failed autocommit transaction should be cleaned up")) {
    return false;
  }
  auto rows = engine.ExecuteSql(session.value(), "SELECT id, name FROM student;");
  const bool rolled_back =
      Check(rows.ok() && rows.value().rows.size() == 1 &&
                rows.value().rows[0][0].AsInt() == 1,
            "autocommit failure must roll back both Heap and index");
  return rolled_back && Check(engine.CloseSession(session.value()).ok(),
                              "failed-session close") &&
         Check(engine.Close().ok(), "failed-session engine close");
}

bool TestExactIndexUpdateLocks() {
  TempFiles files;
  oursql::DatabaseEngine engine(files.database, 32);
  const std::string payload(2800, 'x');
  if (!Check(engine.GetInitStatus().ok(), "exact-index engine should open") ||
      !Check(engine.ExecuteSql(
                 "CREATE TABLE student(id INT, payload VARCHAR(3000));")
                 .ok(),
             "exact-index table setup") ||
      !Check(engine.ExecuteSql(
                 "INSERT INTO student VALUES(1, '" + payload + "');")
                 .ok(),
             "exact-index first row") ||
      !Check(engine.ExecuteSql(
                 "INSERT INTO student VALUES(2, '" + payload + "');")
                 .ok(),
             "exact-index second row") ||
      !Check(engine.ExecuteSql(
                 "CREATE UNIQUE INDEX idx_student_id ON student(id);")
                 .ok(),
             "exact-index index setup")) {
    return false;
  }

  auto first_rid = engine.ExecuteSql("SELECT id FROM student WHERE id = 1;");
  auto second_rid = engine.ExecuteSql("SELECT id FROM student WHERE id = 2;");
  if (!Check(first_rid.ok() && second_rid.ok() &&
                 first_rid.value().rids.size() == 1 &&
                 second_rid.value().rids.size() == 1 &&
                 first_rid.value().rids[0].page_id !=
                     second_rid.value().rids[0].page_id,
             "fixture rows should reside on different data pages")) {
    return false;
  }
  auto first = engine.CreateSession();
  auto second = engine.CreateSession();
  if (!Check(first.ok() && second.ok(), "exact-index sessions should be created") ||
      !Check(engine.ExecuteSql(first.value(), "BEGIN;").ok() &&
                 engine.ExecuteSql(second.value(), "BEGIN;").ok(),
             "exact-index sessions should begin")) {
    return false;
  }

  std::vector<oursql::Status> parallel_status(
      2, oursql::Status::InternalError("parallel update not finished"));
  std::mutex parallel_mutex;
  std::condition_variable parallel_changed;
  std::size_t parallel_completed = 0;
  std::vector<bool> parallel_done(2, false);
  StartGate parallel_gate(2);
  std::thread first_update([&] {
    parallel_gate.Wait();
    auto result = engine.ExecuteSql(
        first.value(), "UPDATE student SET payload = 'first' WHERE id = 1;");
    parallel_status[0] = result.ok() ? oursql::Status::Ok() : result.status();
    {
      std::lock_guard<std::mutex> lock(parallel_mutex);
      parallel_done[0] = true;
      ++parallel_completed;
      parallel_changed.notify_all();
    }
  });
  std::thread second_update([&] {
    parallel_gate.Wait();
    auto result = engine.ExecuteSql(
        second.value(), "UPDATE student SET payload = 'second' WHERE id = 2;");
    parallel_status[1] = result.ok() ? oursql::Status::Ok() : result.status();
    {
      std::lock_guard<std::mutex> lock(parallel_mutex);
      parallel_done[1] = true;
      ++parallel_completed;
      parallel_changed.notify_all();
    }
  });
  const auto cleanup_parallel_updates = [&] {
    // Wait for one worker to release its session mutex before submitting
    // cleanup SQL. That rollback can then release locks needed by the other
    // worker before it is joined.
    {
      std::unique_lock<std::mutex> lock(parallel_mutex);
      parallel_changed.wait_for(lock, 2s,
                                [&] { return parallel_completed != 0; });
    }
    bool first_done = false;
    bool second_done = false;
    {
      std::lock_guard<std::mutex> lock(parallel_mutex);
      first_done = parallel_done[0];
      second_done = parallel_done[1];
    }
    if (first_done) (void)engine.ExecuteSql(first.value(), "ROLLBACK;");
    if (second_done) (void)engine.ExecuteSql(second.value(), "ROLLBACK;");
    if (first_update.joinable()) first_update.join();
    if (second_update.joinable()) second_update.join();
    (void)engine.ExecuteSql(first.value(), "ROLLBACK;");
    (void)engine.ExecuteSql(second.value(), "ROLLBACK;");
  };
  const auto has_key_and_page_locks = [](const std::shared_ptr<oursql::Transaction> &transaction) {
    bool has_key = false;
    bool has_page = false;
    for (const auto &lock : transaction->GrantedLocksSnapshot()) {
      if (lock.type == oursql::LockResourceType::IndexKey) has_key = true;
      if (lock.type == oursql::LockResourceType::Page) has_page = true;
    }
    return has_key && has_page;
  };
  const auto lock_deadline = std::chrono::steady_clock::now() + 2s;
  while ((!has_key_and_page_locks(first.value()->current_transaction) ||
          !has_key_and_page_locks(second.value()->current_transaction)) &&
         std::chrono::steady_clock::now() < lock_deadline) {
    std::this_thread::yield();
  }
  if (!Check(has_key_and_page_locks(first.value()->current_transaction) &&
                 has_key_and_page_locks(second.value()->current_transaction),
             "different-page updates should concurrently hold key and page locks")) {
    cleanup_parallel_updates();
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(parallel_mutex);
    if (!Check(parallel_changed.wait_for(
                   lock, 1s, [&] { return parallel_completed != 0; }),
               "one exact-index update should finish without a table X lock")) {
      cleanup_parallel_updates();
      return false;
    }
  }
  bool first_completed_first = false;
  {
    std::lock_guard<std::mutex> lock(parallel_mutex);
    first_completed_first = parallel_done[0];
  }
  if (first_completed_first) {
    if (!Check(engine.ExecuteSql(first.value(), "COMMIT;").ok(),
               "first parallel exact-index update commit")) {
      cleanup_parallel_updates();
      return false;
    }
  } else if (!Check(engine.ExecuteSql(second.value(), "COMMIT;").ok(),
                    "second parallel exact-index update commit")) {
    cleanup_parallel_updates();
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(parallel_mutex);
    if (!Check(parallel_changed.wait_for(
                   lock, 1s, [&] { return parallel_completed == 2; }),
               "both exact-index updates should finish after first commit")) {
      cleanup_parallel_updates();
      return false;
    }
  }
  if (first_completed_first) {
    if (!Check(engine.ExecuteSql(second.value(), "COMMIT;").ok(),
               "second parallel exact-index update commit")) {
      cleanup_parallel_updates();
      return false;
    }
  } else if (!Check(engine.ExecuteSql(first.value(), "COMMIT;").ok(),
                    "first parallel exact-index update commit")) {
    cleanup_parallel_updates();
    return false;
  }
  first_update.join();
  second_update.join();
  if (!Check(parallel_status[0].ok() && parallel_status[1].ok(),
             "different data-page updates should both complete")) {
    return false;
  }
  if (!Check(engine.ExecuteSql(first.value(), "BEGIN;").ok() &&
                 engine.ExecuteSql(second.value(), "BEGIN;").ok(),
             "conflict sessions should begin")) {
    return false;
  }
  auto holding = engine.ExecuteSql(
      first.value(), "UPDATE student SET payload = 'holder' WHERE id = 1;");
  if (!Check(holding.ok(), "conflict holder update should succeed")) return false;
  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool waiter_started = false;
  bool waiter_finished = false;
  oursql::Status waiter_status =
      oursql::Status::InternalError("conflict waiter not finished");
  std::thread waiter([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      waiter_started = true;
      state_changed.notify_all();
    }
    auto result = engine.ExecuteSql(
        second.value(), "UPDATE student SET payload = 'waiter' WHERE id = 1;");
    waiter_status = result.ok() ? oursql::Status::Ok() : result.status();
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      waiter_finished = true;
      state_changed.notify_all();
    }
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 1s, [&] { return waiter_started; }),
               "conflict waiter should start")) {
      (void)engine.ExecuteSql(first.value(), "ROLLBACK;");
      (void)engine.ExecuteSql(second.value(), "ROLLBACK;");
      waiter.join();
      return false;
    }
    const bool finished_early =
        state_changed.wait_for(lock, 75ms, [&] { return waiter_finished; });
    if (!Check(!finished_early,
               "same index key update should wait for the first transaction")) {
      (void)engine.ExecuteSql(first.value(), "ROLLBACK;");
      waiter.join();
      (void)engine.ExecuteSql(second.value(), "ROLLBACK;");
      return false;
    }
  }
  if (!Check(engine.ExecuteSql(first.value(), "COMMIT;").ok(),
             "conflict holder should commit")) {
    (void)engine.ExecuteSql(first.value(), "ROLLBACK;");
    waiter.join();
    (void)engine.ExecuteSql(second.value(), "ROLLBACK;");
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 1s, [&] { return waiter_finished; }),
               "conflict waiter should resume after COMMIT")) {
      waiter.join();
      (void)engine.ExecuteSql(second.value(), "ROLLBACK;");
      return false;
    }
  }
  waiter.join();
  const bool waiter_ok = Check(waiter_status.ok(),
                               "conflict waiter update should succeed") &&
                         Check(engine.ExecuteSql(second.value(), "COMMIT;").ok(),
                               "conflict waiter should commit");
  auto final_rows =
      engine.ExecuteSql(second.value(), "SELECT payload FROM student WHERE id = 1;");
  const bool final_ok =
      Check(final_rows.ok() && final_rows.value().rows.size() == 1 &&
                final_rows.value().rows[0][0].AsVarchar() == "waiter",
            "committed waiter value should be visible");

  const bool closed = Check(engine.CloseSession(first.value()).ok() &&
                                engine.CloseSession(second.value()).ok(),
                            "exact-index sessions should close") &&
                      Check(engine.Close().ok(), "exact-index engine close");
  return waiter_ok && final_ok && closed;
}

bool TestDdlWaitsForTargetDml() {
  TempFiles files;
  oursql::DatabaseEngine engine(files.database, 8);
  if (!Check(engine.GetInitStatus().ok(), "DDL/DML engine should open") ||
      !Check(engine.ExecuteSql("CREATE TABLE target(id INT);").ok(),
             "DDL/DML table setup")) {
    return false;
  }
  auto dml_session = engine.CreateSession();
  auto ddl_session = engine.CreateSession();
  if (!Check(dml_session.ok() && ddl_session.ok(), "DDL/DML sessions should be created") ||
      !Check(engine.ExecuteSql(dml_session.value(), "BEGIN;").ok() &&
                 engine.ExecuteSql(dml_session.value(),
                                   "INSERT INTO target VALUES(1);")
                     .ok(),
             "DDL/DML holder transaction should start")) {
    return false;
  }

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool ddl_started = false;
  bool ddl_finished = false;
  oursql::Status ddl_status =
      oursql::Status::InternalError("DDL worker not finished");
  std::thread ddl_worker([&] {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      ddl_started = true;
      state_changed.notify_all();
    }
    auto result = engine.ExecuteSql(ddl_session.value(), "DROP TABLE target;");
    ddl_status = result.ok() ? oursql::Status::Ok() : result.status();
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      ddl_finished = true;
      state_changed.notify_all();
    }
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 1s, [&] { return ddl_started; }),
               "DDL worker should start")) {
      (void)engine.ExecuteSql(dml_session.value(), "ROLLBACK;");
      ddl_worker.join();
      return false;
    }
    const bool finished_early =
        state_changed.wait_for(lock, 75ms, [&] { return ddl_finished; });
    if (!Check(!finished_early, "DDL should wait for target-table DML locks")) {
      (void)engine.ExecuteSql(dml_session.value(), "ROLLBACK;");
      ddl_worker.join();
      return false;
    }
  }
  if (!Check(engine.ExecuteSql(dml_session.value(), "COMMIT;").ok(),
             "DDL holder transaction should commit")) {
    (void)engine.ExecuteSql(dml_session.value(), "ROLLBACK;");
    ddl_worker.join();
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    if (!Check(state_changed.wait_for(lock, 1s, [&] { return ddl_finished; }),
               "DDL should resume after DML commit")) {
      ddl_worker.join();
      return false;
    }
  }
  ddl_worker.join();
  const bool dropped =
      Check(ddl_status.ok(), "DDL should eventually succeed") &&
      Check(!engine.ExecuteSql(ddl_session.value(), "SELECT * FROM target;").ok(),
            "dropped table should no longer exist");
  const bool closed = Check(engine.CloseSession(dml_session.value()).ok() &&
                                engine.CloseSession(ddl_session.value()).ok(),
                            "DDL/DML sessions should close") &&
                      Check(engine.Close().ok(), "DDL/DML engine close");
  return dropped && closed;
}

}  // namespace

int main() {
  int failures = 0;
  if (!TestConcurrentBegin()) ++failures;
  if (!TestLockCompatibilityAndFairness()) ++failures;
  if (!TestLockUpgradeAndDeadlockVictim()) ++failures;
  if (!TestTransactionManagerDeadlockResolution()) ++failures;
  if (!TestAutomaticDeadlockDetector()) ++failures;
  if (!TestDatabaseEngineRepeatedClose()) ++failures;
  if (!TestBufferPoolPageLockIntegration()) ++failures;
  if (!TestInterleavedWalChains()) ++failures;
  if (!TestConcurrentWalFlush()) ++failures;
  if (!TestSessionLifecycleAndAutocommit()) ++failures;
  if (!TestFailedSessionAndAutocommitRollback()) ++failures;
  if (!TestExactIndexUpdateLocks()) ++failures;
  if (!TestDdlWaitsForTargetDml()) ++failures;
  if (failures == 0) {
    std::cout << "concurrent transaction tests passed\n";
    return 0;
  }
  std::cerr << failures << " concurrent transaction test(s) failed\n";
  return 1;
}
