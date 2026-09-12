#include "oursql/common/value.h"

#include <sstream>
#include <utility>

namespace oursql {

Value::Value() : data_(std::monostate{}) {}

Value::Value(std::int64_t integer) : data_(integer) {}

Value::Value(std::string text) : data_(std::move(text)) {}

Value::Value(const char *text) : data_(std::string(text)) {}

bool Value::IsInt() const noexcept {
  return std::holds_alternative<std::int64_t>(data_);
}

bool Value::IsVarchar() const noexcept {
  return std::holds_alternative<std::string>(data_);
}

bool Value::IsNull() const noexcept {
  return std::holds_alternative<std::monostate>(data_);
}

DataType Value::type() const noexcept {
  if (IsNull()) return DataType::Null;
  return IsInt() ? DataType::Int : DataType::Varchar;
}

std::int64_t Value::AsInt() const {
  return std::get<std::int64_t>(data_);
}

const std::string &Value::AsVarchar() const {
  return std::get<std::string>(data_);
}

std::string Value::ToString() const {
  if (IsNull()) return "NULL";
  if (IsInt()) {
    return std::to_string(AsInt());
  }
  return AsVarchar();
}

bool operator==(const Value &lhs, const Value &rhs) noexcept {
  return lhs.data_ == rhs.data_;
}

bool operator!=(const Value &lhs, const Value &rhs) noexcept {
  return !(lhs == rhs);
}

}  // namespace oursql

