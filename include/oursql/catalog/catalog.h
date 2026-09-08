#pragma once

#include "oursql/common/schema.h"

#include <map>
#include <string>
#include <string_view>

namespace oursql {

struct TableInfo {
  std::string name;
  Schema schema;
};

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

  // 调用者：DDL 执行器；作用：登记一张表；返回：成功或失败状态。
  [[nodiscard]] Status CreateTable(TableInfo table);

  // 调用者：查询或执行模块；作用：按表名读取元信息；返回：找到则返回只读指针，否则返回错误。
  [[nodiscard]] Result<const TableInfo *> FindTable(std::string_view table_name) const override;

  // 调用者：测试或管理代码；作用：判断表是否存在；返回：是否已登记。
  [[nodiscard]] bool HasTable(std::string_view table_name) const;

 private:
  std::map<std::string, TableInfo> tables_;
};

}  // namespace oursql
