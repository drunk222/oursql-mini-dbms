#pragma once

#include "oursql/common/schema.h"
#include "oursql/transaction/transaction.h"

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

struct IndexMetadata {
  std::string name;
  std::string table_name;
  std::string column_name;
  page_id_t root_page_id{INVALID_PAGE_ID};
  bool is_unique{false};
};

using IndexInfo = IndexMetadata;

class CatalogView {
 public:
  virtual ~CatalogView() = default;

  // 调用者：Planner 等只读模块；作用：按表名读取元信息；返回：找到则返回只读指针，否则返回错误。
  virtual Result<const TableInfo *> FindTable(std::string_view table_name) const = 0;

  // 调用者：Planner/Optimizer；作用：读取指定表的索引定义；返回：索引元数据副本。
  virtual std::vector<IndexMetadata> ListTableIndexes(
      std::string_view table_name) const = 0;
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
  // 调用者：事务协调器；作用：在回滚恢复 Catalog 页面后重建内存索引；返回：重新加载状态。
  [[nodiscard]] Status Reload();

  // 调用者：DDL 执行器；作用：登记一张表；返回：成功或失败状态。
  [[nodiscard]] Status CreateTable(TableInfo table);

  // 调用者：DROP TABLE编排层；作用：删除无关联索引的表元数据记录；不释放数据页。
  [[nodiscard]] Status DropTable(std::string_view table_name);

  // 调用者：索引创建流程；作用：登记并持久化索引定义；返回：成功或校验错误。
  [[nodiscard]] Status CreateIndex(IndexInfo index);

  // 调用者：B+树根分裂流程；作用：更新并持久化索引根页面编号；返回：操作状态。
  [[nodiscard]] Status UpdateIndexRootPageId(std::string_view index_name,
                                             page_id_t root_page_id,
                                             Transaction *transaction = nullptr);

  // 调用者：IndexManager；作用：删除索引元数据记录；不负责释放B+树页面。
  [[nodiscard]] Status DropIndex(std::string_view index_name);

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

  // 调用者：执行器或管理代码；作用：按索引名获取元数据；返回：只读索引定义或错误。
  [[nodiscard]] Result<const IndexMetadata *> FindIndex(std::string_view index_name) const;

  // 调用者：测试或管理代码；作用：判断索引是否存在；返回：是否已登记。
  [[nodiscard]] bool HasIndex(std::string_view index_name) const;

  // 调用者：索引执行路径；作用：获取索引元数据；返回：只读索引定义或错误。
  [[nodiscard]] Result<const IndexMetadata *> GetIndexMetadata(
      std::string_view index_name) const;

  // 调用者：管理代码或测试；作用：列出全部索引；返回：索引元数据副本。
  [[nodiscard]] std::vector<IndexMetadata> ListIndexes() const;

  // 调用者：优化器或测试；作用：列出指定表的索引；返回：索引元数据副本。
  [[nodiscard]] std::vector<IndexMetadata> ListTableIndexes(
      std::string_view table_name) const override;

 private:
  [[nodiscard]] Status AppendCatalogRecord(const std::vector<std::byte> &record);

  std::map<std::string, TableInfo> tables_;
  std::map<std::string, IndexInfo> indexes_;
  BufferPoolManager *buffer_pool_{nullptr};
  DiskManager *disk_manager_{nullptr};
  page_id_t catalog_head_{INVALID_PAGE_ID};
  bool opened_{false};
};

}  // namespace oursql
