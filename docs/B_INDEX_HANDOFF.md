# B 索引模块联合验收说明

## 1. 当前结论

B+ 树、Catalog 索引元数据、`IndexManager` 与 SQL 执行链已经接通。当前可以直接执行：

```sql
CREATE [UNIQUE] INDEX index_name ON table_name(column_name);
DROP INDEX index_name;
SELECT * FROM table_name WHERE indexed_int_column = 100;
```

INSERT、UPDATE、DELETE 会同步维护一张表上的全部索引；数据库重启后优化器仍能恢复索引元数据并生成 `IndexScanPlan`。`DROP TABLE` 也已按“删除索引、释放数据页、删除表元数据”的顺序接通。

## 2. 已完成能力

- 4096 字节叶页和内部页；
- 单列 `INT Key + RID`；
- 插入、等值查询、闭区间范围查询；
- 叶页、内部页和根分裂；
- 删除借位、合并、父分隔键更新及根收缩；
- 非唯一索引跨叶页重复 Key；
- Catalog 保存索引名、表、列、根 Page ID 和唯一性；
- 从 HeapTable 已有记录建立索引；
- 唯一索引创建和写入冲突检查；
- SQL `CREATE INDEX`、`DROP INDEX` 和等值 `IndexScan`；
- SQL INSERT、UPDATE、DELETE 索引维护；
- `DROP INDEX`/`DROP TABLE` 页面释放与持久化 Free List 复用；
- 关闭重开后的查询和继续维护。

## 3. 联合验收

`tests/execution_tests.cpp` 已覆盖：

- 创建索引后重启，`EXPLAIN` 仍显示 `IndexScanPlan`；
- 重启后执行 INSERT、UPDATE、DELETE，再次重启验证；
- SeqScan 与 IndexScan 的完整结果集合一致；
- 非唯一索引一个 Key 返回多个 RID；
- 一张表同时维护唯一和非唯一两个索引；
- 大量删除触发 B+ 树合并；
- 最小 Key、最大 Key 和不存在 Key；
- 已有重复数据时创建唯一索引失败且不遗留元数据；
- DROP INDEX 后页面复用；
- DROP TABLE 后索引页、数据页和 Catalog 元数据均消失，重启不恢复。

## 4. 性能展示

新增 `oursql_index_benchmark`，使用 10,000 行、16-frame BufferPool 和固定查询：

```sql
SELECT * FROM student WHERE id = 8765;
```

基准分别创建无索引和有索引的同数据数据库；冷缓存每轮重新创建 `DatabaseEngine`，热缓存先查询预热再调用 `ResetStatistics()`。输出耗时、BufferPool accesses/hit rate/evictions、磁盘读取和返回行数。

详细运行方法和一次实测结果见 `docs/INDEX_BENCHMARK.md`。

## 5. 当前边界

- SQL 优化目前只对 `indexed_int_column = integer_literal` 生成 IndexScan；
- B+ 树和 `IndexManager::RangeScan()` 已支持闭区间，但 SQL `>=`、`<=`、`BETWEEN` 尚未接入范围计划；
- 暂不支持 VARCHAR、复合索引、NULL Key、并发、事务、WAL 和覆盖索引；
- 当前没有事务/WAL，DROP 等多页操作遇到中途 I/O 错误时不提供原子回滚。

索引工作已经从“等待 A/C 接口”进入联合验收阶段；目前剩余的索引增强项主要是 SQL 范围 IndexScan。
