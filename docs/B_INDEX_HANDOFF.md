# B 索引模块完成情况与 A/B/C 协作说明

## 1. 当前结论

B 已经完成 B+ 树和索引管理层的第一版，包括页式存储、持久化、删除重平衡、Catalog 索引元数据、索引创建与删除、唯一性检查，以及 INSERT/DELETE/UPDATE 所需的索引维护接口。

当前代码已经可以通过 C++ 接口建立和维护索引，但还不能直接执行以下 SQL：

```sql
CREATE INDEX idx_student_id ON student(id);
DROP INDEX idx_student_id;
SELECT * FROM student WHERE id = 100;
```

剩余关键工作是由 A 补充 SQL 的 Lexer、Parser、AST、Plan 和执行分派，并调用 B 已提供的 `IndexManager`。C 当前提供的 BufferPool/PageGuard/DeletePage 接口已经足够，B 不需要等待 C 才能继续。

## 2. B 已完成的内容

### 2.1 B+ 树页面与基本操作

相关文件：

- `include/oursql/index/b_plus_tree_page.h`
- `src/index/b_plus_tree_page.cpp`
- `include/oursql/index/b_plus_tree.h`
- `src/index/b_plus_tree.cpp`

已经实现：

- 固定 4096 字节的叶子页和内部页格式；
- 索引项保存 `INT Key + RID(page_id, slot_id)`；
- 空树创建；
- 插入和等值查询；
- 闭区间范围扫描；
- 相同 Key 对应多个 RID；
- 叶子节点分裂和叶子链表；
- 内部节点分裂和根分裂；
- 删除指定 `Key + RID`；
- 删除后的借位、合并、父分隔键更新和根收缩；
- 整棵索引树页面释放；
- 重启后恢复和继续查询、插入、删除。

当前第一版只支持单列 `INT` 索引，不支持 VARCHAR、复合索引和 NULL Key。

### 2.2 索引元数据

相关文件：

- `include/oursql/catalog/catalog.h`
- `src/catalog/catalog.cpp`

Catalog 已经支持保存和恢复：

- 索引名；
- 所属表名；
- 索引列名；
- B+ 树根 Page ID；
- 是否为唯一索引。

同时提供索引查找、按表列出索引、根 Page ID 更新和删除索引元数据的能力。

### 2.3 IndexManager

相关文件：

- `include/oursql/index/index_manager.h`
- `src/index/index_manager.cpp`

B 已提供以下主要接口：

```cpp
Status CreateIndex(index_name, table_name, column_name, is_unique);
Status DropIndex(index_name);

Result<std::vector<RID>> Lookup(index_name, key);
Result<std::vector<std::pair<index_key_t, RID>>> RangeScan(
    index_name, lower, upper);

Status OnInsert(table_name, row, rid);
Status OnDelete(table_name, row, rid);
Status OnUpdate(table_name, old_row, old_rid, new_row, new_rid);
```

其中：

- `CreateIndex` 会扫描表中已有数据并建立 B+ 树；
- 唯一索引会检查已有数据和后续写入中的重复 Key；
- 根节点变化后会自动更新 Catalog 中的根 Page ID；
- `DropIndex` 会删除 Catalog 元数据并释放索引页面；
- 行级维护接口会处理一张表上的全部索引，并对已经执行的多索引修改进行尽力回滚。

### 2.4 测试情况

相关文件：

- `tests/b_plus_tree_tests.cpp`
- `tests/catalog_heap_tests.cpp`

已经覆盖：

- 空树；
- 乱序插入；
- 等值查询和范围扫描；
- 重复 Key；
- 叶子、内部和根分裂；
- 删除借位、合并和根收缩；
- 小 BufferPool（3 个 Frame）；
- 重启恢复；
- 扫描 1000 行已有数据创建唯一索引；
- INSERT、DELETE、UPDATE 索引维护接口；
- 唯一索引冲突；
- DROP INDEX 和重启后元数据消失。

当前最新构建下，全项目 CTest 为 9/9 通过。

## 3. A 接下来需要完成的工作

A 不需要修改 B+ 树内部，也不需要等待 B 再实现新的数据结构。请直接依赖 `IndexManager`。

### 3.1 CREATE INDEX 和 DROP INDEX

A 负责增加：

- Lexer 关键字；
- `CreateIndexStatement`、`DropIndexStatement`；
- 语义检查：表、列是否存在；
- `CreateIndexPlan`、`DropIndexPlan`；
- 在执行分派中调用：

```cpp
index_manager.CreateIndex(index_name, table_name, column_name, is_unique);
index_manager.DropIndex(index_name);
```

第一版语法建议限定为：

```sql
CREATE INDEX index_name ON table_name(column_name);
CREATE UNIQUE INDEX index_name ON table_name(column_name);
DROP INDEX index_name;
```

### 3.2 INSERT、DELETE、UPDATE 的索引维护

A 的执行器完成普通表操作时，需要同步调用 B 的接口。没有事务/WAL 的当前版本必须做失败补偿，避免“表成功、索引失败”或“索引成功、表失败”。

建议调用约定：

#### INSERT

```cpp
auto rid = heap_table.InsertRow(row);
auto status = index_manager.OnInsert(table_name, row, rid);
if (!status.ok()) {
  heap_table.DeleteRow(rid);  // 补偿表插入
  return status;
}
```

#### DELETE

在删除 Heap 行前保留旧 Row 和 RID：

```cpp
auto status = index_manager.OnDelete(table_name, old_row, rid);
if (!status.ok()) return status;

status = heap_table.DeleteRow(rid);
if (!status.ok()) {
  index_manager.OnInsert(table_name, old_row, rid);  // 补偿索引删除
  return status;
}
```

#### UPDATE

A 当前更新可能产生新 RID，因此必须同时传递旧、新 Row 和 RID：

```cpp
index_manager.OnUpdate(
    table_name, old_row, old_rid, new_row, new_rid);
```

UPDATE 的表修改失败或索引维护失败时，需要恢复旧 Row 和旧索引。接线时 A、B 应一起补一个端到端失败补偿测试。

### 3.3 IndexScan

A 负责：

- 增加 `IndexScanPlan`；
- 优化器检查 `Catalog::ListTableIndexes(table_name)`；
- 对可使用索引的条件生成 IndexScan；
- 没有合适索引时继续生成 SeqScan，保证查询正确性不依赖索引。

第一版建议只优化：

```sql
WHERE indexed_int_column = integer_literal
```

随后再扩展 `<`、`<=`、`>`、`>=` 和 BETWEEN。B 已提供 `Lookup` 和闭区间 `RangeScan`。

IndexScan 从 B 得到 RID 后，应通过 HeapTable 读取对应 Row，并再次计算 WHERE 条件，避免优化器或索引边界错误影响查询正确性。

### 3.4 EXPLAIN 和端到端测试

A、B 最终需要共同验证：

```sql
EXPLAIN SELECT * FROM student WHERE id = 100;
```

有索引时显示 `IndexScan`，无索引时显示 `SeqScan`。还需要通过 `DatabaseEngine` 执行 SQL，验证 INSERT、UPDATE、DELETE 后索引结果和全表扫描一致。

## 4. C 接下来如何配合

C 当前不需要为 B+ 树新增接口，现有接口已经能支撑索引的创建、查询、修改、删除和恢复。

C 后续需要和 B 确认：

1. `DeletePage` 遇到仍被 pin 的页面时，错误语义保持稳定；
2. DROP INDEX/大量删除后，释放的页面能够进入 Free List 并被后续分配复用；
3. DROP TABLE 时，A 先找到该表全部索引，B 调用 `DropIndex` 释放索引页，C 再释放数据页链；
4. 性能测试中提供 BufferPool 命中、淘汰和磁盘读写统计，供 B 对比 IndexScan 与 SeqScan；
5. 如果 C 后续修改 PageGuard、DeletePage 或页面分配接口，请提前通知 B，避免破坏索引页生命周期。

## 5. 推荐的三人集成顺序

1. B 提交当前 B+ 树、Catalog 索引元数据和 IndexManager；
2. A 合入并完成 CREATE/DROP INDEX 的 AST、Plan 和执行分派；
3. A 将 INSERT/DELETE/UPDATE 接到 `OnInsert/OnDelete/OnUpdate`；
4. A 增加 IndexScanPlan，B 协助完成索引扫描执行；
5. A、B 添加 SQL 端到端正确性测试；
6. C 提供统计数据，B、C 完成索引与全表扫描性能对比；
7. 三人最后再接 DROP TABLE；事务和回滚放在上述功能稳定以后。

## 6. 当前完成边界

可以表述为：

> B 已完成索引存储与管理层第一版，所有核心接口和独立测试可用；当前等待 A 接入 SQL 编译与执行链，之后由 A、B 共同完成 IndexScan 和端到端维护验证。C 的现有存储接口已满足当前需求，后续主要配合页面回收统计、DROP TABLE 和性能展示。

不能表述为：

> SQL 索引功能已经全部完成。

因为当前还不能直接通过 SQL 创建、删除或使用索引，也尚未完成 IndexScan 与 SeqScan 的性能展示。
