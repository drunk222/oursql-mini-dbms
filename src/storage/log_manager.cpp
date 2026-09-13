#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace oursql {

namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kTypeOffset = 6;
constexpr std::size_t kTotalLengthOffset = 8;
constexpr std::size_t kChecksumOffset = 12;
constexpr std::size_t kLsnOffset = 16;
constexpr std::size_t kTxnIdOffset = 24;
constexpr std::size_t kPrevLsnOffset = 32;
constexpr std::size_t kPageIdOffset = 40;
constexpr std::size_t kPayloadLengthOffset = 44;

void StoreU16(std::byte *data, std::size_t offset, std::uint16_t value) {
  data[offset] = std::byte{static_cast<unsigned char>(value & 0xffU)};
  data[offset + 1] = std::byte{static_cast<unsigned char>((value >> 8U) & 0xffU)};
}

void StoreU32(std::byte *data, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    data[offset + i] = std::byte{static_cast<unsigned char>((value >> (8U * i)) & 0xffU)};
  }
}

void StoreU64(std::byte *data, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    data[offset + i] = std::byte{static_cast<unsigned char>((value >> (8U * i)) & 0xffU)};
  }
}

std::uint16_t LoadU16(const std::byte *data, std::size_t offset) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned int>(data[offset])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned int>(data[offset + 1]) << 8U);
}

std::uint32_t LoadU32(const std::byte *data, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned int>(data[offset + i]))
             << (8U * i);
  }
  return value;
}

std::uint64_t LoadU64(const std::byte *data, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(data[offset + i]))
             << (8U * i);
  }
  return value;
}

std::uint32_t Crc32(const std::byte *data, std::size_t length,
                    std::size_t skipped_offset, std::size_t skipped_length) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < length; ++i) {
    if (i >= skipped_offset && i < skipped_offset + skipped_length) continue;
    crc ^= std::to_integer<unsigned int>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
    }
  }
  return crc ^ 0xffffffffU;
}

bool IsKnownType(std::uint16_t raw) {
  return raw >= static_cast<std::uint16_t>(LogRecordType::Begin) &&
         raw <= static_cast<std::uint16_t>(LogRecordType::Compensation);
}

std::string PathText(const std::filesystem::path &path) {
#ifdef _WIN32
  return path.u8string();
#else
  return path.string();
#endif
}

}  // namespace

LogManager::~LogManager() { (void)Close(); }

Status LogManager::EnsureOpenUnlocked() const {
  if (!open_) return Status::InvalidArgument("LogManager is not open");
  return Status::Ok();
}

Status LogManager::Open(const std::filesystem::path &database_path) {
  // Serialize lifecycle changes with Append. Flush intentionally does not
  // take this lock, so group commit can still run while records are appended.
  std::lock_guard<std::mutex> append_lock(append_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  flush_cv_.wait(lock, [&] { return !flush_in_progress_; });
  if (open_) {
    {
      std::lock_guard<std::mutex> file_lock(file_mutex_);
      file_.flush();
      file_.close();
    }
    open_ = false;
  }

  path_ = database_path;
  path_ += ".wal";
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    if (ec) return Status::IOError("cannot inspect WAL: " + PathText(path_));
    std::ofstream create(path_, std::ios::binary | std::ios::trunc);
    if (!create.is_open()) return Status::IOError("cannot create WAL: " + PathText(path_));
  }
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!file_.is_open()) return Status::IOError("cannot open WAL: " + PathText(path_));
  open_ = true;
  auto records = ScanUnlocked(true);
  if (!records.ok()) {
    file_.close();
    open_ = false;
    return records.status();
  }
  append_lsn_ = kInvalidLsn;
  for (const auto &record : records.value()) append_lsn_ = std::max(append_lsn_, record.lsn);
  durable_lsn_ = append_lsn_;
  flush_target_lsn_ = durable_lsn_;
  last_flush_status_ = Status::Ok();
  return Status::Ok();
}

Status LogManager::Close() {
  std::lock_guard<std::mutex> append_lock(append_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return Status::Ok();
  }
  const auto flush_status = Flush(GetAppendLsn());
  if (!flush_status.ok()) return flush_status;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return Status::Ok();
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  file_.flush();
  const bool good = static_cast<bool>(file_);
  file_.close();
  open_ = false;
  flush_target_lsn_ = kInvalidLsn;
  return good ? Status::Ok() : Status::IOError("cannot close WAL: " + PathText(path_));
}

Result<std::vector<std::byte>> LogManager::Encode(const LogRecord &record, lsn_t lsn) {
  const auto type = static_cast<std::uint16_t>(record.type);
  if (!IsKnownType(type)) return Result<std::vector<std::byte>>(Status::InvalidArgument("unknown WAL record type"));
  if (record.type == LogRecordType::PageUpdate &&
      (record.before_image.size() != Page::kSize || record.after_image.size() != Page::kSize)) {
    return Result<std::vector<std::byte>>(Status::InvalidArgument("PageUpdate requires two full page images"));
  }
  if (record.type == LogRecordType::Compensation && !record.before_image.empty()) {
    return Result<std::vector<std::byte>>(
        Status::InvalidArgument("Compensation cannot contain a before image"));
  }
  if (record.type == LogRecordType::Compensation &&
      !record.after_image.empty() && record.after_image.size() != Page::kSize) {
    return Result<std::vector<std::byte>>(
        Status::InvalidArgument("Compensation page image must be one full page"));
  }
  if (record.type == LogRecordType::Compensation &&
      record.compensation_type != LogRecordType::PageUpdate &&
      record.compensation_type != LogRecordType::PageAllocate &&
      record.compensation_type != LogRecordType::PageFree) {
    return Result<std::vector<std::byte>>(
        Status::InvalidArgument("Compensation target type is invalid"));
  }
  if (record.type == LogRecordType::Compensation &&
      ((record.compensation_type == LogRecordType::PageUpdate &&
        record.after_image.size() != Page::kSize) ||
       (record.compensation_type != LogRecordType::PageUpdate &&
        !record.after_image.empty()))) {
    return Result<std::vector<std::byte>>(
        Status::InvalidArgument("Compensation image does not match its target type"));
  }
  if (record.type != LogRecordType::PageUpdate && record.type != LogRecordType::Compensation &&
      (!record.before_image.empty() || !record.after_image.empty())) {
    return Result<std::vector<std::byte>>(
        Status::InvalidArgument("only PageUpdate or Compensation may contain images"));
  }
  std::vector<std::byte> payload;
  payload.reserve(record.before_image.size() + record.after_image.size() + 12);
  if (record.type == LogRecordType::Compensation) {
    payload.resize(12, std::byte{0});
    StoreU64(payload.data(), 0, record.undo_next_lsn);
    StoreU16(payload.data(), 8, static_cast<std::uint16_t>(record.compensation_type));
  }
  payload.insert(payload.end(), record.before_image.begin(), record.before_image.end());
  payload.insert(payload.end(), record.after_image.begin(), record.after_image.end());
  if (payload.size() > std::numeric_limits<std::uint32_t>::max() - LogManager::kHeaderSize) {
    return Result<std::vector<std::byte>>(Status::RecordTooLarge("WAL record is too large"));
  }
  const auto total = LogManager::kHeaderSize + payload.size();
  std::vector<std::byte> bytes(total, std::byte{0});
  StoreU32(bytes.data(), kMagicOffset, LogManager::kMagic);
  StoreU16(bytes.data(), kVersionOffset, LogManager::kVersion);
  StoreU16(bytes.data(), kTypeOffset, type);
  StoreU32(bytes.data(), kTotalLengthOffset, static_cast<std::uint32_t>(total));
  StoreU32(bytes.data(), kChecksumOffset, 0);
  StoreU64(bytes.data(), kLsnOffset, lsn);
  StoreU64(bytes.data(), kTxnIdOffset, record.txn_id);
  StoreU64(bytes.data(), kPrevLsnOffset, record.prev_lsn);
  StoreU32(bytes.data(), kPageIdOffset, record.page_id);
  StoreU32(bytes.data(), kPayloadLengthOffset, static_cast<std::uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(), bytes.begin() + LogManager::kHeaderSize);
  StoreU32(bytes.data(), kChecksumOffset,
           Crc32(bytes.data(), bytes.size(), kChecksumOffset, sizeof(std::uint32_t)));
  return Result<std::vector<std::byte>>(std::move(bytes));
}

Result<LogRecord> LogManager::Decode(const std::byte *data, std::size_t length) {
  if (data == nullptr || length < kHeaderSize) {
    return Result<LogRecord>(Status::IOError("WAL record header is truncated"));
  }
  if (LoadU32(data, kMagicOffset) != kMagic || LoadU16(data, kVersionOffset) != kVersion) {
    return Result<LogRecord>(Status::IOError("WAL record magic or version is invalid"));
  }
  const auto type = LoadU16(data, kTypeOffset);
  const auto total = LoadU32(data, kTotalLengthOffset);
  const auto payload_length = LoadU32(data, kPayloadLengthOffset);
  if (!IsKnownType(type) || total < kHeaderSize || total != length ||
      payload_length != total - kHeaderSize) {
    return Result<LogRecord>(Status::IOError("WAL record length or type is invalid"));
  }
  const auto expected = LoadU32(data, kChecksumOffset);
  const auto actual = Crc32(data, length, kChecksumOffset, sizeof(std::uint32_t));
  if (expected != actual) return Result<LogRecord>(Status::IOError("WAL record checksum mismatch"));

  LogRecord record;
  record.type = static_cast<LogRecordType>(type);
  record.lsn = LoadU64(data, kLsnOffset);
  record.txn_id = LoadU64(data, kTxnIdOffset);
  record.prev_lsn = LoadU64(data, kPrevLsnOffset);
  record.page_id = static_cast<page_id_t>(LoadU32(data, kPageIdOffset));
  if (record.type == LogRecordType::PageUpdate) {
    if (payload_length != Page::kSize * 2) {
      return Result<LogRecord>(Status::IOError("PageUpdate image length is invalid"));
    }
    record.before_image.assign(data + kHeaderSize, data + kHeaderSize + Page::kSize);
    record.after_image.assign(data + kHeaderSize + Page::kSize, data + length);
  } else if (record.type == LogRecordType::Compensation) {
    if (payload_length != 12 && payload_length != 12 + Page::kSize) {
      return Result<LogRecord>(Status::IOError("Compensation payload length is invalid"));
    }
    record.undo_next_lsn = LoadU64(data, kHeaderSize);
    const auto compensation_type = LoadU16(data, kHeaderSize + 8);
    if (compensation_type != static_cast<std::uint16_t>(LogRecordType::PageUpdate) &&
        compensation_type != static_cast<std::uint16_t>(LogRecordType::PageAllocate) &&
        compensation_type != static_cast<std::uint16_t>(LogRecordType::PageFree)) {
      return Result<LogRecord>(Status::IOError("Compensation target type is invalid"));
    }
    record.compensation_type = static_cast<LogRecordType>(compensation_type);
    if (payload_length == 12 + Page::kSize) {
      record.after_image.assign(data + kHeaderSize + 12, data + length);
    }
  } else if (payload_length != 0) {
    return Result<LogRecord>(Status::IOError("non-update WAL record has a payload"));
  }
  return Result<LogRecord>(std::move(record));
}

Result<lsn_t> LogManager::Append(LogRecord record) {
  std::lock_guard<std::mutex> append_lock(append_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  auto open_status = EnsureOpenUnlocked();
  if (!open_status.ok()) return Result<lsn_t>(open_status);
  const lsn_t lsn = append_lsn_ + 1;
  auto encoded = Encode(record, lsn);
  if (!encoded.ok()) return Result<lsn_t>(encoded.status());
  {
    std::lock_guard<std::mutex> file_lock(file_mutex_);
    file_.clear();
    file_.seekp(0, std::ios::end);
    if (!file_) return Result<lsn_t>(Status::IOError("cannot seek WAL append position"));
    file_.write(reinterpret_cast<const char *>(encoded.value().data()),
                static_cast<std::streamsize>(encoded.value().size()));
    if (!file_) return Result<lsn_t>(Status::IOError("cannot append WAL record"));
  }
  append_lsn_ = lsn;
  return Result<lsn_t>(lsn);
}

Result<std::vector<LogRecord>> LogManager::ScanUnlocked(bool repair_tail) const {
  auto open_status = EnsureOpenUnlocked();
  if (!open_status.ok()) return Result<std::vector<LogRecord>>(open_status);
  file_.clear();
  file_.seekg(0, std::ios::end);
  const auto end = file_.tellg();
  if (end < 0) return Result<std::vector<LogRecord>>(Status::IOError("cannot size WAL"));
  const auto file_size = static_cast<std::size_t>(end);
  file_.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(file_size);
  if (file_size != 0) {
    file_.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(file_size));
    if (file_.gcount() != static_cast<std::streamsize>(file_size)) {
      return Result<std::vector<LogRecord>>(Status::IOError("cannot read WAL"));
    }
  }

  std::vector<LogRecord> records;
  std::size_t offset = 0;
  lsn_t previous_lsn = kInvalidLsn;
  std::unordered_set<lsn_t> seen_lsns;
  std::unordered_map<txn_id_t, lsn_t> last_lsn_by_transaction;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining < kHeaderSize) {
      if (!repair_tail) return Result<std::vector<LogRecord>>(Status::IOError("WAL tail is truncated"));
      break;
    }
    const auto total = LoadU32(bytes.data() + offset, kTotalLengthOffset);
    if (total < kHeaderSize || total > remaining) {
      if (!repair_tail) return Result<std::vector<LogRecord>>(Status::IOError("WAL tail length is invalid"));
      break;
    }
    auto decoded = Decode(bytes.data() + offset, total);
    if (!decoded.ok()) {
      if (offset + total != bytes.size()) {
        return Result<std::vector<LogRecord>>(Status::IOError("WAL corruption before file tail: " +
                                                              decoded.status().message()));
      }
      if (!repair_tail) return Result<std::vector<LogRecord>>(decoded.status());
      break;
    }
    const auto &record = decoded.value();
    if (record.lsn == kInvalidLsn || record.lsn <= previous_lsn ||
        !seen_lsns.insert(record.lsn).second) {
      return Result<std::vector<LogRecord>>(Status::IOError("WAL LSN chain is invalid"));
    }
    if (record.txn_id == kInvalidTxnId) {
      if (record.prev_lsn != kInvalidLsn) {
        return Result<std::vector<LogRecord>>(
            Status::IOError("WAL non-transaction record has a prev_lsn"));
      }
    } else {
      const auto previous_transaction_lsn =
          last_lsn_by_transaction.find(record.txn_id);
      const auto expected_prev = previous_transaction_lsn == last_lsn_by_transaction.end()
                                     ? kInvalidLsn
                                     : previous_transaction_lsn->second;
      if (record.prev_lsn != expected_prev ||
          (record.prev_lsn != kInvalidLsn &&
           seen_lsns.find(record.prev_lsn) == seen_lsns.end())) {
        return Result<std::vector<LogRecord>>(
            Status::IOError("WAL transaction prev_lsn chain is invalid"));
      }
      last_lsn_by_transaction[record.txn_id] = record.lsn;
    }
    previous_lsn = record.lsn;
    records.push_back(std::move(decoded.value()));
    offset += total;
  }
  if (repair_tail && offset < bytes.size()) {
    file_.flush();
    file_.close();
    std::error_code ec;
    std::filesystem::resize_file(path_, offset, ec);
    if (ec) return Result<std::vector<LogRecord>>(Status::IOError("cannot trim WAL tail: " + ec.message()));
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_.is_open()) return Result<std::vector<LogRecord>>(Status::IOError("cannot reopen trimmed WAL"));
  }
  return Result<std::vector<LogRecord>>(std::move(records));
}

Result<std::vector<LogRecord>> LogManager::Scan() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  return ScanUnlocked(false);
}

Result<txn_id_t> LogManager::GetNextTransactionIdSeed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  auto records = ScanUnlocked(false);
  if (!records.ok()) return Result<txn_id_t>(records.status());
  txn_id_t max_txn_id = kInvalidTxnId;
  for (const auto &record : records.value()) {
    max_txn_id = std::max(max_txn_id, record.txn_id);
  }
  if (max_txn_id == std::numeric_limits<txn_id_t>::max()) {
    return Result<txn_id_t>(Status::OutOfSpace("transaction id space is exhausted"));
  }
  return Result<txn_id_t>(max_txn_id + 1);
}

Status LogManager::Flush(lsn_t target_lsn) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto open_status = EnsureOpenUnlocked();
  if (!open_status.ok()) return open_status;
  if (target_lsn > append_lsn_) {
    return Status::InvalidArgument("WAL flush target exceeds append LSN");
  }
  if (durable_lsn_ >= target_lsn) return Status::Ok();
  flush_target_lsn_ = std::max(flush_target_lsn_, target_lsn);
  if (flush_in_progress_) {
    const auto generation = flush_generation_;
    flush_cv_.wait(lock, [&] {
      return durable_lsn_ >= target_lsn ||
             (!flush_in_progress_ && flush_generation_ != generation);
    });
    if (durable_lsn_ >= target_lsn) return Status::Ok();
    return last_flush_status_;
  }

  flush_in_progress_ = true;
  lock.unlock();
  while (true) {
    lock.lock();
    const auto batch_target = flush_target_lsn_;
    const bool inject_failure = fail_next_flush_for_testing_;
    fail_next_flush_for_testing_ = false;
    lock.unlock();

    Status status = Status::Ok();
    if (inject_failure) {
      status = Status::IOError("injected WAL flush failure");
    } else {
      std::lock_guard<std::mutex> file_lock(file_mutex_);
      file_.flush();
      if (!file_) status = Status::IOError("cannot flush WAL");
    }

    lock.lock();
    if (!status.ok()) {
      last_flush_status_ = status;
      flush_in_progress_ = false;
      ++flush_generation_;
      flush_cv_.notify_all();
      return status;
    }
    durable_lsn_ = std::max(durable_lsn_, batch_target);
    if (durable_lsn_ < flush_target_lsn_) {
      lock.unlock();
      continue;
    }
    last_flush_status_ = Status::Ok();
    flush_in_progress_ = false;
    ++flush_generation_;
    flush_cv_.notify_all();
    return Status::Ok();
  }
}

void LogManager::FailNextFlushForTesting() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  fail_next_flush_for_testing_ = true;
}

Status LogManager::Truncate() {
  std::lock_guard<std::mutex> append_lock(append_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  flush_cv_.wait(lock, [&] { return !flush_in_progress_; });
  auto open_status = EnsureOpenUnlocked();
  if (!open_status.ok()) return open_status;
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  file_.flush();
  file_.close();
  std::error_code ec;
  std::filesystem::resize_file(path_, 0, ec);
  if (ec) return Status::IOError("cannot truncate WAL: " + ec.message());
  file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!file_.is_open()) return Status::IOError("cannot reopen truncated WAL");
  append_lsn_ = kInvalidLsn;
  durable_lsn_ = kInvalidLsn;
  flush_target_lsn_ = kInvalidLsn;
  last_flush_status_ = Status::Ok();
  return Status::Ok();
}

lsn_t LogManager::GetAppendLsn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return append_lsn_;
}

lsn_t LogManager::GetDurableLsn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return durable_lsn_;
}

}  // namespace oursql
