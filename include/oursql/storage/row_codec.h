#pragma once

#include "oursql/common/schema.h"

#include <cstddef>
#include <vector>

namespace oursql {

using Row = std::vector<Value>;

class RowCodec {
 public:
  // 调用者：HeapTable；作用：按 Schema 编码一行；返回：带列边界的二进制记录或错误。
  [[nodiscard]] static Result<std::vector<std::byte>> Encode(const Schema &schema, const Row &row);
  // 调用者：HeapTable；作用：按 Schema 安全解码记录；返回：一行值或损坏数据错误。
  [[nodiscard]] static Result<Row> Decode(const Schema &schema, const std::vector<std::byte> &record);
};

}  // namespace oursql
