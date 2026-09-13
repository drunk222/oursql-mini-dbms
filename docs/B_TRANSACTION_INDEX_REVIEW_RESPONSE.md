# B 模块事务接入审查回复

审查基线：`5b30ddd`（A 组事务编译与 DatabaseEngine 接线已合入）

## 审查结论

1. B+ 树的写页、分配页和释放页路径已经贯穿 C 提供的 `Transaction *`。
2. 分裂、借位、合并、根创建和根收缩均通过事务版 BufferPool 接口修改页面。
3. IndexManager 的事务路径不再执行业务级反向补偿；非事务兼容路径仍保留原补偿。
4. IndexManager 将同一个事务传给 B+ 树 Header Page 和 Catalog 根 Page ID 更新。
5. 显式事务中的索引错误会进入 Failed 状态，后续交由统一 Rollback。
6. CREATE/DROP INDEX 仍属于非事务 DDL；显式事务内由 DatabaseEngine 拒绝。

## 本次修正

- B+ 树原先只调用 `MarkDirty()`，随后依赖析构释放 WritePageGuard。析构会忽略 WAL 追加或 WriteThrough 刷盘错误。本次改为显式 `Release()`，并把错误返回调用者。
- 带临时事务上下文的 `Insert/Remove` 只在操作成功后同步新的内存根 Page ID，避免失败后把中间根传播给旧对象。
- IndexManager 在缺失 Catalog/BufferPool 依赖时也会把传入事务标记为 Failed。

## 新增验收覆盖

- B+ 树事务写 Guard 释放失败必须向上传播。
- UPDATE 不修改索引 Key、但 Heap RID 改变后的事务回滚。
- 未提交插入触发叶分裂、内部节点分裂和新根后真实进程崩溃；恢复撤销新 Key 并回收分裂页。
- 删除触发合并和根收缩后，COMMIT durable、deferred free 尚未完成时真实进程崩溃；恢复重做树结构并完成页面释放。
- `ListTables()` 的稳定排序和值副本。
- `GetTableMetadata()` 的 Schema、NotFound 和值副本。
- 最小 Web 演示 CRUD 序列、影响行数、列名/行长度及重启持久化。

## 与 A 的接口边界

B 已验证以下 DatabaseEngine 公共结果可供 QueryService 使用：

- `ExecuteSqlBatch()`
- `ListTables()`
- `GetTableMetadata()`
- `ExecutionResult::column_names/rows/affected_rows`

B 未新增 HTTP、JSON、HTML、CSS 或 JavaScript。A 可以在全局数据库互斥范围内直接转发这些公共结果。

## 需要 C 确认的问题

当事务 WritePageGuard 找不到绑定的 LogManager 时，BufferPool 会返回失败并把事务标记为 Failed，但当前早期失败分支没有恢复 before image。正常 DatabaseEngine 初始化会绑定 LogManager，因此现有 SQL 路径不受影响；C 仍应让 `RecordGuardUpdate()` 的所有日志失败分支统一恢复页面 before image，以满足冻结协议。

## 验证结果

```text
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

100% tests passed, 0 tests failed out of 10
```

本次审查后，Catalog、IndexManager 和 B+ 树的事务业务接入由 B 继续维护；WAL 文件、BufferPool 日志协议、事务管理器和恢复算法仍由 C 维护。
