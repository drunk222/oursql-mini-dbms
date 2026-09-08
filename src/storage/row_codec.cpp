#include "oursql/storage/row_codec.h"

#include <limits>

namespace oursql {

namespace {

constexpr std::uint32_t kRowMagic = 0x31574F52U;
constexpr std::size_t kRowHeaderSize = 8;
constexpr std::size_t kFieldHeaderSize = 5;

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
  for (std::size_t i = 0; i < row.size(); ++i) {
    const auto &column = schema.At(i);
    if (row[i].type() != column.type) {
      return Result<std::vector<std::byte>>(Status::TypeMismatch(
          "Row 第 " + std::to_string(i + 1) + " 项与列 " + column.name + " 类型不匹配"));
    }
    record.push_back(std::byte{TypeTag(column.type)});
    if (column.type == DataType::Int) {
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
