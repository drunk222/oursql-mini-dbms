#pragma once

#include "oursql/common/status.h"
#include "oursql/common/value.h"

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oursql {

struct Column {
  std::string name;
  DataType type{DataType::Int};
  // VARCHAR(n) 的 n 是 UTF-8 编码后的最大字节数；std::string::size() 按字节计数。
  std::optional<std::size_t> length;
  // 约束元数据。PRIMARY KEY 同时隐含 nullable=false 和 unique=true。
  bool nullable{true};
  bool primary_key{false};
  bool unique{false};
  std::optional<Value> default_value;

  Column() = default;

  // 调用者：模式定义代码；作用：描述一列；返回：列名、类型和可选长度。
  Column(std::string column_name, DataType column_type,
         std::optional<std::size_t> varchar_length = std::nullopt,
         bool is_nullable = true, bool is_primary_key = false,
         bool is_unique = false,
         std::optional<Value> default_literal = std::nullopt)
      : name(std::move(column_name)),
        type(column_type),
        length(varchar_length),
        nullable(is_nullable),
        primary_key(is_primary_key),
        unique(is_unique || is_primary_key),
        default_value(std::move(default_literal)) {}
};

class Schema {
 public:
  Schema() = default;

  // 调用者：表定义或测试代码；作用：创建一张表的结构；返回：按顺序保存的列集合。
  Schema(std::initializer_list<Column> columns);

  // 调用者：表定义或测试代码；作用：创建一张表的结构；返回：按顺序保存的列集合。
  explicit Schema(std::vector<Column> columns);

  [[nodiscard]] const std::vector<Column> &columns() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

  // 调用者：上层查询或校验代码；作用：按列名查找列；返回：找到则返回只读指针，否则返回错误。
  [[nodiscard]] Result<const Column *> FindColumn(std::string_view name) const;

  // 调用者：上层查询或校验代码；作用：按列名查找列下标；返回：找到则返回下标，否则返回错误。
  [[nodiscard]] Result<std::size_t> FindColumnIndex(std::string_view name) const;

  // 调用者：测试或内部代码；作用：按下标读取列；返回：对应列的只读引用。
  [[nodiscard]] const Column &At(std::size_t index) const;

 private:
  std::vector<Column> columns_;
};

}  // namespace oursql
