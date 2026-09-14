#pragma once

#include "oursql/common/status.h"
#include "oursql/execution/database_engine.h"

#include <nlohmann/json.hpp>

#include <string>

namespace oursql {

[[nodiscard]] std::string ErrorCodeName(ErrorCode code);
[[nodiscard]] int HttpStatusFor(ErrorCode code);

[[nodiscard]] nlohmann::json SuccessPayload(nlohmann::json data);
[[nodiscard]] nlohmann::json ErrorPayload(const Status &status);

[[nodiscard]] nlohmann::json ValueToJson(const Value &value);
[[nodiscard]] nlohmann::json RidToJson(const RID &rid);
[[nodiscard]] nlohmann::json ExecutionResultToJson(const ExecutionResult &result);
[[nodiscard]] nlohmann::json ColumnToJson(const Column &column);
[[nodiscard]] nlohmann::json TableMetadataToJson(const TableMetadata &table);
[[nodiscard]] nlohmann::json StatisticsToJson(const DatabaseStatistics &statistics);
[[nodiscard]] nlohmann::json FrameSnapshotToJson(const FrameSnapshot &snapshot);

}  // namespace oursql
