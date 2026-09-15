# OurSQL Web 端启动说明

## 1. 环境要求

- Windows 10 或 Windows 11。
- Visual Studio 2022，并安装“使用 C++ 的桌面开发”工作负载。
- CMake 3.20 或更高版本。
- 支持 HTML、CSS 和 JavaScript 的现代浏览器。

项目已经固定 vendored 以下依赖，配置阶段不需要联网下载：

- `cpp-httplib v0.18.7`
- `nlohmann/json v3.11.3`

## 2. 构建项目

在仓库根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

只构建 Web 端时：

```powershell
cmake --build build --config Debug --target oursql_web
```

构建完成后，主要程序位于：

```text
build\Debug\oursql_web.exe
```

静态页面会自动复制到：

```text
build\Debug\web
```

## 3. 启动 Web 服务

在仓库根目录执行：

```powershell
.\build\Debug\oursql_web.exe `
  --host 127.0.0.1 `
  --port 8080 `
  --database .\build\demo.oursql `
  --pool-size 16 `
  --policy lru `
  --flush-policy write-back `
  --web-root .\build\Debug\web
```

默认只监听本机地址，不提供远程访问。

## 4. 打开页面

浏览器访问：

```text
http://127.0.0.1:8080
```

并发演示页面：

```text
http://127.0.0.1:8080/concurrency.html
```

性能实验室：

```text
http://127.0.0.1:8080/performance.html
```

性能实验室会在临时数据库中构造相同数据，首次运行 SeqScan 与 B+ 树
IndexScan，并展示 Token、AST、常量折叠、优化规则、服务端中位耗时、
执行计划、Buffer 访问、命中率、淘汰和磁盘 I/O。测试不会修改工作台当前
打开的数据库。页面还包含基于 90%/10% 选择率的成本优化实验，以及
FIFO/LRU/CLOCK 页面替换策略在顺序、随机和热点访问下的对比实验。

## 5. 页面功能

主工作台包含：

- 带行号的 SQL 编辑器。
- SQL 编辑器高度支持竖向拖拽、键盘调整和双击恢复默认。
- SQL 执行结果表格，每条结果左侧标出对应的输入行号。
- “链路展示”模式按顺序显示 Token、AST、语义检查和逻辑 Plan。
- DBeaver 风格的数据库导航树。
- 点击表节点查看表数据和字段 UML 图。
- 可拖拽调整宽度的侧栏。
- 表名筛选和字段展开。
- BufferPool 与磁盘统计。
- 并发 SQL 工作台，可切换 2–6 个独立会话并查看每条 SQL 的累计执行时间。

数据库导航支持：

- 展开和折叠连接、数据库、表和字段。
- 鼠标拖动侧栏分隔条调整宽度。
- 双击分隔条恢复默认宽度。
- 键盘左右方向键调整侧栏宽度。
- 浏览器本地保存侧栏宽度。

## 6. 演示 SQL

```sql
CREATE TABLE student(id INT, name VARCHAR);
INSERT INTO student VALUES(1, 'Alice');
INSERT INTO student VALUES(2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 2;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

## 7. 浏览器不显示页面

检查端口是否已被占用：

```powershell
Get-NetTCPConnection -LocalPort 8080 -State Listen
```

如果被占用，可以改用其他端口：

```powershell
.\build\Debug\oursql_web.exe `
  --host 127.0.0.1 `
  --port 8081 `
  --database .\build\demo.oursql `
  --web-root .\build\Debug\web
```

然后访问：

```text
http://127.0.0.1:8081
```

## 8. 停止服务

在运行服务的终端按 `Ctrl+C`，或结束对应的 `oursql_web.exe` 进程。

## 9. 运行测试

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

只运行 Web API 测试：

```powershell
ctest --test-dir build -C Debug -R oursql_web_tests --output-on-failure
```

## 10. 当前限制

- 不提供登录、账号、token 或权限系统。
- 不支持跨请求显式事务。
- 工作台数据库操作由服务层互斥保护。
- 并发页面使用多个独立 Session 对当前数据库同时执行输入的 SQL。
- 默认不启用 TLS、WebSocket 或公网部署。
