# C 侧存储与索引接入说明

本文记录 B+ 树接入后，存储模块已经提供的页面释放和统计接口，以及 A/B 后续接线时必须遵守的调用顺序。

## 已稳定接口

### `BufferPoolManager::DeletePage(page_id)`

- 页面仍被任意 Guard pin 时返回 `InvalidArgument`，错误包含“被 pin 的页面不能删除”。
- 失败时不删除 Page Table 映射、不清空 Frame、不调用 `DiskManager::DeallocatePage()`。
- Guard 释放后可以重试；成功释放的磁盘页进入持久化 Free List，重启后仍由 `NewPage()` 优先复用。

### `HeapTable::Destroy()`

- 只负责释放当前 HeapTable 副本指向的 SlottedPage 数据页链。
- 第一阶段通过 `FetchPage()` 读取并校验完整链，检测非法 Page ID、页面损坏和环；验证失败时不删除任何页。
- 第二阶段释放全部读 Guard，再调用 `DeletePages()` 批量释放。
- `DeletePages()` 会在真正删除前一次性检查所有 page ID、重复项和 pin 状态；任意页面被 pin 时不会先释放链中的其他页面。
- 全部成功后，该 HeapTable 对象的 `first_data_page_id` 副本变为 `INVALID_PAGE_ID`，对象不能继续扫描、插入或删除。
- 它不删除 Catalog 表元数据，不删除索引，也不解析或执行 `DROP TABLE`。

当前没有事务和 WAL。如果多页释放阶段发生 I/O 错误，可能已经释放部分页面；后续需要事务/WAL 才能保证整条 DDL 原子性。

### `DatabaseEngine::ResetStatistics()`

该接口同时调用：

```cpp
buffer_pool_.ResetStatistics();
disk_manager_.ResetIoStatistics();
```

它只清零累计计数，不清除 BufferPool 页面，不改变 Page Table、pin、dirty 或 FIFO/LRU 状态。CLI 对应命令为：

```text
.stats
.stats reset
```

## DROP TABLE 接线顺序

完整 `DROP TABLE` 已在执行层接通，执行顺序固定为：

```text
查找该表的全部索引
  -> 逐个调用 IndexManager::DropIndex()
  -> 调用 HeapTable::Destroy() 释放数据页链
  -> 最后删除 Catalog 中的表元数据
```

Catalog 元数据不能先删，否则索引和数据页入口会丢失。当前任一步失败都应立即向上返回原始存储错误，不得假装 DROP 成功。

## SeqScan 与 IndexScan 性能测试

当前 IndexScan 已经接通：Optimizer 选择 `IndexScanPlan` 后，由 `IndexManager::Lookup()` 得到 RID，再通过 `HeapTable::GetRow()` 读取行。做性能比较前，必须先验证 IndexScan 与 SeqScan 在相同谓词下返回相同记录集合，再进行冷缓存和热缓存实验。

### 冷缓存实验

每轮销毁并重新创建 `DatabaseEngine`，打开同一个数据库文件。SeqScan 和 IndexScan 使用相同的数据、谓词、BufferPool 容量和替换策略，分别记录统计和耗时。仅调用 `ResetStatistics()` 不能制造冷缓存。

### 热缓存实验

先执行一次相同查询预热页面，再调用 `ResetStatistics()` 清零计数，随后执行第二次查询并记录结果。该实验保留预热后的缓存状态。

两套实验统一记录：

- 查询方式
- Buffer accesses、hits、misses、hit rate
- evictions
- disk reads、disk writes
- 查询耗时
- 返回行数

只有查询条件、数据集、BufferPool 大小、替换策略和结果正确性完全一致时，性能数据才可比较。

## 当前边界

- B+ 树节点布局、分裂、合并、借位、查询算法未由 C 侧修改。
- Parser、Planner、Executor 的 SQL 语义未由 C 侧扩张。
- 当前不提供事务、WAL、后台刷脏或完整 DROP TABLE 原子性。

## C 侧存储补充说明

### 页面替换与可观测性

`BufferPoolManager` 当前支持 `FIFO`、`LRU` 和 `CLOCK` 三种策略。替换器始终返回
`frame_id`，因为它管理的是内存槽位；`BufferPoolManager` 再通过该 frame 找到被替换的
`page_id`。CLOCK 为每个 frame 保留 reference bit：第一次遇到可淘汰 frame 时清零并给予
二次机会，下一轮仍未被访问才淘汰。`pin_count != 0` 的 frame 永远不会进入候选集合。

CLI 可以用下面的命令观察当前状态：

```text
oursql.exe --pool-size 3 --policy fifo demo.oursql
.buffer
.evictions
.stats
```

`.buffer` 显示 frame、page、pin、dirty 和 evictable；`.evictions` 显示实际被淘汰的
page ID，而不是 frame ID。淘汰日志最多保留最近 1024 条，日志只用于观测，不参与恢复。

`oursql_benchmark` 使用固定种子 `20260908`、相同的 4-frame 缓冲池和 WriteBack，分别测量
顺序、随机、热点三种请求模式下的 FIFO/LRU/CLOCK。每个冷缓存实验都会重新建立同样的
32 页 fixture；每个热缓存实验先用同一请求序列预热，再调用 `ResetStatistics()`，所以输出
中的 accesses/hits/misses/disk reads 是预热之后的数据。耗时只作为实测观察值，不作为单元
测试的固定断言。

### 空数据页回收

`HeapTable::DeleteRow()` 先把 slot 标为 deleted。若删除后目标页没有任何有效记录，且它不是
`TableMetadata.first_data_page_id`，则前驱页会跳过目标页，之后调用
`BufferPoolManager::DeletePage()` 将目标页放回 DiskManager 的 Free List。首页是表的稳定入口，
即使变空也不回收；这样 Catalog 中的入口不会失效。删除部分记录但仍有有效记录的页面也不会
回收，后续插入可以复用其中的 slot 和空间。

这段链表修改没有 WAL 或事务保护。如果在“前驱改链”和“释放目标页”之间发生进程崩溃，当前
版本不能保证操作原子提交；I/O 错误会向上返回，但可能需要人工检查数据库文件。该边界不应
包装成事务安全。

### WriteBack 与 WriteThrough

- `WriteBack` 是默认策略：修改先留在 frame 中，页面在淘汰、显式 `FlushPage`、
  `FlushAllPages` 或缓冲池关闭时写回。
- `WriteThrough` 使用同一配置接口：写 guard 标脏后，在 guard 显式 `Release()` 或普通
  `UnpinPage(page_id, true)` 时立即写回；成功后清除 dirty，失败则保留 dirty 并返回 I/O
  错误，后续 Flush/Close 可以重试。

WriteThrough 只描述页面写回时机，不提供 WAL、事务原子性或崩溃一致性。`MarkDirty()` 本身
先记录在 guard 内部，guard 释放前 frame 快照仍可能显示 clean，这是为了让 guard 在离开
作用域时一次性提交 pin 和 dirty 状态。

### 当前边界

- 文件 I/O 仍只允许出现在 `DiskManager`；HeapTable、Catalog、索引和执行层通过 Guard 与
  BufferPoolManager 访问页面。
- 当前没有事务、MVCC、WAL、后台刷脏线程、并发表级修改和完整 B+ 树算法扩展。
- 页级读写锁保护单个 frame 的字节访问；它与 `pin_count` 是两件事：锁防止同时读写，pin
  防止页面在使用期间被替换。
- BufferPool 的并发测试使用条件变量协调两个读 guard 和读写等待，不依赖随意 sleep；多个
  ReadPageGuard 可以共享读锁，WritePageGuard 获取独占锁，guard 释放后等待者继续执行。
