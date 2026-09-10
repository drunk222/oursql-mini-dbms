# OurSQL 答辩演示手册

本文档对应当前仓库真实代码。构建目录假定为 `build`，数据库文件示例使用源码目录外的 `build/demo.oursql`。

## 3 分钟基础功能

在项目根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\oursql.exe .\build\demo.oursql
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
.\build\Debug\oursql_benchmark.exe
```

预期流程：基准先通过 DiskManager 创建 32 个数据页，再用固定 `pool=4`、`128` 次请求、随机种子 `20260908`，分别运行顺序、随机、热点请求，并打印 FIFO/LRU 的访问次数、命中率、淘汰数、磁盘读写数和耗时。

一次真实运行结果如下，耗时会随机器负载变化：

```text
OurSQL buffer benchmark pool=4 requests=128 seed=20260908 write_policy=WriteBack
sequential FIFO accesses=128 hits=0 misses=128 hit_rate=0.0000 evictions=124 disk_reads=129 disk_writes=12 elapsed_ms=1.050
sequential LRU accesses=128 hits=0 misses=128 hit_rate=0.0000 evictions=124 disk_reads=129 disk_writes=12 elapsed_ms=0.804
random FIFO accesses=128 hits=19 misses=109 hit_rate=0.1484 evictions=105 disk_reads=110 disk_writes=12 elapsed_ms=0.664
random LRU accesses=128 hits=18 misses=110 hit_rate=0.1406 evictions=106 disk_reads=111 disk_writes=12 elapsed_ms=0.753
hotspot FIFO accesses=128 hits=70 misses=58 hit_rate=0.5469 evictions=54 disk_reads=59 disk_writes=11 elapsed_ms=0.507
hotspot LRU accesses=128 hits=76 misses=52 hit_rate=0.5938 evictions=48 disk_reads=53 disk_writes=11 elapsed_ms=0.689
contrast FIFO sequence=1,2,3,1,4,1 first_evicted=1 hits=1 misses=5
contrast LRU sequence=1,2,3,1,4,1 first_evicted=2 hits=2 misses=4
write_through_status=NotImplemented: WriteThrough 刷新策略尚未实现
```

预期解释：顺序扫描超出 4 个 frame 的容量，命中率接近零；热点访问会重复访问少数页面，命中率明显提高；随机模式介于两者之间。固定对照序列用于直接观察两种替换顺序的差异。耗时只能作为本机同次运行的参考，不能据此宣称某策略普遍更快。

## 常见问题

### FIFO 和 LRU 有什么区别？

FIFO 按页面进入缓冲池的到达顺序记录历史。命中后的 Pin/Unpin 不改变这个到达顺序；页面被淘汰后重新装入时获得新的到达顺序。LRU 在每次成功 Fetch 命中时更新时间戳，候选页按最近使用时间排序。固定序列 `1,2,3,1,4,1` 中，FIFO 先淘汰 1，最后一次访问 1 是 miss；LRU 先淘汰 2，最后一次访问 1 是 hit。

### pin_count 是锁吗？

不是。`pin_count` 只表示页面还有多少个 guard 或调用者持有，pin 大于零时页面不能被淘汰。页级 `shared_mutex` 保护页面字节：读 guard 持共享锁，写 guard 持独占锁。锁保证访问期间数据安全，pin 保证 frame 生命周期稳定，两者不能混用。

### dirty 什么时候产生？

WritePageGuard 修改页面后调用 `MarkDirty()`，guard 释放时脏状态保留在 frame。WriteBack 模式下，显式 `.flush`、BufferPool 关闭或淘汰脏页时调用 DiskManager 写回，成功后清除 dirty。

### WriteThrough 支持吗？

不支持。`FlushPolicy::WriteThrough` 配置接口已经保留，但 BufferPoolManager 构造时返回明确的 `NotImplemented` 状态；基准也会打印该状态。后续实现应单独改变写路径和测试，不能把 WriteBack 静默当成 WriteThrough。

### RecordTooLarge 和 OutOfSpace 有什么区别？

`RecordTooLarge` 表示单条记录超过空白 Slotted Page 的最大记录大小 `4096-32-12=4052` 字节，无论当前页面是否有空间都不会申请新页。`OutOfSpace` 表示记录本身不超过 4052 字节，但当前页面在必要整理后仍放不下。Catalog 元数据也遵守同一单条记录上限。

### VARCHAR(n) 的 n 按什么计数？

`n` 表示 UTF-8 编码后的最大字节数，不是 Unicode 字符数。例如“你好”占 6 个字节，因此 `VARCHAR(6)` 接受，`VARCHAR(5)` 拒绝。实现使用 `std::string::size()`，不做额外 Unicode 字符计数。

### 内存 FreeList 和磁盘空闲链有什么区别？

BufferPool 的 FreeList 只记录尚未占用的内存 frame；磁盘 Superblock 的 free list 记录数据库文件中已经释放的 page。读取或分配失败时，BufferPool 会把取出的 frame 归还 FreeList；成功删除缓存页后也会清除替换历史再归还。

### Plan 为什么不会被执行器修改？

`SelectPlan::root`、`FilterPlan::child` 和 `ProjectPlan::child` 都是 `std::shared_ptr<const PlanNode>`。Planner 构造完成后，ExecutionEngine 只能读取 `const PlanNode`，不能通过这些指针修改算子树。

编译层可选的后处理通道是 `Optimizer`（由 `Planner::Build` 在返回前调用）。它的职责是校验 SELECT 计划树的允许形态，只允许不改变计划结构和执行结果的重写。当前包含「冗余内层 Project 裁剪」规则：外层是显式投影时，内层必须覆盖它需要的列；外层是 `SELECT *` 时，内层也必须保留整行。根 Project 永不裁剪；传入不符合冻结形态的计划时，`Optimizer` 返回 `InternalError`。规则调度器用显式 `changed` 标志反复运行固定规则集，直到整轮没有改写，并通过 `OptimizeWithStats()` 返回 `passes` 和 `rule_hits` 供测试与答辩展示。

### 一条数据如何落到磁盘？

RowCodec 把 Values 编成带边界信息的记录，HeapTable 沿数据页链寻找空间，SlottedPage 写入槽目录和记录区，WritePageGuard 标记 dirty，BufferPoolManager 最终把 4096 字节 Page 交给 DiskManager，DiskManager 写入 `.oursql` 文件。

## 测试与当前边界

完整测试：

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

本次最新构建的 CTest 实际结果为 `8/8` 通过，对应 8 个测试目标：`oursql_tests`（公共类型与基础结构）、`oursql_storage_tests`（DiskManager/Replacer/BufferPool）、`oursql_slotted_page_tests`（Slotted Page 与 RowCodec）、`oursql_frontend_tests`（Lexer/Parser/Planner）、`oursql_optimizer_tests`（编译层计划形态校验）、`oursql_catalog_heap_tests`（Catalog 与 HeapTable）、`oursql_execution_tests`（Executor、重启端到端）、`oursql_edge_case_tests`（边界）。另有 `oursql_benchmark` 基准目标，不作为 CTest 用例。

使用 Ninja 单配置生成器时，可执行文件位于 `build/`（例如 `build/oursql.exe`），而不是 `build/Debug/`。

当前没有实现：JOIN、索引、事务、MVCC、完整 WAL、WriteThrough、ALTER/DROP、网络服务和 GUI。数据库仍是单数据库、单表查询教学实现；这些功能不能在演示中包装成已实现能力。

编译层已能解析并生成对应 Plan 节点，但执行层没有对应算子的语法（`GROUP BY`、`ORDER BY`、`UPDATE`）在本项目中会**先完成表、列和类型语义检查，再由 ExecutionEngine 明确返回 `NotImplemented`**。复杂 WHERE 表达式目前仍由 Planner 在语义检查后返回 `NotImplemented`。整个链路不会让执行器静默忽略计划节点。演示时可以直接展示这一行为：

```sql
SELECT * FROM student ORDER BY id DESC;   -- NotImplemented: Execution: ORDER BY 仅编译层支持，未接入数据库执行
SELECT * FROM student GROUP BY name;      -- NotImplemented: Execution: GROUP BY 仅编译层支持，未接入数据库执行
UPDATE student SET name = 'Zoe' WHERE id = 1;  -- NotImplemented: Execution: UPDATE 仅编译层支持，未接入数据库执行
SELECT * FROM student WHERE id > 1;       -- NotImplemented: Planner: 复杂 WHERE 表达式仅编译层支持，未接入数据库执行，位置 1:1
```
