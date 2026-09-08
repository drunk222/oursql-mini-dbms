#include "oursql/catalog/catalog.h"

#include <utility>

namespace oursql {

Status Catalog::CreateTable(TableInfo table) {
  if (table.name.empty()) {
    return Status::InvalidArgument("表名不能为空");
  }
  if (table.schema.size() == 0) {
    return Status::InvalidArgument("表结构不能为空");
  }
  if (tables_.find(table.name) != tables_.end()) {
    return Status::AlreadyExists("表已存在: " + table.name);
  }

  const std::string key = table.name;
  tables_.emplace(key, std::move(table));
  return Status::Ok();
}

Result<const TableInfo *> Catalog::FindTable(std::string_view table_name) const {
  auto it = tables_.find(std::string(table_name));
  if (it == tables_.end()) {
    return Result<const TableInfo *>(Status::NotFound(std::string("Catalog 未找到表: ") + std::string(table_name)));
  }
  return Result<const TableInfo *>(&it->second);
}

bool Catalog::HasTable(std::string_view table_name) const {
  return tables_.find(std::string(table_name)) != tables_.end();
}

}  // namespace oursql

