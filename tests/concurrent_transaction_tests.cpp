#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"
#include "oursql/transaction/lock_manager.h"
#include "oursql/transaction/transaction_manager.h"

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

}  // namespace

int main() {
  int failures = 0;
  if (!TestConcurrentBegin()) ++failures;
  if (!TestLockCompatibilityAndFairness()) ++failures;
  if (!TestLockUpgradeAndDeadlockVictim()) ++failures;
  if (!TestTransactionManagerDeadlockResolution()) ++failures;
  if (!TestBufferPoolPageLockIntegration()) ++failures;
  if (!TestInterleavedWalChains()) ++failures;
  if (!TestConcurrentWalFlush()) ++failures;
  if (failures == 0) {
    std::cout << "concurrent transaction tests passed\n";
    return 0;
  }
  std::cerr << failures << " concurrent transaction test(s) failed\n";
  return 1;
}
