# OurSQL C 组存储与事务答辩说明

本文只按照当前仓库已经存在的代码整理，重点覆盖操作系统与存储管理部分。答辩时应把“已经实现的行为”和“以后可以扩展的方向”分开，不要把未接入的功能说成已经完成。

## 1. 先记住总链路

OurSQL 使用一个 `.oursql` 文件保存数据库页面。程序启动后，`DiskManager` 打开文件，`BufferPoolManager` 在进程内存中维护固定数量的 Frame。上层通过 `PageGuard` 访问页面，`SlottedPage` 在页面内管理变长记录，`HeapTable` 管理一张表的数据页链，`Catalog` 保存表的元数据。

一条查询的存储路径可以概括为：

```text
SQL
  -> Parser / Planner 生成 Plan
  -> DatabaseEngine 获取事务并申请锁
  -> Executor
  -> HeapTable
  -> BufferPoolManager
  -> PageGuard
  -> DiskManager
  -> database.oursql
```

其中，SQL 前端决定“要做什么”，执行器决定“调用哪一个表操作”，存储层决定“页面如何在内存和文件之间移动”。C 组主要负责最后两部分，以及事务、日志和并发配合。

## 2. 文件位置速查

| 功能 | 主要文件 | 答辩时要说的核心对象 |
| --- | --- | --- |
| 页面、磁盘、BufferPool、置换、Guard、SlottedPage | `include/oursql/storage/storage.h`、`src/storage/storage.cpp` | `DiskManager`、`BufferPoolManager`、`FIFOReplacer`、`LRUReplacer`、`ReadPageGuard`、`WritePageGuard`、`SlottedPage` |
| 行编码与堆表 | `include/oursql/storage/row_codec.h`、`src/storage/row_codec.cpp`、`include/oursql/storage/heap_table.h`、`src/storage/heap_table.cpp` | `RowCodec`、`HeapTable`、`ScanCursor` |
| 日志 | `include/oursql/storage/log_manager.h`、`src/storage/log_manager.cpp` | `LogManager`、`LogRecord`、LSN |
| 事务 | `include/oursql/transaction/transaction.h`、`src/transaction/transaction.cpp` | `Transaction`、事务状态、事务日志链 |
| 锁 | `include/oursql/transaction/lock_manager.h`、`src/transaction/lock_manager.cpp` | 表锁、页面锁、索引键锁、等待队列、死锁检测 |
| 事务协调 | `include/oursql/transaction/transaction_manager.h`、`src/transaction/transaction_manager.cpp` | Begin、Commit、Abort、Undo、自动死锁检测 |
| 恢复 | `include/oursql/recovery/recovery_manager.h`、`src/recovery/recovery_manager.cpp` | Redo、Undo、CLR、恢复后的检查点 |
| 引擎编排 | `include/oursql/execution/database_engine.h`、`src/execution/database_engine.cpp` | 打开顺序、锁、事务、关闭顺序 |
| 主要测试 | `tests/storage_disk_manager_tests.cpp`、`tests/storage_contract_tests.cpp`、`tests/slotted_page_tests.cpp`、`tests/catalog_heap_tests.cpp`、`tests/transaction_tests.cpp`、`tests/concurrent_transaction_tests.cpp` | 页面、缓存、持久化、WAL、并发和死锁证据 |

## 3. `.oursql` 文件和程序内存不是一回事

### 3.1 数据库文件

`.oursql` 是磁盘上的持久化文件。文件由固定大小的 4096 字节 Page 组成：

```text
Page 0       Superblock
后续页面      Catalog 页面、表数据页面、空闲页面
```

Page 0 保存文件格式、页面大小、页面数量、空闲页链表头以及 Catalog 入口等元数据。普通页面的分配和释放由 `DiskManager` 完成。

### 3.2 程序内存

`BufferPoolManager` 的 `frames_` 是程序运行期间的内存缓存。一个 Frame 可以暂时装载一个磁盘 Page，但 Frame 不是数据库文件中的另一个区域，也不会在程序退出后直接保留。

因此要区分：

```text
page_id       磁盘页面的逻辑编号
frame_id      内存缓存槽位的编号
Page          页面内容和页面状态
Frame         BufferPool 中承载 Page 的内存槽位
```

同一个 `page_id` 在不同时间可以被装入不同的 `frame_id`。BufferPool 的 PageTable 记录当前映射关系。Catalog Page 和数据 Page 都可以被 BufferPool 缓存，但它们不会永久各占一个 Frame；容量不够时，未 pin 的页面可以被替换。

## 4. DiskManager：文件最底层

实现位置：`include/oursql/storage/storage.h` 和 `src/storage/storage.cpp` 的 `DiskManager`。

### 4.1 打开与关闭

`Open()` 打开或创建数据库文件，并读取或初始化 Superblock。`Close()` 在关闭前处理必要的元数据写回并关闭文件资源。文件句柄由 DiskManager 持有，其他数据库层不应直接使用 `fstream` 或底层文件 API。

答辩表述：

> DiskManager 是唯一直接操作数据库文件的组件。BufferPool 只向它请求读页、写页、分配页和释放页，因此数据库层不会绕过缓存直接读文件。

### 4.2 读写页面

`ReadPage(page_id)` 根据 `page_id * Page::kSize` 定位，读取完整的 4096 字节页面。不存在的页面、非法编号、短读和 I/O 错误都应该返回错误状态。`WritePage(page_id, page)` 写入准确偏移，并检查写入是否成功。

这里不能用“读不到就自动补零”掩盖错误，因为那会把损坏或越界访问伪装成一个有效页面。

### 4.3 分配和释放

`AllocatePage()` 优先从空闲页链表中取页面；没有空闲页时，增加 Superblock 中的 page_count 并在文件末尾增加新页。`DeallocatePage()` 把页面放回空闲链表，并检查重复释放和非法页面。

页面分配流程是：

```text
读取 Superblock
  -> free_list_head_ 有效？
       是：取链表头，并读取下一个空闲页
       否：使用 page_count_ 作为新 page_id
  -> 更新 page_count_ 或 free_list_head_
  -> 写回 Superblock
```

## 5. BufferPoolManager：磁盘页面的内存缓存

实现位置仍然是 `storage.h` 和 `storage.cpp` 中的 `BufferPoolManager`。

### 5.1 FetchPage

`FetchPage(page_id)` 的逻辑是：

1. 检查 BufferPool 是否已经关闭、page_id 是否有效。
2. 在 PageTable 中查找 page_id。
3. 找到时，增加对应 Frame 的 pin_count，记录一次 hit。
4. 找不到时，记录一次 miss。
5. 从 free_frames_ 取空 Frame，或者向 Replacer 请求一个可淘汰 Frame。
6. 如果被淘汰页面是 dirty，先按照刷盘策略写回。
7. 调用 `DiskManager::ReadPage()` 读入新页面。
8. 更新 page table 和 Frame 状态，返回 Guard。

### 5.2 NewPage

`NewPage()` 先向 DiskManager 分配一个新的 page_id，再把新页面放入一个可用 Frame。它返回 `WritePageGuard`，调用者可以初始化页面并显式标记脏页。分配成功但装入内存失败时，必须处理回滚，不能遗留一个无法访问的页面。

### 5.3 Unpin、Flush 和 Close

`UnpinPage(page_id, is_dirty)` 减少 pin_count。`is_dirty == true` 时只会把 dirty 状态置为脏，不会因为之后某次 Unpin 传入 false 就清掉已经存在的脏状态。

`FlushPage()` 把指定脏页写回 DiskManager，成功后清除 dirty。`FlushAllPages()` 遍历所有页面刷盘。BufferPool 的关闭路径也会刷回脏页，并且会报告失败。

### 5.4 pin 和 dirty 的区别

这两个字段解决不同问题：

```text
pin_count > 0   表示当前有人正在使用，不能被淘汰
dirty == true   表示内存内容比磁盘新，淘汰前必须写回
```

一个页面可以是“没有被使用但很脏”，也可以是“正在使用但不脏”。

## 6. Replacer、FIFO 和 LRU

`Replacer` 只管理“pin_count 为 0 的 Frame”。它不负责读取页面，也不负责判断页面内容。

### 6.1 Pin

页面被 Fetch 或创建后，如果调用者还在使用，BufferPool 会让对应 Frame 进入 pinned 状态。Replacer 的 `Pin(frame_id)` 会把这个 Frame 从可淘汰候选集合中移除。

### 6.2 Unpin

当 Guard 释放且 pin_count 降到 0 时，BufferPool 调用 Replacer 的 `Unpin(frame_id)`。这时 Frame 才重新进入候选集合。

### 6.3 FIFOReplacer

FIFO 按进入候选集合的先后顺序保存 Frame。`Victim()` 从最早进入候选集合的 Frame 开始尝试淘汰。Frame 被再次 pin 后会离开候选集合；之后再次 Unpin 时，它会按照当前时间重新进入 FIFO 队列。

### 6.4 LRUReplacer

LRU 保存可淘汰 Frame 的最近使用顺序。一次新的 Fetch 命中或 Unpin 会更新顺序，`Victim()` 选择最久没有使用的 Frame。

答辩中要特别说明：

> FIFO 和 LRU 选择的是 Frame，不是直接选择 page_id。因为替换发生在内存缓存槽位上。Frame 被替换后，PageTable 会删除旧 page_id 的映射并建立新映射，所以旧页面不会因为 Replacer 还保存一个数字而被误认为仍在内存中。

### 6.5 mutex 与置换的关系

`mutex_` 主要保护 BufferPool 的 Frame、PageTable、free list、pin_count 和 dirty 状态。Replacer 自己也有互斥保护，用于保护候选集合。

锁不是定时检查机制。Pin 和 Unpin 都是在 Fetch、Guard 构造、Guard 释放等事件发生时同步触发。Replacer 不会每隔一段时间扫描所有页面。

Page 的读写锁保护页面内容；FIFO/LRU 的互斥锁保护候选集合；BufferPool 的互斥锁保护缓存管理状态。这三者作用不同。

## 7. PageGuard：让页面生命周期自动结束

实现位置：`ReadPageGuard` 和 `WritePageGuard`，声明在 `storage.h`，实现位于 `storage.cpp`。

两个 Guard 都禁止复制，只允许移动。这样同一个 Guard 的 Unpin 和解锁责任不会被复制成两份。

### ReadPageGuard

读 Guard 持有共享读锁。多个读者可以同时读同一页面，但 Guard 销毁时会释放读锁并 Unpin。

### WritePageGuard

写 Guard 持有独占写锁。它可以修改页面，并通过 `MarkDirty()` 表示页面内容需要写回。销毁时释放写锁并 Unpin。

答辩可以用下面一句话：

> PageGuard 把“加 pin、加锁、访问页面、解锁、减 pin”绑定为一个 RAII 对象，避免业务代码在异常路径或提前 return 路径忘记 Unpin。

## 8. SlottedPage：一个页面里如何放很多变长记录

实现位置：`SlottedPage`，位于 `storage.h` 和 `storage.cpp`。

页面布局大致是：

```text
页面起始
  PageHeader: page_type、slot_count、free_start、free_end、next_page_id
  Slot 0: offset、length、state
  Slot 1: offset、length、state
  ...
  空闲区域
  ...
  记录数据区，从页面尾部向前增长
页面末尾
```

目录从前向后增长，记录数据从后向前增长。Slot 中保存的是记录位置和长度，不保存“这是学生还是教师”这样的业务含义。记录本身按照表 Schema 由 RowCodec 解码，RID 中的 page_id 和 slot_id 才能定位一条记录。

### 8.1 InsertRecord

`InsertRecord(WritePageGuard &, bytes)` 先验证页面布局，再检查新 Slot 和记录数据是否都放得下。空间不足时返回明确错误；如果是删除后形成的碎片导致连续空间不足，会先 Compact，再重试一次。

### 8.2 DeleteRecord

删除不会改变其他有效记录的 slot_id。对应 Slot 被标记为 deleted，后续 `GetRecord()` 不会返回旧数据。插入时可以重新利用已经删除的 Slot。

### 8.3 Compact

Compact 只搬移有效记录的字节数据和 Slot 中的 offset，不重新编号 Slot。因此有效 RID 的 slot_id 保持不变。Compact 后空闲空间变成一段连续区域。

所有多字节整数都通过显式小端编码和解码处理，不把可能包含 padding 的 C++ 结构体直接 memcpy 到磁盘。

## 9. RowCodec、Schema、Catalog 和 HeapTable 的关系

### 9.1 Schema

Schema 描述一张表有哪些列、每列是什么类型以及 VARCHAR 长度限制。它回答的是“这一行应该长什么样”。

### 9.2 RowCodec

RowCodec 把逻辑上的 `vector<Value>` 转换成页面可以保存的一段字节：

```text
Values:  1, "Alice"
   -> RowCodec::Encode
bytes:   可持久化的二进制记录
   -> SlottedPage::InsertRecord
```

读取时反过来：

```text
SlottedPage::GetRecord
   -> bytes
   -> RowCodec::Decode(schema, bytes)
   -> Values
```

RowCodec 会检查列数量、类型、字符串长度和记录边界。它不决定记录放在哪个页面，也不负责 SQL 查询。

### 9.3 Catalog

Catalog 是表的登记册，保存表名、Schema、首数据页等元数据。它通过 Superblock 的 catalog_head 找到持久化的 Catalog 页面链表。Catalog 不是每张表的数据；它保存的是“表在哪里、表有哪些列”。

### 9.4 HeapTable

HeapTable 是表数据的管理者。它根据 TableMetadata 沿数据页链插入、读取、删除和扫描行。它不使用索引来定位行，基本扫描路径是顺序遍历页面链和 Slot 目录。

关系可以记成：

```text
Catalog
  -> TableMetadata
    -> Schema + first_data_page_id
      -> HeapTable 的数据页链
        -> SlottedPage 的 Slot
          -> RowCodec 保存的记录 bytes
            -> Values
```

`ScanCursor` 是惰性的，一次推进一条有效记录，不需要先把整张表复制到内存。

## 10. LogManager 和 WAL

实现位置：`include/oursql/storage/log_manager.h`、`src/storage/log_manager.cpp`。

`LogRecord` 保存事务号、LSN、前一条 LSN、页面编号以及 before image/after image 等字段。`LogManager` 负责 WAL 文件的打开、追加、刷盘、扫描和截断。

数据页写回前要满足：

```text
先 Flush WAL 到对应 LSN
再 WritePage 数据页
```

这样即使进程在数据页落盘后崩溃，恢复阶段仍能根据日志重建或撤销页面。

当前代码中还测试了 WAL 尾部修复、校验错误、断裂的 prev_lsn 链、页面更新、页面分配和页面释放等情况。答辩时不要说它是完整商业数据库 WAL，而应说是当前教学项目实现的事务日志和恢复闭环。

## 11. Transaction：事务对象保存什么

实现位置：`transaction.h`、`transaction.cpp`。

`Transaction` 保存：

- `txn_id_t` 事务编号
- `TransactionState` 当前状态
- `last_lsn_` 事务自己的日志链尾部
- `commit_lsn_`
- 已分配页面
- 延迟释放页面
- 已获得的锁集合
- 活跃写 Guard 数量

它不保存整张表，也不复制数据库数据。事务对象保存的是事务状态和恢复所需的 bookkeeping。

常见状态包括 Active、Failed、Committing、Committed、Aborting 和 Aborted。死锁检测会先把 victim 标记为 Failed，TransactionManager 再执行 Undo、写 Abort 日志、释放锁并从活动事务表移除。

## 12. LockManager：锁保护什么

实现位置：`lock_manager.h`、`lock_manager.cpp`。

当前 LockManager 支持：

- Schema 锁
- Table 锁
- Page 锁
- IndexKey 锁

每个锁资源有一个请求队列。请求如果不能立即授予，就在条件变量上等待。队列中的请求会检查：

1. 当前事务是否仍然可工作。
2. 前面的等待请求是否阻塞了它。
3. 已授予的冲突锁是否存在。
4. 请求是否被 Abort 或 UnlockAll 标记为取消。

这里要区分锁和 pin：

```text
LockManager: 防止事务之间产生不正确的并发访问
pin_count:   防止 BufferPool 淘汰仍被某个调用者使用的 Frame
PageGuard:   在一次页面访问期间持有页面锁并管理 pin 生命周期
```

它们可以同时存在，但不是同一个概念。

## 13. TransactionManager 和自动死锁检测

实现位置：`transaction_manager.h`、`transaction_manager.cpp`。

TransactionManager 负责 Begin、BeginAutocommit、Commit、Abort、Undo 和活动事务管理。`ResolveDeadlocksOnce()` 调用已有的 `LockManager::DetectDeadlocks()`，取回 victim 后调用正常的 Abort 路径，而不是单独写一套“释放锁”的逻辑。

自动检测部分包含：

- `detector_thread_`
- `detector_cv_`
- `detector_stop_requested_`
- `StartDeadlockDetector()`
- `StopDeadlockDetector()`
- `DeadlockDetectorLoop()`

后台线程每 100ms 使用 `condition_variable::wait_for()` 等待。Stop 时设置退出标志、`notify_all()` 并 `join()`。没有使用 `detach`，所以 TransactionManager 析构时不会留下悬空线程。

重复 Start 会检查 `detector_thread_.joinable()`，不会再创建第二个线程；重复 Stop 在没有线程时直接返回成功。

## 14. RecoveryManager：重启时发生什么

启动顺序由 `DatabaseEngine` 编排：

1. 初始化 BufferPool。
2. 打开 DiskManager。
3. 打开 LogManager。
4. 调用 RecoveryManager 恢复 WAL。
5. 获取下一个事务号种子。
6. 做检查点。
7. 绑定 LogManager 和 LockManager 到 BufferPool。
8. 打开 Catalog。
9. 启动死锁检测线程。

恢复大致分为：

- 扫描 WAL，识别事务和日志链。
- 对已提交操作做 Redo。
- 对未完成事务做 Undo。
- 用 Compensation Log Record 记录撤销动作。
- 写入 Abort 记录。

关闭顺序相反，先停止死锁检测线程，再处理事务、检查点、刷新 BufferPool，最后关闭日志和数据库文件。这样后台线程不会在底层对象已经销毁后继续访问它们。

## 15. 关闭和异常路径为什么重要

正常路径是：

```text
DatabaseEngine::Close
  -> StopDeadlockDetector
  -> 结束会话事务
  -> Checkpoint
  -> BufferPool::Close / FlushAllPages
  -> LogManager::Close
  -> DiskManager::Close
```

所有公开接口都使用 `Status` 或 `Result<T>` 表示错误。答辩时可以举这些错误例子：

- 非法 page_id
- 读取不存在页面
- 所有 Frame 都被 pin
- dirty 页刷盘失败
- WAL 没有先刷盘
- LockManager 发现事务已经 Abort
- SlottedPage 的 offset/length 越界
- RowCodec 类型或列数量不匹配

## 16. 测试怎么对应代码

### 页面和 BufferPool

`tests/storage_disk_manager_tests.cpp` 覆盖数据库文件、页面分配释放、Superblock、FIFO/LRU、命中与未命中、脏页刷盘、页面 Guard 和并发访问等行为。

`tests/storage_contract_tests.cpp` 检查存储层接口契约，例如页面生命周期、写回、删除和关闭路径。

`tests/slotted_page_tests.cpp` 检查不同长度记录、删除、Compact、非法元数据和重启读取。

### Catalog 和 HeapTable

`tests/catalog_heap_tests.cpp` 检查：

- INT/VARCHAR 行编解码
- 创建表和重复建表
- Catalog 重启恢复
- 跨多个数据页插入
- Scan 数量和内容
- 删除后跳过无效 Slot
- 重启后的跨页扫描

### WAL、事务和恢复

`tests/transaction_tests.cpp` 检查日志追加、WAL 刷盘、页面更新、页面分配释放、Undo、Redo、CLR、恢复、提交不确定和故障注入路径。

### 并发事务和死锁

`tests/concurrent_transaction_tests.cpp` 检查并发 Begin、锁兼容性、锁升级、手动死锁解决、自动死锁检测、事务 Abort、锁释放、会话并发和重复 Close。

自动死锁测试的答辩说法：

> T1 持有 Page 31 并等待 Page 32，T2 持有 Page 32 并等待 Page 31。测试不手动调用 ResolveDeadlocksOnce，而是等待后台线程检测。检测器选择事务号较大的 T2，Abort 并释放它的锁，T1 随后继续完成。

## 17. 当前代码不能夸大的地方

答辩时可以明确说：

- 当前不是完整 SQL 数据库，不应声称支持所有 SQL。
- 当前不是完整商业级 WAL/ARIES 实现。
- 没有把事务、MVCC、网络数据库和 GUI 说成 C 组存储层已经实现。
- BufferPool 的置换对象是 Frame，不是直接对磁盘 page_id 做替换。
- Catalog 是表结构和入口元数据，不是把所有行都复制到内存。
- `.oursql` 是数据库文件；内存缓存是 BufferPool 的 Frame，两者不能混为一谈。

## 18. 一段可以直接背的总结

> OurSQL 把数据库文件划分为固定 4096 字节页面。DiskManager 是唯一直接访问文件的模块，负责页面分配、释放、读取、写入以及 Superblock 和空闲页链表。BufferPoolManager 在内存中维护固定数量的 Frame，通过 PageTable 找页面，通过 FIFO 或 LRU 选择未 pin 的 Frame 淘汰。页面访问使用只能移动的 ReadPageGuard 和 WritePageGuard，Guard 负责锁和 Unpin，写 Guard 负责标记 dirty。SlottedPage 在页面中用 Slot 目录管理变长记录，RowCodec 负责 Schema 和 Values 与二进制记录之间的转换，HeapTable 沿数据页链管理一张表，Catalog 保存表结构和数据页入口。事务修改先写 WAL，脏页写回前先保证日志落盘；TransactionManager 负责提交、撤销和恢复配合，LockManager 负责并发锁，后台线程定期检测死锁并通过正常 Abort 路径释放 victim。这样 SQL 最终通过执行器进入 HeapTable，再经过 BufferPool 和 DiskManager 持久化到 `.oursql` 文件。

