#pragma once

#include "oursql/common/schema.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oursql {

class BufferPoolManager;
class DiskManager;

struct TableMetadata {
  std::string name;
  Schema schema;
  page_id_t first_data_page_id{INVALID_PAGE_ID};
};

using TableInfo = TableMetadata;

class CatalogView {
 public:
  virtual ~CatalogView() = default;

  // 调用者：Planner 等只读模块；作用：按表名读取元信息；返回：找到则返回只读指针，否则返回错误。
  virtual Result<const TableInfo *> FindTable(std::string_view table_name) const = 0;
};

using CatalogReader = CatalogView;

class Catalog final : public CatalogView {
 public:
  Catalog() = default;
  // 调用者：数据库打开流程；作用：绑定持久化所需的缓冲池和磁盘管理器；返回：未打开的 Catalog 对象。
  Catalog(BufferPoolManager *buffer_pool, DiskManager *disk_manager) noexcept
      : buffer_pool_(buffer_pool), disk_manager_(disk_manager) {}

  // 调用者：数据库打开流程；作用：加载或创建 Catalog Slotted Page 链；返回：操作状态。
  [[nodiscard]] Status Open();

  // 调用者：DDL 执行器；作用：登记一张表；返回：成功或失败状态。
  [[nodiscard]] Status CreateTable(TableInfo table);

  // 调用者：查询或执行模块；作用：按表名读取元信息；返回：找到则返回只读指针，否则返回错误。
  [[nodiscard]] Result<const TableInfo *> FindTable(std::string_view table_name) const override;

  // 调用者：测试或管理代码；作用：判断表是否存在；返回：是否已登记。
  [[nodiscard]] bool HasTable(std::string_view table_name) const;

  // 调用者：元数据或测试代码；作用：列出当前 Catalog 中的表；返回：表元数据副本。
  [[nodiscard]] std::vector<TableMetadata> ListTables() const;

  // 调用者：Planner 或执行器；作用：读取指定表 Schema；返回：只读 Schema 指针或错误。
  [[nodiscard]] Result<const Schema *> GetSchema(std::string_view table_name) const;

  // 调用者：HeapTable；作用：获取一张表的持久化元数据；返回：只读表元数据指针或错误。
  [[nodiscard]] Result<const TableMetadata *> GetTableMetadata(std::string_view table_name) const;

 private:
  std::map<std::string, TableInfo> tables_;
  BufferPoolManager *buffer_pool_{nullptr};
  DiskManager *disk_manager_{nullptr};
  page_id_t catalog_head_{INVALID_PAGE_ID};
  bool opened_{false};
};

}  // namespace oursql
