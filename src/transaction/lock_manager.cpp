#include "oursql/transaction/lock_manager.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>

namespace oursql {

namespace {

Status ValidateTransaction(Transaction *transaction) {
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
  return LockInternal(transaction, LockId::Schema(), mode);
}

Status LockManager::LockTable(Transaction *transaction, table_id_t table_id,
                              TableLockMode mode) {
  return LockInternal(transaction, LockId::Table(table_id), mode);
}

Status LockManager::LockPage(Transaction *transaction, page_id_t page_id,
                             LockMode mode) {
  if (page_id == INVALID_PAGE_ID || page_id == 0) {
    return Status::InvalidArgument("page lock requires a valid page id");
  }
  return LockInternal(transaction, LockId::Page(page_id), mode);
}

Status LockManager::LockIndexKey(Transaction *transaction, index_id_t index_id,
                                 const std::string &encoded_key, LockMode mode,
                                 table_id_t table_id) {
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

  std::unique_lock<std::mutex> lock(mutex_);
  transactions_[transaction->id()] = transaction;
  auto &queue = queues_[lock_id];
  auto request = std::find_if(queue.requests.begin(), queue.requests.end(),
                              [&](const LockRequest &entry) {
                                return entry.txn_id == transaction->id();
                              });
  if (request != queue.requests.end()) {
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
    queue.requests.push_back(
        LockRequest{transaction->id(), transaction, true, mode, LockMode::S, false, false});
    request = std::prev(queue.requests.end());
  }

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
      queue.cv.notify_all();
      return Status::Ok();
    }
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
      queue.cv.notify_all();
      return Status::Ok();
    }
    queue.cv.wait(lock);
  }
}

bool LockManager::IsTableResource(LockResourceType type) noexcept {
  return type == LockResourceType::Schema || type == LockResourceType::Table;
}

bool LockManager::TableModesConflict(TableLockMode left,
                                     TableLockMode right) noexcept {
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
  return left == LockMode::X || right == LockMode::X;
}

bool LockManager::CanUpgrade(TableLockMode current,
                             TableLockMode requested) noexcept {
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
  return current == LockMode::S && requested == LockMode::X;
}

bool LockManager::SameRequestedMode(const LockRequest &request,
                                    TableLockMode mode) noexcept {
  return request.table_mode && request.table_lock_mode == mode;
}

bool LockManager::SameRequestedMode(const LockRequest &request,
                                    LockMode mode) noexcept {
  return !request.table_mode && request.lock_mode == mode;
}

bool LockManager::RequestConflicts(const LockRequest &request,
                                   const LockRequest &other) const noexcept {
  if (request.table_mode != other.table_mode) return true;
  if (request.table_mode) {
    return TableModesConflict(request.table_lock_mode, other.table_lock_mode);
  }
  return LockModesConflict(request.lock_mode, other.lock_mode);
}

bool LockManager::IsWaiting(const LockRequest &request) const noexcept {
  return !request.cancelled && (!request.granted || request.upgrading);
}

bool LockManager::CanGrant(const LockRequestQueue &queue,
                           ConstRequestIterator request) const noexcept {
  for (auto it = queue.requests.begin(); it != request; ++it) {
    if (it->txn_id != request->txn_id && IsWaiting(*it)) return false;
  }
  for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
    if (it == request || it->txn_id == request->txn_id || !it->granted) continue;
    if (RequestConflicts(*request, *it)) return false;
  }
  return true;
}

Status LockManager::Unlock(Transaction *transaction, const LockId &lock_id) {
  if (transaction == nullptr) return Status::InvalidArgument("unlock requires a Transaction");
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
    queue.requests.erase(request);
  }
  transaction->RemoveGrantedLock(lock_id);
  queue.cv.notify_all();
  if (queue.requests.empty()) queues_.erase(queue_it);
  return Status::Ok();
}

Status LockManager::UnlockAll(Transaction *transaction) {
  if (transaction == nullptr) return Status::InvalidArgument("unlock all requires a Transaction");
  const auto txn_id = transaction->id();
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

std::optional<txn_id_t> LockManager::FindDeadlockVictimUnlocked() const {
  std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> graph;
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
          edges.insert(other->txn_id);
        } else if (!other->granted && RequestConflicts(*request, *other) &&
                   std::distance(requests.begin(), other) <
                       std::distance(requests.begin(), request)) {
          edges.insert(other->txn_id);
        }
      }
    }
  }

  std::unordered_map<txn_id_t, int> color;
  std::vector<txn_id_t> path;
  std::optional<txn_id_t> victim;
  std::function<bool(txn_id_t)> visit = [&](txn_id_t node) {
    color[node] = 1;
    path.push_back(node);
    for (const auto next : graph[node]) {
      if (color[next] == 0 && visit(next)) return true;
      if (color[next] == 1) {
        const auto begin = std::find(path.begin(), path.end(), next);
        victim = *std::max_element(begin, path.end());
        return true;
      }
    }
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
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<txn_id_t> victims;
  const auto victim = FindDeadlockVictimUnlocked();
  if (!victim.has_value()) return victims;
  const auto transaction = transactions_.find(victim.value());
  if (transaction != transactions_.end() && transaction->second != nullptr) {
    transaction->second->MarkFailed();
    victims.push_back(victim.value());
  }
  for (auto &entry : queues_) entry.second.cv.notify_all();
  return victims;
}

std::vector<LockId> LockManager::GetGrantedLocks(txn_id_t txn_id) const {
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
