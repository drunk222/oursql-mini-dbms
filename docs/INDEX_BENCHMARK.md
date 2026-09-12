# SeqScan 与 IndexScan 性能对比

## 构建和运行

```powershell
cmake -S . -B build
cmake --build build --config Release --target oursql_index_benchmark
.\build\Release\oursql_index_benchmark.exe
```

基准自动创建 10,000 行 `student` 表，固定查询：

```sql
SELECT * FROM student WHERE id = 8765;
```

无索引数据库通过 `EXPLAIN` 验证为 `SeqScanPlan`，有索引数据库验证为 `IndexScanPlan`。两份数据库的数据来自同一个关闭后的 fixture 副本。

## 冷热缓存定义

- 冷缓存：每个场景关闭并重新创建 `DatabaseEngine`，不是只清空统计。
- 热缓存：先执行一次相同查询，再调用 `ResetStatistics()`，随后测量第二次查询。
- 四个场景使用相同的 16-frame BufferPool、LRU 策略、查询和数据集。
- 计时范围只包含被测 `SELECT`，不包含建表、插入、建索引、关闭和刷盘。

## 2026-09-12 本机实测

| 场景 | 耗时 ms | accesses | hits | misses | 命中率 | 淘汰数 | 磁盘读 | 磁盘写 | 行数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 冷缓存 SeqScan | 195.968 | 10124 | 10000 | 124 | 98.78% | 109 | 124 | 0 | 1 |
| 冷缓存 IndexScan | 1.654 | 113 | 2 | 111 | 1.77% | 96 | 111 | 0 | 1 |
| 热缓存 SeqScan | 197.412 | 10124 | 10000 | 124 | 98.78% | 124 | 124 | 0 | 1 |
| 热缓存 IndexScan | 1.594 | 113 | 2 | 111 | 1.77% | 111 | 111 | 0 | 1 |

结果会受机器、编译配置和后台负载影响，应以现场重新运行的输出为准。本次 IndexScan 约比 SeqScan 快两个数量级。IndexScan 仍产生较多读取，是因为 `HeapTable::GetRow()` 会沿表页链验证 RID 归属；这属于后续可单独优化的存储安全检查，不影响当前索引与全表扫描的可比性。
