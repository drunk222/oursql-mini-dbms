#include "oursql/transaction/lock_manager.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>

namespace oursql {

namespace {

thread_local std::size_t current_event_session = 0;

const char *ResourceName(LockResourceType type) noexcept {
  // 作用：把资源枚举转换为诊断文本；调用者：事件记录；返回：静态字符串。
  switch (type) {
    case LockResourceType::Schema: return "schema";
    case LockResourceType::Table: return "table";
    case LockResourceType::Page: return "page";
    case LockResourceType::IndexKey: return "index_key";
  }
  return "unknown";
}

const char *TableModeName(TableLockMode mode) noexcept {
  // 作用：把表锁模式转换为诊断文本；返回：IS/IX/S/SIX/X 字符串。
  switch (mode) {
    case TableLockMode::IS: return "IS";
    case TableLockMode::IX: return "IX";
    case TableLockMode::S: return "S";
    case TableLockMode::SIX: return "SIX";
    case TableLockMode::X: return "X";
  }
  return "?";
}

const char *LockModeName(LockMode mode) noexcept {
  // 作用：把页/键锁模式转换为诊断文本；返回：S 或 X。
  return mode == LockMode::X ? "X" : "S";
}

Status ValidateTransaction(Transaction *transaction) {
  // 作用：检查锁请求是否有可工作的 Active 事务；调用者：所有 LockInternal；返回：验证状态。
  if (transaction == nullptr) {
    return Status::InvalidArgument("lock request requires a Transaction");
  }
  if (!transaction->IsUsableForWork()) {
    return Status::InvalidArgument("lock request requires an Active transaction");
  }
  return Status::Ok();
}

}  // namespace

Status LockManager::LockSchema(Transaction *transaction, TableLockMode mode) {
  // 调用者：事务/数据库初始化；作用：申请全局 Schema 锁；返回：获取或等待结果。
  return LockInternal(transaction, LockId::Schema(), mode);
}

Status LockManager::LockTable(Transaction *transaction, table_id_t table_id,
                              TableLockMode mode) {
  // 调用者：Catalog/执行层；作用：申请指定表的多粒度锁；返回：获取或等待结果。
  return LockInternal(transaction, LockId::Table(table_id), mode);
}

Status LockManager::LockPage(Transaction *transaction, page_id_t page_id,
                             LockMode mode) {
  // 调用者：BufferPool/HeapTable；作用：申请指定页面的 S/X 锁；返回：获取或等待结果。
  if (page_id == INVALID_PAGE_ID || page_id == 0) {
    return Status::InvalidArgument("page lock requires a valid page id");
  }
  return LockInternal(transaction, LockId::Page(page_id), mode);
}

Status LockManager::LockIndexKey(Transaction *transaction, index_id_t index_id,
                                 const std::string &encoded_key, LockMode mode,
                                 table_id_t table_id) {
  // 调用者：索引执行层；作用：申请索引键的 S/X 锁；返回：获取或等待结果。
  if (encoded_key.empty()) {
    return Status::InvalidArgument("index-key lock requires a non-empty encoded key");
  }
  return LockInternal(transaction, LockId::IndexKey(index_id, encoded_key, table_id), mode);
}

Status LockManager::LockInternal(Transaction *transaction, LockId lock_id,
                                 TableLockMode mode) {
  const auto valid = ValidateTransaction(transaction);
  if (!valid.ok()) return valid;
  if (!IsTableResource(lock_id.type)) {
    return Status::InvalidArgument("table lock mode used for a non-table resource");
  }

  // unique_lock 支持后面的 cv.wait() 暂时解锁；普通 lock_guard 做不到这一点。
  std::unique_lock<std::mutex> lock(mutex_);
  // 登记事务指针，死锁检测选出 txn_id 后才能找到并标记对应 Transaction。
  transactions_[transaction->id()] = transaction;
  // queues_[lock_id] 取得该资源唯一的 FIFO 请求队列；不存在时自动创建。
  auto &queue = queues_[lock_id];
  auto request = std::find_if(queue.requests.begin(), queue.requests.end(),
                              [&](const LockRequest &entry) {
                                return entry.txn_id == transaction->id();
                              });
  if (request != queue.requests.end()) {
    // 同一事务已有请求时，只允许幂等获取或合法升级，禁止重复等待。
    if (request->granted && !request->upgrading &&
        SameRequestedMode(*request, mode)) {
      return Status::Ok();
    }
    if (!request->granted && !request->upgrading) {
      return Status::AlreadyExists("transaction already waits for this lock");
    }
    if (request->upgrading) {
      return Status::AlreadyExists("transaction already has a lock upgrade pending");
    } else {
      if (!CanUpgrade(request->table_lock_mode, mode)) {
        return Status::InvalidArgument("requested table lock is not a valid upgrade");
      }
      request->table_lock_mode = mode;
      request->upgrading = true;
    }
  } else {
    // 新请求追加到队尾，后面的 CanGrant 会同时检查兼容性和队列公平性。
    queue.requests.push_back(
        LockRequest{transaction->id(), transaction, true, mode, LockMode::S, false, false});
    request = std::prev(queue.requests.end());
  }

  bool wait_recorded = false;
  while (true) {
    // 每次被唤醒后都重新检查取消、事务状态和可授予条件，防止虚假唤醒。
    if (request->cancelled) {
      queue.requests.erase(request);
      queue.cv.notify_all();
      if (queue.requests.empty()) queues_.erase(lock_id);
      return Status::InvalidArgument("lock request was cancelled");
    }
    if (!transaction->IsUsableForWork()) {
      queue.requests.erase(request);
      queue.cv.notify_all();
      if (queue.requests.empty()) queues_.erase(lock_id);
      return Status::InvalidArgument("transaction was aborted while waiting for a lock");
    }
    if (CanGrant(queue, request)) {
      // 在 LockManager mutex_ 内原子地把请求变成 granted，再登记到事务对象。
      request->granted = true;
      request->upgrading = false;
      transaction->AddGrantedLock(lock_id);
      EmitEventUnlocked("GRANTED", lock_id, TableModeName(mode),
                        transaction->id());
      queue.cv.notify_all();
      return Status::Ok();
    }
    if (!wait_recorded) {
      EmitEventUnlocked("WAIT", lock_id, TableModeName(mode),
                        transaction->id());
      wait_recorded = true;
    }
    // wait 原子释放 mutex_ 并休眠；Unlock/死锁检测 notify 后会重新拿锁再返回。
    queue.cv.wait(lock);
  }
}

Status LockManager::LockInternal(Transaction *transaction, LockId lock_id,
                                 LockMode mode) {
  const auto valid = ValidateTransaction(transaction);
  if (!valid.ok()) return valid;
  if (IsTableResource(lock_id.type)) {
    return Status::InvalidArgument("page/key lock mode used for a table resource");
  }

  // 页锁和索引键锁沿用与表锁相同的排队、升级、等待流程。
  std::unique_lock<std::mutex> lock(mutex_);
  transactions_[transaction->id()] = transaction;
  auto &queue = queues_[lock_id];
  auto request = std::find_if(queue.requests.begin(), queue.requests.end(),
                              [&](const LockRequest &entry) {
                                return entry.txn_id == transaction->id();
                              });
  if (request != queue.requests.end()) {
    if (request->granted && !request->upgrading && SameRequestedMode(*request, mode)) {
      return Status::Ok();
    }
    if (!request->granted && !request->upgrading) {
      return Status::AlreadyExists("transaction already waits for this lock");
    }
    if (request->upgrading) {
      return Status::AlreadyExists("transaction already has a lock upgrade pending");
    } else {
      if (!CanUpgrade(request->lock_mode, mode)) {
        return Status::InvalidArgument("requested page/key lock is not a valid upgrade");
      }
      request->lock_mode = mode;
      request->upgrading = true;
    }
  } else {
    queue.requests.push_back(
        LockRequest{transaction->id(), transaction, false, TableLockMode::IS, mode, false, false});
    request = std::prev(queue.requests.end());
  }

  bool wait_recorded = false;
  while (true) {
    if (request->cancelled) {
      queue.requests.erase(request);
      queue.cv.notify_all();
      if (queue.requests.empty()) queues_.erase(lock_id);
      return Status::InvalidArgument("lock request was cancelled");
    }
    if (!transaction->IsUsableForWork()) {
      queue.requests.erase(request);
      queue.cv.notify_all();
      if (queue.requests.empty()) queues_.erase(lock_id);
      return Status::InvalidArgument("transaction was aborted while waiting for a lock");
    }
    if (CanGrant(queue, request)) {
      request->granted = true;
      request->upgrading = false;
      transaction->AddGrantedLock(lock_id);
      EmitEventUnlocked("GRANTED", lock_id, LockModeName(mode),
                        transaction->id());
      queue.cv.notify_all();
      return Status::Ok();
    }
    if (!wait_recorded) {
      EmitEventUnlocked("WAIT", lock_id, LockModeName(mode),
                        transaction->id());
      wait_recorded = true;
    }
    // 睡眠时不占有 LockManager mutex_，否则持锁事务将永远无法进入 Unlock。
    queue.cv.wait(lock);
  }
}

bool LockManager::IsTableResource(LockResourceType type) noexcept {
  // 作用：判断资源是否使用 TableLockMode；返回：Schema/Table 为 true。
  return type == LockResourceType::Schema || type == LockResourceType::Table;
}

bool LockManager::TableModesConflict(TableLockMode left,
                                     TableLockMode right) noexcept {
  // 作用：根据多粒度锁兼容矩阵判断两个表锁是否冲突；返回：是否冲突。
  switch (left) {
    case TableLockMode::IS:
      return right == TableLockMode::X;
    case TableLockMode::IX:
      return right == TableLockMode::S || right == TableLockMode::SIX ||
             right == TableLockMode::X;
    case TableLockMode::S:
      return right == TableLockMode::IX || right == TableLockMode::SIX ||
             right == TableLockMode::X;
    case TableLockMode::SIX:
      return right != TableLockMode::IS;
    case TableLockMode::X:
      return true;
  }
  return true;
}

bool LockManager::LockModesConflict(LockMode left, LockMode right) noexcept {
  // 作用：判断两个页/索引键 S/X 模式是否冲突；返回：是否冲突。
  return left == LockMode::X || right == LockMode::X;
}

bool LockManager::CanUpgrade(TableLockMode current,
                             TableLockMode requested) noexcept {
  // 作用：验证表锁升级路径是否合法；返回：是否允许升级。
  if (current == requested || current == TableLockMode::X) return false;
  if (requested == TableLockMode::X) return true;
  if (current == TableLockMode::IS &&
      (requested == TableLockMode::IX || requested == TableLockMode::S ||
       requested == TableLockMode::SIX)) {
    return true;
  }
  if (current == TableLockMode::IX && requested == TableLockMode::SIX) return true;
  if (current == TableLockMode::S && requested == TableLockMode::SIX) return true;
  return false;
}

bool LockManager::CanUpgrade(LockMode current, LockMode requested) noexcept {
  // 作用：验证页/键锁从 S 到 X 的升级；返回：是否允许升级。
  return current == LockMode::S && requested == LockMode::X;
}

bool LockManager::SameRequestedMode(const LockRequest &request,
                                    TableLockMode mode) noexcept {
  // 作用：判断请求是否已经持有相同表锁；返回：是否相同。
  return request.table_mode && request.table_lock_mode == mode;
}

bool LockManager::SameRequestedMode(const LockRequest &request,
                                    LockMode mode) noexcept {
  // 作用：判断请求是否已经持有相同页/键锁；返回：是否相同。
  return !request.table_mode && request.lock_mode == mode;
}

bool LockManager::RequestConflicts(const LockRequest &request,
                                   const LockRequest &other) const noexcept {
  // 作用：统一比较两条请求的资源层级和锁模式；返回：是否冲突。
  if (request.table_mode != other.table_mode) return true;
  if (request.table_mode) {
    return TableModesConflict(request.table_lock_mode, other.table_lock_mode);
  }
  return LockModesConflict(request.lock_mode, other.lock_mode);
}

bool LockManager::IsWaiting(const LockRequest &request) const noexcept {
  // 作用：判断请求是否仍在等待或等待升级；返回：是否需要继续等待。
  return !request.cancelled && (!request.granted || request.upgrading);
}

bool LockManager::CanGrant(const LockRequestQueue &queue,
                           ConstRequestIterator request) const noexcept {
  // 第一轮保证 FIFO 公平：前面只要有另一个等待者，当前请求就不能插队。
  for (auto it = queue.requests.begin(); it != request; ++it) {
    if (it->txn_id != request->txn_id && IsWaiting(*it)) return false;
  }
  // 第二轮检查所有已授予锁；存在 S/X 或表锁模式冲突就继续等待。
  for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
    if (it == request || it->txn_id == request->txn_id || !it->granted) continue;
    if (RequestConflicts(*request, *it)) return false;
  }
  return true;
}

Status LockManager::Unlock(Transaction *transaction, const LockId &lock_id) {
  if (transaction == nullptr) return Status::InvalidArgument("unlock requires a Transaction");
  // 删除请求和唤醒等待者必须在同一临界区，避免队列观察到中间状态。
  std::lock_guard<std::mutex> lock(mutex_);
  const auto queue_it = queues_.find(lock_id);
  if (queue_it == queues_.end()) return Status::NotFound("lock resource is not present");
  auto &queue = queue_it->second;
  const auto request = std::find_if(queue.requests.begin(), queue.requests.end(),
                                    [&](const LockRequest &entry) {
                                      return entry.txn_id == transaction->id();
                                    });
  if (request == queue.requests.end()) {
    return Status::NotFound("transaction does not hold the requested lock");
  }
  if (!request->granted || request->upgrading) {
    // A waiter owns an iterator into this list. Mark it cancelled and let the
    // waiting call erase itself after wake-up, avoiding iterator invalidation.
    request->cancelled = true;
    request->granted = false;
    request->upgrading = false;
  } else {
    EmitEventUnlocked(
        "RELEASE", lock_id,
        request->table_mode ? TableModeName(request->table_lock_mode)
                            : LockModeName(request->lock_mode),
        transaction->id());
    queue.requests.erase(request);
  }
  transaction->RemoveGrantedLock(lock_id);
  // 资源状态改变后唤醒全部等待者；每个线程会重新执行 CanGrant。
  queue.cv.notify_all();
  if (queue.requests.empty()) queues_.erase(queue_it);
  return Status::Ok();
}

Status LockManager::UnlockAll(Transaction *transaction) {
  if (transaction == nullptr) return Status::InvalidArgument("unlock all requires a Transaction");
  const auto txn_id = transaction->id();
  // COMMIT/ABORT 在一把锁下遍历全部队列，完整移除该事务的所有资源请求。
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto queue_it = queues_.begin(); queue_it != queues_.end();) {
    auto &queue = queue_it->second;
    for (auto request = queue.requests.begin(); request != queue.requests.end();) {
      if (request->txn_id == txn_id) {
        if (!request->granted || request->upgrading) {
          // Waiting LockInternal calls keep their list iterator until they
          // observe cancellation. Do not erase those nodes from this thread.
          request->cancelled = true;
          request->granted = false;
          request->upgrading = false;
          ++request;
        } else {
          EmitEventUnlocked(
              "RELEASE", queue_it->first,
              request->table_mode ? TableModeName(request->table_lock_mode)
                                  : LockModeName(request->lock_mode),
              txn_id);
          request = queue.requests.erase(request);
        }
      } else {
        ++request;
      }
    }
    queue.cv.notify_all();
    if (queue.requests.empty()) {
      queue_it = queues_.erase(queue_it);
    } else {
      ++queue_it;
    }
  }
  transactions_.erase(txn_id);
  transaction->ClearGrantedLocks();
  return Status::Ok();
}

void LockManager::BeginEventCapture() {
  // 调用者：并发诊断/测试；作用：开始记录锁生命周期事件；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  capture_events_ = true;
  capture_started_at_ = std::chrono::steady_clock::now();
  captured_events_.clear();
}

std::vector<LockEvent> LockManager::EndEventCapture() {
  // 调用者：并发诊断/测试；作用：停止记录并取出事件副本；返回：事件列表。
  std::lock_guard<std::mutex> lock(mutex_);
  capture_events_ = false;
  return std::move(captured_events_);
}

void LockManager::SetEventSession(std::size_t session_index) noexcept {
  // 调用者：会话线程；作用：设置当前线程的事件会话编号；返回：无。
  current_event_session = session_index;
}

void LockManager::ClearEventSession() noexcept {
  // 调用者：会话线程结束；作用：清除当前线程的事件会话编号；返回：无。
  current_event_session = 0;
}

void LockManager::EmitEventUnlocked(std::string action, const LockId &lock_id,
                                    std::string mode, txn_id_t txn_id) {
  // 调用者：已持有 LockManager mutex_ 的锁路径；作用：追加一条诊断事件；返回：无。
  if (!capture_events_) return;
  const double offset_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - capture_started_at_)
          .count();
  captured_events_.push_back(LockEvent{
      offset_ms,
      current_event_session,
      txn_id,
      std::move(action),
      ResourceName(lock_id.type),
      std::move(mode),
      lock_id.table_id,
      lock_id.page_id,
      lock_id.index_id,
      lock_id.encoded_key,
      "",
      "",
  });
}

std::optional<txn_id_t> LockManager::FindDeadlockVictimUnlocked() const {
  // 等待图邻接表：graph[A] 中的 B 表示事务 A 正在等待事务 B。
  std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> graph;
  // 从每个资源队列中，把等待请求指向阻塞它的持锁者或前序冲突等待者。
  for (const auto &entry : queues_) {
    const auto &requests = entry.second.requests;
    for (auto request = requests.begin(); request != requests.end(); ++request) {
      if (!IsWaiting(*request) || request->transaction == nullptr ||
          !request->transaction->IsUsableForWork()) {
        continue;
      }
      auto &edges = graph[request->txn_id];
      for (auto other = requests.begin(); other != requests.end(); ++other) {
        if (other == request || other->txn_id == request->txn_id) continue;
        if (other->granted && RequestConflicts(*request, *other)) {
          // 已授予且模式冲突：当前事务必须等待这个持锁事务。
          edges.insert(other->txn_id);
        } else if (!other->granted && RequestConflicts(*request, *other) &&
                   std::distance(requests.begin(), other) <
                       std::distance(requests.begin(), request)) {
          edges.insert(other->txn_id);
        }
      }
    }
  }

  // DFS 三色标记：0=未访问，1=当前递归路径，2=已完成且无环。
  std::unordered_map<txn_id_t, int> color;
  std::vector<txn_id_t> path;
  std::optional<txn_id_t> victim;
  std::function<bool(txn_id_t)> visit = [&](txn_id_t node) {
    // 节点染灰并压入当前路径；再次遇到灰色节点就说明存在环。
    color[node] = 1;
    path.push_back(node);
    for (const auto next : graph[node]) {
      if (color[next] == 0 && visit(next)) return true;
      if (color[next] == 1) {
        const auto begin = std::find(path.begin(), path.end(), next);
        // 从环起点到当前节点都在死锁环中，选择最大的 txn_id 作为 victim。
        victim = *std::max_element(begin, path.end());
        return true;
      }
    }
    // 当前分支没有发现环，退出递归路径并染黑。
    path.pop_back();
    color[node] = 2;
    return false;
  };
  for (const auto &entry : graph) {
    if (color[entry.first] == 0 && visit(entry.first)) return victim;
  }
  return std::nullopt;
}

std::vector<txn_id_t> LockManager::DetectDeadlocks() {
  // 构造等待图期间锁住全部请求队列，得到一致快照。
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<txn_id_t> victims;
  const auto victim = FindDeadlockVictimUnlocked();
  if (!victim.has_value()) return victims;
  const auto transaction = transactions_.find(victim.value());
  if (transaction != transactions_.end() && transaction->second != nullptr) {
    // 这里只标记 victim 为 Failed；WAL Undo 和真正释放锁由 TransactionManager 完成。
    transaction->second->MarkFailed();
    victims.push_back(victim.value());
  }
  // 唤醒所有资源等待者，使 victim 的等待调用及时观察 Failed 状态并退出。
  for (auto &entry : queues_) entry.second.cv.notify_all();
  return victims;
}

std::vector<LockId> LockManager::GetGrantedLocks(txn_id_t txn_id) const {
  // 调用者：诊断/测试；作用：复制事务当前已授予的资源锁；返回：锁地址列表。
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LockId> result;
  for (const auto &entry : queues_) {
    for (const auto &request : entry.second.requests) {
      if (request.txn_id == txn_id && request.granted && !request.upgrading) {
        result.push_back(entry.first);
      }
    }
  }
  return result;
}

std::size_t LockManager::GetWaitingRequestCount() const {
  // 调用者：诊断/测试；作用：统计等待或升级中的请求；返回：请求数量。
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0;
  for (const auto &entry : queues_) {
    for (const auto &request : entry.second.requests) {
      if (IsWaiting(request)) ++count;
    }
  }
  return count;
}

}  // namespace oursql
