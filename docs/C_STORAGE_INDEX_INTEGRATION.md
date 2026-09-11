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

完整 `DROP TABLE` 仍由 A 侧负责语法、计划和执行编排，固定顺序应为：

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
- 当前不提供事务、WAL、后台刷脏、CLOCK、Write-Through 或完整 DROP TABLE 原子性。
