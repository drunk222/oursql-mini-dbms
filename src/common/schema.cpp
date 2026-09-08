#include "oursql/common/schema.h"

#include <stdexcept>
#include <utility>

namespace oursql {

Schema::Schema(std::initializer_list<Column> columns) : columns_(columns) {}

Schema::Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}

const std::vector<Column> &Schema::columns() const noexcept {
  return columns_;
}

std::size_t Schema::size() const noexcept {
  return columns_.size();
}

Result<std::size_t> Schema::FindColumnIndex(std::string_view name) const {
  for (std::size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name) {
      return Result<std::size_t>(i);
    }
  }
  return Result<std::size_t>(Status::NotFound(std::string("Schema 未找到列: ") + std::string(name)));
}

Result<const Column *> Schema::FindColumn(std::string_view name) const {
  auto index = FindColumnIndex(name);
  if (!index.ok()) {
    return Result<const Column *>(index.status());
  }
  return Result<const Column *>(&columns_[index.value()]);
}

const Column &Schema::At(std::size_t index) const {
  if (index >= columns_.size()) {
    throw std::out_of_range("Schema::At 下标越界");
  }
  return columns_[index];
}

}  // namespace oursql

