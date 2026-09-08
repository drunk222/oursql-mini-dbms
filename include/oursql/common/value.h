#pragma once

#include "oursql/common/types.h"

#include <cstdint>
#include <string>
#include <variant>

namespace oursql {

class Value {
 public:
  // 调用者：解析器或执行器；作用：构造整数值；返回：可比较的标量值。
  Value(std::int64_t integer);

  // 调用者：解析器或执行器；作用：构造字符串值；返回：可比较的标量值。
  Value(std::string text);

  // 调用者：解析器或执行器；作用：构造字符串值；返回：可比较的标量值。
  Value(const char *text);

  [[nodiscard]] bool IsInt() const noexcept;
  [[nodiscard]] bool IsVarchar() const noexcept;
  [[nodiscard]] DataType type() const noexcept;
  [[nodiscard]] std::int64_t AsInt() const;
  [[nodiscard]] const std::string &AsVarchar() const;
  [[nodiscard]] std::string ToString() const;

  friend bool operator==(const Value &lhs, const Value &rhs) noexcept;
  friend bool operator!=(const Value &lhs, const Value &rhs) noexcept;

 private:
  std::variant<std::int64_t, std::string> data_;
};

}  // namespace oursql

