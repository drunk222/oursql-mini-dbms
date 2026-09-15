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

// 表级锁采用多粒度意向锁；页和索引键使用普通共享锁 S、独占锁 X。
// IS/IX 表示事务准备在下层资源读/写，S/X 表示共享/独占整个表。
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
  // type 决定资源种类，其余字段只在对应资源类型下参与唯一标识。
  LockResourceType type{LockResourceType::Page};
  table_id_t table_id{0};
  page_id_t page_id{INVALID_PAGE_ID};
  index_id_t index_id{0};
  std::string encoded_key;

  // 调用者：锁管理器；作用：构造全局 Schema 资源标识；返回：Schema LockId。
  static LockId Schema() noexcept {
    LockId lock;
    lock.type = LockResourceType::Schema;
    return lock;
  }

  // 调用者：锁管理器；作用：构造表资源标识；返回：带 table_id 的 LockId。
  static LockId Table(table_id_t id) noexcept {
    LockId lock;
    lock.type = LockResourceType::Table;
    lock.table_id = id;
    return lock;
  }

  // 调用者：锁管理器/BufferPool；作用：构造页面资源标识；返回：带 page_id 的 LockId。
  static LockId Page(page_id_t id) noexcept {
    LockId lock;
    lock.type = LockResourceType::Page;
    lock.page_id = id;
    return lock;
  }

  // 调用者：索引锁路径；作用：构造索引键资源标识；返回：带索引和键的 LockId。
  static LockId IndexKey(index_id_t id, std::string key,
                         table_id_t table = 0) {
    LockId lock;
    lock.type = LockResourceType::IndexKey;
    lock.table_id = table;
    lock.index_id = id;
    lock.encoded_key = std::move(key);
    return lock;
  }

  // 调用者：unordered 容器/锁判断；作用：比较两个资源是否完全相同；返回：相等结果。
  friend bool operator==(const LockId &left, const LockId &right) noexcept {
    return left.type == right.type && left.table_id == right.table_id &&
           left.page_id == right.page_id && left.index_id == right.index_id &&
           left.encoded_key == right.encoded_key;
  }

  // 调用者：锁判断；作用：比较两个资源是否不同；返回：不等结果。
  friend bool operator!=(const LockId &left, const LockId &right) noexcept {
    return !(left == right);
  }
};

struct LockIdHash {
  // 调用者：unordered_map/set；作用：把资源类型和字段混合为哈希值；返回：哈希值。
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
