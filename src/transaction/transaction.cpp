#include "oursql/transaction/transaction.h"

namespace oursql {

txn_id_t Transaction::id() const noexcept {
  // 调用者：事务表/日志/锁管理器；作用：读取稳定事务编号；返回：txn_id。
  // 即使是只读访问也加锁，避免与事务状态/WAL 记录更新形成数据竞争。
  std::lock_guard<std::mutex> lock(mutex_);
  return id_;
}

TransactionState Transaction::state() const noexcept {
  // 调用者：提交、回滚和锁等待路径；作用：读取事务状态；返回：TransactionState。
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

lsn_t Transaction::last_lsn() const noexcept {
  // 调用者：AppendFor/Undo；作用：读取事务日志链尾；返回：LSN。
  std::lock_guard<std::mutex> lock(mutex_);
  return last_lsn_;
}

lsn_t Transaction::commit_lsn() const noexcept {
  // 调用者：CommitUncertain 重试/恢复；作用：读取 COMMIT LSN；返回：LSN。
  std::lock_guard<std::mutex> lock(mutex_);
  return commit_lsn_;
}

bool Transaction::explicit_transaction() const noexcept {
  // 调用者：会话协调器；作用：判断是否为显式事务；返回：布尔值。
  std::lock_guard<std::mutex> lock(mutex_);
  return explicit_transaction_;
}

bool Transaction::IsUsableForWork() const noexcept {
  // 调用者：锁请求和页面访问；作用：判断事务是否仍可执行工作；返回：Active 才为 true。
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == TransactionState::Active;
}

std::uint32_t Transaction::active_write_guards() const noexcept {
  // 调用者：Commit/Abort；作用：检查是否仍有写页面 guard；返回：数量。
  std::lock_guard<std::mutex> lock(mutex_);
  return active_write_guards_;
}

void Transaction::MarkFailed() noexcept {
  // 调用者：死锁检测或 I/O 错误；作用：把 Active 事务标为 Failed；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == TransactionState::Active) state_ = TransactionState::Failed;
}

bool Transaction::TrySetState(TransactionState expected,
                              TransactionState desired) noexcept {
  // “检查旧状态并写入新状态”在同一个临界区完成，防止两个线程同时提交或回滚。
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != expected) return false;
  state_ = desired;
  return true;
}

std::vector<page_id_t> Transaction::allocated_pages() const {
  // 调用者：兼容旧接口；作用：返回本事务分配页清单副本；返回：page_id 列表。
  return AllocatedPagesSnapshot();
}

std::vector<page_id_t> Transaction::deferred_free_pages() const {
  // 调用者：兼容旧接口；作用：返回提交后待释放页清单副本；返回：page_id 列表。
  return DeferredFreePagesSnapshot();
}

std::vector<page_id_t> Transaction::AllocatedPagesSnapshot() const {
  // 调用者：Undo/提交清理；作用：读取分配页清单快照；返回：副本。
  std::lock_guard<std::mutex> lock(mutex_);
  return allocated_pages_;
}

std::vector<page_id_t> Transaction::DeferredFreePagesSnapshot() const {
  // 调用者：提交清理；作用：读取延迟释放页清单快照；返回：副本。
  std::lock_guard<std::mutex> lock(mutex_);
  return deferred_free_pages_;
}

bool Transaction::HasDeferredFreePage(page_id_t page_id) const noexcept {
  // 调用者：删除/WAL 防重复逻辑；作用：查询页面是否已经登记待释放；返回：布尔值。
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto value : deferred_free_pages_) {
    if (value == page_id) return true;
  }
  return false;
}

std::vector<LockId> Transaction::GrantedLocksSnapshot() const {
  // 返回副本而不是暴露内部集合；函数返回后无需继续持有事务 mutex_。
  std::lock_guard<std::mutex> lock(mutex_);
  return {granted_locks_.begin(), granted_locks_.end()};
}

void Transaction::SetState(TransactionState state) noexcept {
  // 调用者：事务管理器；作用：在锁保护下写入新的生命周期状态；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

void Transaction::SetLastLsn(lsn_t lsn) noexcept {
  // 调用者：AppendFor；作用：更新事务日志链尾；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  last_lsn_ = lsn;
}

void Transaction::SetCommitLsn(lsn_t lsn) noexcept {
  // 调用者：Commit；作用：保存 COMMIT 日志的 LSN；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  commit_lsn_ = lsn;
}

void Transaction::AddAllocatedPage(page_id_t page_id) {
  // 调用者：事务新页逻辑；作用：登记本事务分配的页面；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  allocated_pages_.push_back(page_id);
}

void Transaction::AddDeferredFreePage(page_id_t page_id) {
  // 调用者：PageFree WAL；作用：登记提交后才能回收的页面；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  deferred_free_pages_.push_back(page_id);
}

void Transaction::AddGrantedLock(const LockId &lock_id) {
  // LockManager 授予资源锁后，在事务对象中登记，供提交/回滚时统一释放。
  std::lock_guard<std::mutex> lock(mutex_);
  granted_locks_.insert(lock_id);
}

void Transaction::RemoveGrantedLock(const LockId &lock_id) {
  // 调用者：LockManager::Unlock；作用：从事务清单移除一个已释放锁；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  granted_locks_.erase(lock_id);
}

void Transaction::ClearGrantedLocks() {
  // UnlockAll 清理完所有资源队列后，同步清空事务自己的锁清单。
  std::lock_guard<std::mutex> lock(mutex_);
  granted_locks_.clear();
}

void Transaction::AddActiveWriteGuard() noexcept {
  // 调用者：WritePageGuard 构造；作用：增加未释放写 guard 计数；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  ++active_write_guards_;
}

void Transaction::ReleaseActiveWriteGuard() noexcept {
  // 调用者：WritePageGuard Reset；作用：减少未释放写 guard 计数；返回：无。
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_write_guards_ != 0) --active_write_guards_;
}
}
