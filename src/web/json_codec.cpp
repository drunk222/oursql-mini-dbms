#include "oursql/web/json_codec.h"

namespace oursql {

std::string ErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::NotImplemented: return "NotImplemented";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::TypeMismatch: return "TypeMismatch";
    case ErrorCode::OutOfSpace: return "OutOfSpace";
    case ErrorCode::RecordTooLarge: return "RecordTooLarge";
    case ErrorCode::IOError: return "IOError";
    case ErrorCode::InternalError: return "InternalError";
  }
  return "InternalError";
}

int HttpStatusFor(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return 200;
    case ErrorCode::InvalidArgument: return 400;
    case ErrorCode::NotFound: return 404;
    case ErrorCode::NotImplemented: return 422;
    case ErrorCode::AlreadyExists: return 409;
    case ErrorCode::TypeMismatch: return 422;
    case ErrorCode::OutOfSpace: return 507;
    case ErrorCode::RecordTooLarge: return 413;
    case ErrorCode::IOError:
    case ErrorCode::InternalError: return 500;
  }
  return 500;
}

nlohmann::json SuccessPayload(nlohmann::json data) {
  return nlohmann::json{{"ok", true}, {"data", std::move(data)}};
}

nlohmann::json ErrorPayload(const Status &status) {
  return nlohmann::json{
      {"ok", false},
      {"error",
       {{"code", ErrorCodeName(status.code())},
        {"message", status.message().empty() ? status.ToString() : status.message()}}}};
}

nlohmann::json ValueToJson(const Value &value) {
  switch (value.type()) {
    case DataType::Int:
      return nlohmann::json{{"type", "int"}, {"value", std::to_string(value.AsInt())}};
    case DataType::Varchar:
      return nlohmann::json{{"type", "string"}, {"value", value.AsVarchar()}};
    case DataType::Null:
      return nlohmann::json{{"type", "null"}, {"value", nullptr}};
  }
  return nlohmann::json{{"type", "null"}, {"value", nullptr}};
}

nlohmann::json RidToJson(const RID &rid) {
  return nlohmann::json{{"page_id", rid.page_id}, {"slot_id", rid.slot_id}};
}

nlohmann::json ExecutionResultToJson(const ExecutionResult &result) {
  auto rows = nlohmann::json::array();
  for (const auto &row : result.rows) {
    auto json_row = nlohmann::json::array();
    for (const auto &value : row) json_row.push_back(ValueToJson(value));
    rows.push_back(std::move(json_row));
  }

  auto rids = nlohmann::json::array();
  for (const auto &rid : result.rids) rids.push_back(RidToJson(rid));

  return nlohmann::json{
      {"columns", result.column_names},
      {"rows", std::move(rows)},
      {"rids", std::move(rids)},
      {"affected_rows", result.affected_rows},
  };
}

nlohmann::json ColumnToJson(const Column &column) {
  nlohmann::json result{
      {"name", column.name},
      {"type", column.type == DataType::Int ? "INT" : "VARCHAR"},
      {"nullable", column.nullable},
      {"primary_key", column.primary_key},
      {"unique", column.unique},
  };
  if (column.length.has_value()) {
    result["length"] = *column.length;
  } else {
    result["length"] = nullptr;
  }
  return result;
}

nlohmann::json TableMetadataToJson(const TableMetadata &table) {
  auto columns = nlohmann::json::array();
  for (const auto &column : table.schema.columns()) columns.push_back(ColumnToJson(column));
  return nlohmann::json{{"name", table.name}, {"columns", std::move(columns)}};
}

nlohmann::json StatisticsToJson(const DatabaseStatistics &statistics) {
  return nlohmann::json{
      {"buffer_accesses", statistics.buffer_accesses},
      {"buffer_hits", statistics.buffer_hits},
      {"buffer_misses", statistics.buffer_misses},
      {"buffer_hit_rate", statistics.buffer_hit_rate},
      {"evictions", statistics.evictions},
      {"disk_reads", statistics.disk_reads},
      {"disk_writes", statistics.disk_writes},
  };
}

nlohmann::json FrameSnapshotToJson(const FrameSnapshot &snapshot) {
  nlohmann::json result{
      {"frame_id", snapshot.frame_id},
      {"pin_count", snapshot.pin_count},
      {"is_dirty", snapshot.is_dirty},
      {"is_evictable", snapshot.is_evictable},
  };
  if (snapshot.page_id.has_value()) {
    result["page_id"] = *snapshot.page_id;
  } else {
    result["page_id"] = nullptr;
  }
  return result;
}

}  // namespace oursql
