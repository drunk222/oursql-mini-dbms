#pragma once

#include "oursql/common/types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace oursql {

using table_id_t = std::uint32_t;
using index_id_t = std::uint32_t;

enum class LockResourceType : std::uint8_t {
  Schema,
  Table,
  Page,
  IndexKey,
};

enum class TableLockMode : std::uint8_t {
  IS,//准备在下层进行读操作     第一把事务逻辑锁通常是Schema IS
  IX,//我没有独占整个student表，但我准备在student的某些子资源上加X锁。然后才申请：page的X写锁
  S,
  SIX,//整体共享读，同时准备在部分子资源上写
  X,
};

enum class LockMode : std::uint8_t {
  S,
  X,
};

struct LockId {
  LockResourceType type{LockResourceType::Page};
  table_id_t table_id{0};
  page_id_t page_id{INVALID_PAGE_ID};
  index_id_t index_id{0};
  std::string encoded_key;

  static LockId Schema() noexcept {
    LockId lock;
    lock.type = LockResourceType::Schema;
    return lock;
  }

  static LockId Table(table_id_t id) noexcept {
    LockId lock;
    lock.type = LockResourceType::Table;
    lock.table_id = id;
    return lock;
  }

  static LockId Page(page_id_t id) noexcept {
    LockId lock;
    lock.type = LockResourceType::Page;
    lock.page_id = id;
    return lock;
  }

  static LockId IndexKey(index_id_t id, std::string key,
                         table_id_t table = 0) {
    LockId lock;
    lock.type = LockResourceType::IndexKey;
    lock.table_id = table;
    lock.index_id = id;
    lock.encoded_key = std::move(key);
    return lock;
  }

  friend bool operator==(const LockId &left, const LockId &right) noexcept {
    return left.type == right.type && left.table_id == right.table_id &&
           left.page_id == right.page_id && left.index_id == right.index_id &&
           left.encoded_key == right.encoded_key;
  }

  friend bool operator!=(const LockId &left, const LockId &right) noexcept {
    return !(left == right);
  }
};

struct LockIdHash {
  std::size_t operator()(const LockId &lock) const noexcept {
    std::size_t value = std::hash<std::uint8_t>{}(
        static_cast<std::uint8_t>(lock.type));
    value ^= std::hash<table_id_t>{}(lock.table_id) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    value ^= std::hash<page_id_t>{}(lock.page_id) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    value ^= std::hash<index_id_t>{}(lock.index_id) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    value ^= std::hash<std::string>{}(lock.encoded_key) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    return value;
  }
};

}  // namespace oursql
