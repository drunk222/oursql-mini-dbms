#include "oursql/execution/database_engine.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::int64_t kRowCount = 10000;
constexpr std::int64_t kTargetKey = 8765;
constexpr std::size_t kPoolSize = 16;

struct TempDatabases {
  std::filesystem::path sequential;
  std::filesystem::path indexed;

  TempDatabases() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path();
    sequential = directory / ("oursql_seq_benchmark_" + std::to_string(stamp) + ".oursql");
    indexed = directory / ("oursql_index_benchmark_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDatabases() {
    std::error_code error;
    std::filesystem::remove(sequential, error);
    error.clear();
    std::filesystem::remove(indexed, error);
  }
};

struct Measurement {
  bool ok{false};
  double elapsed_ms{0.0};
  std::size_t rows{0};
  oursql::DatabaseStatistics statistics;
  std::string error;
};

bool BuildFixture(const TempDatabases &files) {
  {
    oursql::DatabaseEngine engine(files.sequential, 64);
    if (!engine.GetInitStatus().ok()) return false;
    auto created = engine.ExecuteSql("CREATE TABLE student(id INT, name VARCHAR(32));");
    if (!created.ok()) return false;
    constexpr std::int64_t kBatchSize = 200;
    for (std::int64_t first = 0; first < kRowCount; first += kBatchSize) {
      std::string sql;
      for (std::int64_t key = first; key < first + kBatchSize && key < kRowCount; ++key) {
        sql += "INSERT INTO student VALUES(" + std::to_string(key) + ", 'student_" +
               std::to_string(key) + "');";
      }
      auto inserted = engine.ExecuteSqlBatch(sql);
      if (!inserted.ok()) {
        std::cerr << "fixture insert failed: " << inserted.status().ToString() << '\n';
        return false;
      }
    }
    if (!engine.Close().ok()) return false;
  }
  std::error_code error;
  if (!std::filesystem::copy_file(files.sequential, files.indexed,
                                  std::filesystem::copy_options::overwrite_existing, error)) {
    std::cerr << "fixture copy failed: " << error.message() << '\n';
    return false;
  }
  oursql::DatabaseEngine indexed(files.indexed, 64);
  if (!indexed.GetInitStatus().ok()) return false;
  auto created = indexed.ExecuteSql("CREATE UNIQUE INDEX idx_student_id ON student(id);");
  if (!created.ok()) {
    std::cerr << "index build failed: " << created.status().ToString() << '\n';
    return false;
  }
  return indexed.Close().ok();
}

bool PlanContains(oursql::DatabaseEngine *engine, const std::string &needle) {
  auto explain = engine->ExecuteSql(
      "EXPLAIN SELECT * FROM student WHERE id = 8765;");
  return explain.ok() && explain.value().rows.size() == 1 &&
         explain.value().rows[0][0].AsVarchar().find(needle) != std::string::npos;
}

Measurement Measure(const std::filesystem::path &path, bool warm,
                    const std::string &expected_plan) {
  Measurement measurement;
  oursql::DatabaseEngine engine(path, kPoolSize);
  if (!engine.GetInitStatus().ok()) {
    measurement.error = engine.GetInitStatus().ToString();
    return measurement;
  }
  if (!PlanContains(&engine, expected_plan)) {
    measurement.error = "unexpected query plan";
    return measurement;
  }
  constexpr auto query = "SELECT * FROM student WHERE id = 8765;";
  if (warm) {
    auto warmup = engine.ExecuteSql(query);
    if (!warmup.ok()) {
      measurement.error = warmup.status().ToString();
      return measurement;
    }
  }
  if (!engine.ResetStatistics().ok()) {
    measurement.error = "statistics reset failed";
    return measurement;
  }
  const auto started = Clock::now();
  auto result = engine.ExecuteSql(query);
  const auto finished = Clock::now();
  if (!result.ok()) {
    measurement.error = result.status().ToString();
    return measurement;
  }
  measurement.elapsed_ms =
      std::chrono::duration<double, std::milli>(finished - started).count();
  measurement.rows = result.value().rows.size();
  measurement.statistics = engine.GetStatistics();
  measurement.ok = measurement.rows == 1 && result.value().rows[0][0].AsInt() == kTargetKey;
  if (!measurement.ok) measurement.error = "query result mismatch";
  return measurement;
}

void Print(const std::string &scenario, const Measurement &measurement) {
  if (!measurement.ok) {
    std::cout << std::left << std::setw(22) << scenario << " ERROR " << measurement.error << '\n';
    return;
  }
  const auto &stats = measurement.statistics;
  std::cout << std::left << std::setw(22) << scenario << std::right
            << std::setw(12) << std::fixed << std::setprecision(3) << measurement.elapsed_ms
            << std::setw(12) << stats.buffer_accesses
            << std::setw(12) << stats.buffer_hits
            << std::setw(12) << stats.buffer_misses
            << std::setw(12) << std::setprecision(2) << stats.buffer_hit_rate * 100.0
            << std::setw(12) << stats.evictions
            << std::setw(12) << stats.disk_reads
            << std::setw(12) << stats.disk_writes
            << std::setw(10) << measurement.rows << '\n';
}

}  // namespace

int main() {
  TempDatabases files;
  std::cout << "Preparing " << kRowCount << " rows...\n";
  if (!BuildFixture(files)) {
    std::cerr << "index benchmark fixture creation failed\n";
    return 1;
  }

  const auto cold_seq = Measure(files.sequential, false, "SeqScanPlan");
  const auto cold_index = Measure(files.indexed, false, "IndexScanPlan");
  const auto hot_seq = Measure(files.sequential, true, "SeqScanPlan");
  const auto hot_index = Measure(files.indexed, true, "IndexScanPlan");

  std::cout << "OurSQL index benchmark query: SELECT * FROM student WHERE id = "
            << kTargetKey << "; pool=" << kPoolSize << '\n';
  std::cout << std::left << std::setw(22) << "scenario" << std::right
            << std::setw(12) << "time(ms)" << std::setw(12) << "accesses"
            << std::setw(12) << "hits" << std::setw(12) << "misses"
            << std::setw(12) << "hit_rate%" << std::setw(12) << "evictions"
            << std::setw(12) << "disk_reads" << std::setw(12) << "disk_writes"
            << std::setw(10) << "rows" << '\n';
  Print("cold SeqScan", cold_seq);
  Print("cold IndexScan", cold_index);
  Print("warm SeqScan", hot_seq);
  Print("warm IndexScan", hot_index);
  return cold_seq.ok && cold_index.ok && hot_seq.ok && hot_index.ok ? 0 : 1;
}
