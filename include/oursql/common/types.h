#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>

namespace oursql {

using page_id_t = std::uint32_t;
using slot_id_t = std::uint32_t;
inline constexpr page_id_t INVALID_PAGE_ID = std::numeric_limits<page_id_t>::max();

enum class DataType {
  Int,
  Varchar,
};

struct RID {
  page_id_t page_id{0};
  slot_id_t slot_id{0};

  constexpr RID() = default;
  constexpr RID(page_id_t page, slot_id_t slot) noexcept : page_id(page), slot_id(slot) {}
};

constexpr bool operator==(const RID &lhs, const RID &rhs) noexcept {
  return lhs.page_id == rhs.page_id && lhs.slot_id == rhs.slot_id;
}

constexpr bool operator!=(const RID &lhs, const RID &rhs) noexcept {
  return !(lhs == rhs);
}

}  // namespace oursql
