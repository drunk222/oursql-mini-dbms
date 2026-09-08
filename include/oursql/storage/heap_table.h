#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/storage/row_codec.h"
#include "oursql/storage/storage.h"

#include <utility>
#include <vector>

namespace oursql {

using RowEntry = std::pair<RID, Row>;

class HeapTable {
 public:
  // 调用者：执行器；作用：绑定一张已登记的堆表和缓冲池；返回：非拥有表对象。
  HeapTable(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept;

  // 调用者：插入执行器；作用：沿数据页链插入一行；返回：新行 RID 或错误。
  [[nodiscard]] Result<RID> InsertRow(const Row &row);
  // 调用者：点查执行器；作用：按 RID 获取并解码一行；返回：行或错误。
  [[nodiscard]] Result<Row> GetRow(const RID &rid);
  // 调用者：删除执行器；作用：按 RID 删除一行；返回：操作状态。
  [[nodiscard]] Status DeleteRow(const RID &rid);
  // 调用者：扫描执行器；作用：沿数据页链返回所有有效行；返回：RID 与行序列或错误。
  [[nodiscard]] Result<std::vector<RowEntry>> Scan();

 private:
  BufferPoolManager *buffer_pool_{nullptr};
  TableMetadata metadata_;
};

}  // namespace oursql
