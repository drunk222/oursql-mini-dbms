#include "oursql/storage/row_codec.h"

#include <limits>

namespace oursql {

// RowCodec 是逻辑 Row 与磁盘字节记录之间的唯一转换点。
// 格式：magic(4) + 列数(4) + 重复的 [类型标签(1) + 字节长度(4) + 字段内容]。
// 修改记录格式时必须同时修改 Encode/Decode，并新增重启读取测试。

namespace {

constexpr std::uint32_t kRowMagic = 0x31574F52U;
constexpr std::size_t kRowHeaderSize = 8;
constexpr std::size_t kFieldHeaderSize = 5;

// 所有整数都按小端序逐字节写入，避免把宿主机结构体布局直接落盘。
void AppendU32(std::vector<std::byte> *record, std::uint32_t value) {
  record->push_back(std::byte{static_cast<unsigned char>(value & 0xFFU)});
  record->push_back(std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)});
  record->push_back(std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)});
  record->push_back(std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)});
}

void AppendU64(std::vector<std::byte> *record, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    record->push_back(std::byte{static_cast<unsigned char>((value >> (i * 8U)) & 0xFFU)});
  }
}

bool CanRead(const std::vector<std::byte> &record, std::size_t offset, std::size_t length) {
  // 先比较 offset，再用减法校验 length，避免 offset + length 整数溢出。
  return offset <= record.size() && length <= record.size() - offset;
}

std::uint32_t ReadU32(const std::vector<std::byte> &record, std::size_t offset) {
  return static_cast<std::uint32_t>(record[offset]) |
         (static_cast<std::uint32_t>(record[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(record[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(record[offset + 3]) << 24U);
}

std::uint64_t ReadU64(const std::vector<std::byte> &record, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint64_t>(record[offset + i]) << (i * 8U);
  }
  return value;
}

std::uint8_t TypeTag(DataType type) {
  // 持久化标签不依赖 enum 的底层数值，调整 DataType 声明不会悄然破坏旧数据。
  return type == DataType::Int ? 1U : 2U;
}

}  // namespace

Result<std::vector<std::byte>> RowCodec::Encode(const Schema &schema, const Row &row) {
  if (row.size() != schema.size()) {
    return Result<std::vector<std::byte>>(Status::InvalidArgument(
        "Row 列数量与 Schema 不符: 期望 " + std::to_string(schema.size()) + "，实际 " +
        std::to_string(row.size())));
  }
  std::vector<std::byte> record;
  record.reserve(kRowHeaderSize + row.size() * kFieldHeaderSize);
  AppendU32(&record, kRowMagic);
  AppendU32(&record, static_cast<std::uint32_t>(schema.size()));
  // 每列带类型和长度，Decode 因而可以检测 schema/记录不一致或损坏。
  for (std::size_t i = 0; i < row.size(); ++i) {
    const auto &column = schema.At(i);
    if (row[i].type() != column.type) {
      return Result<std::vector<std::byte>>(Status::TypeMismatch(
          "Row 第 " + std::to_string(i + 1) + " 项与列 " + column.name + " 类型不匹配"));
    }
    record.push_back(std::byte{TypeTag(column.type)});
    if (column.type == DataType::Int) {
      // INT 固定以 8 字节补码位模式保存；转为 uint64_t 后移位不会受符号扩展影响。
      AppendU32(&record, sizeof(std::int64_t));
      AppendU64(&record, static_cast<std::uint64_t>(row[i].AsInt()));
    } else {
      const auto &text = row[i].AsVarchar();
      if (column.length.has_value() && text.size() > *column.length) {
        return Result<std::vector<std::byte>>(Status::InvalidArgument("字符串超过列长度: " + column.name));
      }
      if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::vector<std::byte>>(Status::InvalidArgument("字符串长度超过编码上限: " + column.name));
      }
      AppendU32(&record, static_cast<std::uint32_t>(text.size()));
      for (const auto character : text) {
        record.push_back(std::byte{static_cast<unsigned char>(character)});
      }
    }
  }
  return Result<std::vector<std::byte>>(std::move(record));
}

Result<Row> RowCodec::Decode(const Schema &schema, const std::vector<std::byte> &record) {
  if (!CanRead(record, 0, kRowHeaderSize)) {
    return Result<Row>(Status::InvalidArgument("Row 记录头部不完整"));
  }
  if (ReadU32(record, 0) != kRowMagic) {
    return Result<Row>(Status::InvalidArgument("Row 记录魔数不匹配"));
  }
  const auto column_count = ReadU32(record, 4);
  if (column_count != schema.size()) {
    return Result<Row>(Status::InvalidArgument("Row 列数量与 Schema 不符"));
  }
  Row row;
  row.reserve(schema.size());
  std::size_t offset = kRowHeaderSize;
  // Decode 对所有偏移做边界检查，不能相信磁盘中的字节一定合法。
  for (std::size_t i = 0; i < schema.size(); ++i) {
    if (!CanRead(record, offset, kFieldHeaderSize)) {
      return Result<Row>(Status::InvalidArgument("Row 字段头部越界: 第 " + std::to_string(i + 1) + " 项"));
    }
    const auto type_tag = static_cast<std::uint8_t>(record[offset]);
    const auto length = ReadU32(record, offset + 1);
    offset += kFieldHeaderSize;
    const auto &column = schema.At(i);
    if (type_tag != TypeTag(column.type)) {
      return Result<Row>(Status::TypeMismatch("Row 第 " + std::to_string(i + 1) + " 项类型标记不匹配"));
    }
    if (column.type == DataType::Int) {
      if (length != sizeof(std::int64_t) || !CanRead(record, offset, length)) {
        return Result<Row>(Status::InvalidArgument("Row INT 字段长度非法: 第 " + std::to_string(i + 1) + " 项"));
      }
      // 还原写入时的 64 位位模式，包括负数。
      row.emplace_back(static_cast<std::int64_t>(ReadU64(record, offset)));
    } else {
      if (!CanRead(record, offset, length)) {
        return Result<Row>(Status::InvalidArgument("Row VARCHAR 字段越界: 第 " + std::to_string(i + 1) + " 项"));
      }
      if (column.length.has_value() && length > *column.length) {
        return Result<Row>(Status::InvalidArgument("Row VARCHAR 超过列长度: " + column.name));
      }
      std::string text;
      text.reserve(length);
      for (std::size_t byte = 0; byte < length; ++byte) {
        text.push_back(static_cast<char>(record[offset + byte]));
      }
      row.emplace_back(std::move(text));
    }
    offset += length;
  }
  if (offset != record.size()) {
    return Result<Row>(Status::InvalidArgument("Row 记录包含多余或损坏数据"));
  }
  return Result<Row>(std::move(row));
}

}  // namespace oursql
