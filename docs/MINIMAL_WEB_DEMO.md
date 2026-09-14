# OurSQL 最小 Web 演示

本版本在现有 `oursql_core` 上增加一个本地 Web 工作台。浏览器是客户端，程序仍然是单数据库、单进程服务。

## 构建

项目已经固定 vendored `cpp-httplib v0.18.7` 和 `nlohmann/json v3.11.3`，配置阶段不需要联网下载依赖。

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

## 启动

```powershell
.\build\Debug\oursql_web.exe `
  --host 127.0.0.1 `
  --port 8080 `
  --database .\build\demo.oursql `
  --pool-size 16 `
  --policy lru `
  --web-root .\build\Debug\web
```

浏览器打开：

```text
http://127.0.0.1:8080
```

默认只监听本机地址，不提供远程访问。

## 演示 SQL

```sql
CREATE TABLE student(id INT, name VARCHAR);
INSERT INTO student VALUES(1, 'Alice');
INSERT INTO student VALUES(2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 2;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

预期结果：

- 建表成功。
- 两次插入均显示执行成功。
- 第一次查询显示 Alice 和 Bob。
- 指定列查询显示 Bob。
- 删除显示执行成功。
- 最后只显示 Bob。

## HTTP API

```text
GET  /api/health
POST /api/query
GET  /api/tables
GET  /api/schema?table=student
GET  /api/stats
POST /api/flush
POST /api/concurrency/demo
```

`GET /api/tables` 返回每张表的名称和列定义，供 API 调用方使用。

## 全量 SQL 测试脚本

工作台的“载入测试 SQL”按钮会读取：

```text
web/assets/frontend_all_runnable.sql
```

也可以在浏览器直接访问：

```text
http://127.0.0.1:8080/assets/frontend_all_runnable.sql
```

该脚本只包含已经接入执行层的 SQL，覆盖：

- `CREATE TABLE`
- 单行全列 `INSERT`
- `CREATE INDEX` 和 `CREATE UNIQUE INDEX`
- `EXPLAIN SELECT`
- 投影、列别名、表别名和 `DISTINCT`
- `WHERE` 比较、`AND`、`OR`、`NOT`、`BETWEEN`、`IN`、`LIKE`、`IS NULL`
- `ORDER BY` 和 `LIMIT`
- `GROUP BY`、`COUNT`、`SUM`、`AVG`、`MAX`、`MIN`、`HAVING`
- `INNER JOIN`
- `UPDATE` 和 `DELETE`
- `BEGIN`、`COMMIT`、`ROLLBACK`
- `DROP INDEX` 和 `DROP TABLE`

当前仍返回 `NotImplemented` 的指定列 INSERT、多行 INSERT、`ALTER TABLE`、外连接和子查询没有放入该脚本。

## 数据库导航

工作台侧栏使用树形数据库导航：

```text
OurSQL 连接
  -> 当前数据库
    -> 表
      -> 表节点
        -> 列名和类型
```

导航支持表名筛选、展开和折叠、节点选中以及键盘方向键操作。执行 `CREATE TABLE` 或 `DROP TABLE` 后会自动刷新树。

## 并发演示页

工作台顶部的“并发演示”进入：

```text
http://127.0.0.1:8080/concurrency.html
```

该页面会运行四个隔离场景：

1. 两个会话在不同表上并发提交。
2. 同一索引键和页面上的锁等待。
3. `DROP TABLE` 等待目标表上的 DML 事务提交。
4. 循环等待的死锁检测与受害者回滚。

演示使用临时数据库，不修改工作台当前数据库。每个场景都会返回事件时间线、耗时和关键指标。

成功响应：

```json
{
  "ok": true,
  "data": {}
}
```

失败响应：

```json
{
  "ok": false,
  "error": {
    "code": "NotFound",
    "message": "表不存在"
  }
}
```

## 限制

- 仅面向本机单用户演示。
- 不支持跨请求显式事务。
- 所有数据库访问由服务层全局互斥保护。
- 不支持 WebSocket、TLS 或公网部署。
- 不修改 B 的索引逻辑或 C 的 WAL/恢复逻辑。

## 测试

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

单独运行 Web API 测试：

```powershell
ctest --test-dir build -C Debug -R oursql_web_tests --output-on-failure
```
