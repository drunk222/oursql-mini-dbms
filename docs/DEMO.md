# OurSQL 答辩演示手册

本文档对应当前仓库真实代码。构建目录假定为 `build`，数据库文件示例使用源码目录外的 `build/demo.oursql`。

## 3 分钟基础功能

在项目根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\oursql.exe .\build\demo.oursql
```

在 CLI 中逐行输入：

```sql
CREATE TABLE student(id INT, name VARCHAR);
INSERT INTO student VALUES(1, 'Alice');
INSERT INTO student VALUES(2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 2;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
.tables
.schema student
.stats
.flush
.quit
```

预期现象和证明：

| 步骤 | 预期看到 | 证明 |
| --- | --- | --- |
| CREATE | `OK` | Parser、Planner 和 Catalog 建表链路成功 |
| INSERT | RID 和 `Affected rows: 1` | RowCodec、HeapTable、BufferPool 成功写入 |
| `SELECT *` | `id | name` 以及 Alice、Bob | SeqScan 读取有效行并返回列名和值 |
| 指定列和 WHERE | 只显示 `Bob` | Project 和 Filter 算子按 Plan 执行 |
| DELETE | `Affected rows: 1` | 满足等值条件的行被删除 |
| 再次 SELECT | 只剩 Bob | 删除槽不会重新返回旧数据 |
| `.tables` | `student` | Catalog 表登记可查询 |
| `.schema student` | `id INT`、`name VARCHAR` | 持久化 Schema 可读取 |
| `.stats` | accesses、hits、misses、hit rate、evictions、disk reads/writes | BufferPool 和 DiskManager 统计可观察 |
| `.flush`、`.quit` | `Flushed`，随后正常退出 | 脏页显式或退出时写回 |

SQL 必须以分号结束；关键字、表名和列名按前端规则处理。CLI 接受单条或同一输入中的多条 SQL，也支持跨行输入直到遇到分号。

## 2 分钟操作系统重点

先运行答辩基准：

```powershell
cmake --build build --config Debug --target oursql_benchmark
.\build\oursql_benchmark.exe
```

预期流程：基准先通过 DiskManager 创建 32 个数据页，再用固定 `pool=4`、`128` 次请求、随机种子 `20260908`，分别运行顺序、随机、热点请求，并打印 FIFO/LRU 的访问次数、命中率、淘汰数、磁盘读写数和耗时。

一次真实运行结果如下，耗时会随机器负载变化：

```text
OurSQL buffer benchmark pool=4 requests=128 seed=20260908 write_policy=WriteBack
sequential FIFO accesses=128 hits=0 misses=128 hit_rate=0.0000 evictions=124 disk_reads=129 disk_writes=12 elapsed_ms=0.674
sequential LRU accesses=128 hits=0 misses=128 hit_rate=0.0000 evictions=124 disk_reads=129 disk_writes=12 elapsed_ms=0.798
random FIFO accesses=128 hits=18 misses=110 hit_rate=0.1406 evictions=106 disk_reads=111 disk_writes=12 elapsed_ms=0.614
random LRU accesses=128 hits=18 misses=110 hit_rate=0.1406 evictions=106 disk_reads=111 disk_writes=12 elapsed_ms=0.659
hotspot FIFO accesses=128 hits=76 misses=52 hit_rate=0.5938 evictions=48 disk_reads=53 disk_writes=11 elapsed_ms=0.483
hotspot LRU accesses=128 hits=76 misses=52 hit_rate=0.5938 evictions=48 disk_reads=53 disk_writes=11 elapsed_ms=0.596
write_through_status=NotImplemented: WriteThrough 刷新策略尚未实现
```

预期解释：顺序扫描超出 4 个 frame 的容量，命中率接近零；热点访问会重复访问少数页面，命中率明显提高；随机模式介于两者之间。当前真实实现中 FIFO 和 LRU 的测试指标可能相同，因为两种 Replacer 都在 Pin 时移出候选、Unpin 时重新加入队列；当前代码没有把它包装成传统意义上完全不同的算法。耗时只能作为本机同次运行的参考，不能据此宣称某策略普遍更快。

## 常见问题

### FIFO 和 LRU 有什么区别？

设计意图是 FIFO 按进入候选集合的顺序淘汰，LRU 按最近使用时间淘汰。当前代码的两个 Replacer 接口和测试均存在，但其 Pin/Unpin 队列更新逻辑相同，所以当前基准中命中率和淘汰数可能相同。这是当前实现的已知风险，后续若要严格区分，需要单独设计访问记录接口并增加回归测试，不能在答辩中声称已经实现了经典 FIFO/LRU 差异。

### pin_count 是锁吗？

不是。`pin_count` 只表示页面还有多少个 guard 或调用者持有，pin 大于零时页面不能被淘汰。页级 `shared_mutex` 保护页面字节：读 guard 持共享锁，写 guard 持独占锁。锁保证访问期间数据安全，pin 保证 frame 生命周期稳定，两者不能混用。

### dirty 什么时候产生？

WritePageGuard 修改页面后调用 `MarkDirty()`，guard 释放时脏状态保留在 frame。WriteBack 模式下，显式 `.flush`、BufferPool 关闭或淘汰脏页时调用 DiskManager 写回，成功后清除 dirty。

### WriteThrough 支持吗？

不支持。`FlushPolicy::WriteThrough` 配置接口已经保留，但 BufferPoolManager 构造时返回明确的 `NotImplemented` 状态；基准也会打印该状态。后续实现应单独改变写路径和测试，不能把 WriteBack 静默当成 WriteThrough。

### 一条数据如何落到磁盘？

RowCodec 把 Values 编成带边界信息的记录，HeapTable 沿数据页链寻找空间，SlottedPage 写入槽目录和记录区，WritePageGuard 标记 dirty，BufferPoolManager 最终把 4096 字节 Page 交给 DiskManager，DiskManager 写入 `.oursql` 文件。

## 测试与当前边界

完整测试：

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

当前阶段实际结果为 `7/7` 通过，包含公共类型、DiskManager、Replacer、BufferPool、Slotted Page、RowCodec、Catalog、Parser/Planner、Executor、重启端到端和边界测试。另有 `oursql_benchmark` 基准目标，不作为 CTest 用例。

当前没有实现：JOIN、索引、事务、MVCC、完整 WAL、WriteThrough、ALTER/DROP、网络服务和 GUI。数据库仍是单数据库、单表查询教学实现；这些功能不能在演示中包装成已实现能力。
