#include "oursql/web/performance_benchmark.h"

#include "oursql/execution/database_engine.h"
#include "oursql/parser/lexer.h"
#include "oursql/parser/parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace oursql {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kPoolSize = 16;

struct TempDatabases {
  std::filesystem::path sequential;
  std::filesystem::path indexed;

  TempDatabases() {
    const auto stamp = Clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path();
    sequential = directory / ("oursql_web_seq_" + std::to_string(stamp) + ".oursql");
    indexed = directory / ("oursql_web_index_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDatabases() {
    std::error_code error;
    for (const auto &path : {sequential, indexed}) {
      std::filesystem::remove(path, error);
      error.clear();
      std::filesystem::remove(path.string() + ".wal", error);
      error.clear();
    }
  }
};

struct Sample {
  double elapsed_ms{0.0};
  DatabaseStatistics statistics;
  std::size_t rows{0};
};

Status BuildFixture(const TempDatabases &files, std::size_t row_count) {
  {
    DatabaseEngine engine(files.sequential, 64);
    if (!engine.GetInitStatus().ok()) return engine.GetInitStatus();
    auto created = engine.ExecuteSql(
        "CREATE TABLE student(id INT, name VARCHAR(32));");
    if (!created.ok()) return created.status();

    constexpr std::size_t kBatchSize = 200;
    for (std::size_t first = 0; first < row_count; first += kBatchSize) {
      std::string sql = "INSERT INTO student VALUES";
      const std::size_t last = std::min(row_count, first + kBatchSize);
      for (std::size_t key = first; key < last; ++key) {
        if (key != first) sql += ',';
        sql += "(" + std::to_string(key) + ", 'student_" +
               std::to_string(key) + "')";
      }
      sql += ';';
      auto inserted = engine.ExecuteSqlBatch(sql);
      if (!inserted.ok()) return inserted.status();
    }
    auto closed = engine.Close();
    if (!closed.ok()) return closed;
  }

  std::error_code error;
  if (!std::filesystem::copy_file(
          files.sequential, files.indexed,
          std::filesystem::copy_options::overwrite_existing, error)) {
    return Status::IOError("Unable to copy benchmark fixture: " + error.message());
  }

  DatabaseEngine indexed(files.indexed, 64);
  if (!indexed.GetInitStatus().ok()) return indexed.GetInitStatus();
  auto created = indexed.ExecuteSql(
      "CREATE UNIQUE INDEX idx_student_id ON student(id);");
  if (!created.ok()) return created.status();
  return indexed.Close();
}

Result<std::string> ReadPlan(const std::filesystem::path &path,
                             const std::string &query) {
  DatabaseEngine engine(path, kPoolSize);
  if (!engine.GetInitStatus().ok()) {
    return Result<std::string>(engine.GetInitStatus());
  }
  auto explained = engine.ExecuteSql("EXPLAIN " + query);
  if (!explained.ok()) return Result<std::string>(explained.status());
  if (explained.value().rows.empty() || explained.value().rows[0].empty() ||
      !explained.value().rows[0][0].IsVarchar()) {
    return Result<std::string>(
        Status::InternalError("Benchmark EXPLAIN returned no plan"));
  }
  return Result<std::string>(explained.value().rows[0][0].AsVarchar());
}

Result<Sample> ExecuteMeasured(DatabaseEngine *engine,
                               const std::string &query) {
  auto reset = engine->ResetStatistics();
  if (!reset.ok()) return Result<Sample>(reset);
  const auto started = Clock::now();
  auto result = engine->ExecuteSql(query);
  const auto finished = Clock::now();
  if (!result.ok()) return Result<Sample>(result.status());
  Sample sample;
  sample.elapsed_ms =
      std::chrono::duration<double, std::milli>(finished - started).count();
  sample.statistics = engine->GetStatistics();
  sample.rows = result.value().rows.size();
  return Result<Sample>(std::move(sample));
}

Result<std::vector<Sample>> MeasureCold(const std::filesystem::path &path,
                                        const std::string &query,
                                        std::size_t repetitions) {
  std::vector<Sample> samples;
  samples.reserve(repetitions);
  for (std::size_t run = 0; run < repetitions; ++run) {
    DatabaseEngine engine(path, kPoolSize);
    if (!engine.GetInitStatus().ok()) {
      return Result<std::vector<Sample>>(engine.GetInitStatus());
    }
    auto sample = ExecuteMeasured(&engine, query);
    if (!sample.ok()) return Result<std::vector<Sample>>(sample.status());
    samples.push_back(std::move(sample.value()));
  }
  return Result<std::vector<Sample>>(std::move(samples));
}

Result<nlohmann::json> BuildCompilerTrace(const std::string &query,
                                          std::size_t expression_left,
                                          std::size_t target_key,
                                          const std::string &seq_plan,
                                          const std::string &index_plan) {
  Lexer lexer;
  auto tokens = lexer.Tokenize(query);
  if (!tokens.ok()) return Result<nlohmann::json>(tokens.status());
  auto token_json = nlohmann::json::array();
  for (const auto &token : tokens.value()) {
    if (token.type == TokenType::EndOfFile) continue;
    token_json.push_back({{"type", TokenTypeName(token.type)},
                          {"lexeme", token.lexeme}});
  }

  Parser parser;
  auto statements = parser.Parse(query);
  if (!statements.ok()) return Result<nlohmann::json>(statements.status());
  if (statements.value().size() != 1) {
    return Result<nlohmann::json>(
        Status::InternalError("Benchmark compiler trace expected one statement"));
  }

  return Result<nlohmann::json>(nlohmann::json{
      {"tokens", std::move(token_json)},
      {"ast", ToString(statements.value().front())},
      {"expression_before", std::to_string(expression_left) + " + 5"},
      {"expression_after", std::to_string(target_key)},
      {"rules",
       nlohmann::json::array(
           {"常量折叠：" + std::to_string(expression_left) + " + 5 → " +
                std::to_string(target_key),
            "等值谓词识别：id = " + std::to_string(target_key),
            "R2：匹配 idx_student_id，SeqScan → IndexScan",
            "R3：基于选择率成本保留 IndexScan"})},
      {"plan_before", seq_plan},
      {"plan_after", index_plan}});
}

double MedianElapsed(const std::vector<Sample> &samples) {
  std::vector<double> values;
  values.reserve(samples.size());
  for (const auto &sample : samples) values.push_back(sample.elapsed_ms);
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  return values.size() % 2 == 0
             ? (values[middle - 1] + values[middle]) / 2.0
             : values[middle];
}

double AverageCounter(const std::vector<Sample> &samples,
                      std::uint64_t DatabaseStatistics::*field) {
  const double total = std::accumulate(
      samples.begin(), samples.end(), 0.0,
      [field](double sum, const Sample &sample) {
        return sum + static_cast<double>(sample.statistics.*field);
      });
  return total / static_cast<double>(samples.size());
}

nlohmann::json ScenarioJson(const std::string &id, const std::string &label,
                            const std::string &cache_state,
                            const std::string &access_path,
                            const std::string &plan,
                            const std::vector<Sample> &samples) {
  const double accesses = AverageCounter(samples, &DatabaseStatistics::buffer_accesses);
  const double hits = AverageCounter(samples, &DatabaseStatistics::buffer_hits);
  const double misses = AverageCounter(samples, &DatabaseStatistics::buffer_misses);
  const double hit_rate = accesses == 0.0 ? 0.0 : hits / accesses;
  return nlohmann::json{
      {"id", id},
      {"label", label},
      {"cache_state", cache_state},
      {"access_path", access_path},
      {"plan", plan},
      {"median_ms", MedianElapsed(samples)},
      {"sample_ms", [&samples] {
         auto values = nlohmann::json::array();
         for (const auto &sample : samples) values.push_back(sample.elapsed_ms);
         return values;
       }()},
      {"rows", samples.front().rows},
      {"statistics",
       {{"buffer_accesses", accesses},
        {"buffer_hits", hits},
        {"buffer_misses", misses},
        {"buffer_hit_rate", hit_rate},
        {"evictions", AverageCounter(samples, &DatabaseStatistics::evictions)},
        {"disk_reads", AverageCounter(samples, &DatabaseStatistics::disk_reads)},
        {"disk_writes", AverageCounter(samples, &DatabaseStatistics::disk_writes)}}}};
}

Status BuildSelectivityFixture(const std::filesystem::path &path,
                               std::size_t row_count) {
  DatabaseEngine engine(path, 64);
  if (!engine.GetInitStatus().ok()) return engine.GetInitStatus();
  auto created = engine.ExecuteSql(
      "CREATE TABLE student(id INT, gender INT, name VARCHAR(32));");
  if (!created.ok()) return created.status();
  constexpr std::size_t kBatchSize = 200;
  const std::size_t rare_count = row_count / 10;
  for (std::size_t first = 0; first < row_count; first += kBatchSize) {
    std::string sql = "INSERT INTO student VALUES";
    const std::size_t last = std::min(row_count, first + kBatchSize);
    for (std::size_t key = first; key < last; ++key) {
      if (key != first) sql += ',';
      const int gender = key < rare_count ? 0 : 1;
      sql += "(" + std::to_string(key) + "," + std::to_string(gender) +
             ",'student_" + std::to_string(key) + "')";
    }
    sql += ';';
    auto inserted = engine.ExecuteSqlBatch(sql);
    if (!inserted.ok()) return inserted.status();
  }
  auto indexed = engine.ExecuteSql(
      "CREATE INDEX idx_student_gender ON student(gender);");
  if (!indexed.ok()) return indexed.status();
  return engine.Close();
}

struct TempBufferDatabase {
  std::filesystem::path path;

  TempBufferDatabase() {
    const auto stamp = Clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_web_buffer_" + std::to_string(stamp) + ".oursql");
  }
  ~TempBufferDatabase() {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
};

Status BuildBufferFixture(const std::filesystem::path &path) {
  DiskManager disk;
  auto opened = disk.Open(path);
  if (!opened.ok()) return opened;
  for (int index = 0; index < 32; ++index) {
    auto page_id = disk.AllocatePage();
    if (!page_id.ok()) return page_id.status();
    Page page;
    page.Data()[0] = std::byte{static_cast<unsigned char>(index)};
    auto written = disk.WritePage(page_id.value(), page);
    if (!written.ok()) return written;
  }
  return disk.Close();
}

std::vector<page_id_t> BufferRequests(const std::string &pattern) {
  constexpr int kPageCount = 32;
  constexpr int kRequestCount = 128;
  std::vector<page_id_t> requests;
  requests.reserve(kRequestCount);
  std::mt19937 generator(20260908U);
  if (pattern == "sequential") {
    for (int index = 0; index < kRequestCount; ++index) {
      requests.push_back(static_cast<page_id_t>(1 + index % kPageCount));
    }
  } else if (pattern == "random") {
    std::uniform_int_distribution<int> page(1, kPageCount);
    for (int index = 0; index < kRequestCount; ++index) {
      requests.push_back(static_cast<page_id_t>(page(generator)));
    }
  } else {
    std::uniform_int_distribution<int> hot(1, 4);
    std::uniform_int_distribution<int> cold(5, kPageCount);
    std::bernoulli_distribution choose_hot(0.8);
    for (int index = 0; index < kRequestCount; ++index) {
      requests.push_back(static_cast<page_id_t>(
          choose_hot(generator) ? hot(generator) : cold(generator)));
    }
  }
  return requests;
}

Result<nlohmann::json> MeasureBufferPolicy(
    const std::filesystem::path &path, const std::vector<page_id_t> &requests,
    ReplacementPolicy policy, const std::string &name) {
  DiskManager disk;
  auto opened = disk.Open(path);
  if (!opened.ok()) return Result<nlohmann::json>(opened);
  BufferPoolManager buffer_pool(4, &disk, policy, FlushPolicy::WriteBack);
  if (!buffer_pool.GetInitStatus().ok()) {
    return Result<nlohmann::json>(buffer_pool.GetInitStatus());
  }
  const auto started = Clock::now();
  for (const auto page_id : requests) {
    auto page = buffer_pool.FetchPage(page_id);
    if (!page.ok()) return Result<nlohmann::json>(page.status());
    auto guard = std::move(page.value());
  }
  const auto finished = Clock::now();
  const auto hit_rate = buffer_pool.GetHitRate();
  nlohmann::json result{
      {"policy", name},
      {"elapsed_ms", std::chrono::duration<double, std::milli>(finished - started).count()},
      {"accesses", buffer_pool.GetAccessCount()},
      {"hits", buffer_pool.GetHitCount()},
      {"misses", buffer_pool.GetMissCount()},
      {"hit_rate", hit_rate},
      {"evictions", buffer_pool.GetEvictionCount()},
      {"disk_reads", disk.GetReadCount()},
      {"disk_writes", disk.GetWriteCount()},
      {"eviction_log", buffer_pool.GetEvictionLog()}};
  auto closed = buffer_pool.Close();
  if (!closed.ok()) return Result<nlohmann::json>(closed);
  closed = disk.Close();
  if (!closed.ok()) return Result<nlohmann::json>(closed);
  return Result<nlohmann::json>(std::move(result));
}

}  // namespace

Result<nlohmann::json> PerformanceBenchmarkService::RunIndexComparison(
    std::size_t row_count, std::size_t repetitions) const {
  if (row_count < 100 || row_count > 20000) {
    return Result<nlohmann::json>(Status::InvalidArgument(
        "row_count must be between 100 and 20000"));
  }
  if (repetitions < 1 || repetitions > 7) {
    return Result<nlohmann::json>(Status::InvalidArgument(
        "repetitions must be between 1 and 7"));
  }

  TempDatabases files;
  auto fixture = BuildFixture(files, row_count);
  if (!fixture.ok()) return Result<nlohmann::json>(fixture);

  const std::size_t target_key = row_count * 7 / 8;
  const std::size_t expression_left = target_key - 5;
  const std::string query = "SELECT * FROM student WHERE id = " +
                            std::to_string(expression_left) + " + 5;";
  auto seq_plan = ReadPlan(files.sequential, query);
  if (!seq_plan.ok()) return Result<nlohmann::json>(seq_plan.status());
  auto index_plan = ReadPlan(files.indexed, query);
  if (!index_plan.ok()) return Result<nlohmann::json>(index_plan.status());
  if (seq_plan.value().find("SeqScanPlan") == std::string::npos ||
      index_plan.value().find("IndexScanPlan") == std::string::npos) {
    return Result<nlohmann::json>(Status::InternalError(
        "Benchmark access paths did not match SeqScan and IndexScan"));
  }

  auto cold_seq = MeasureCold(files.sequential, query, repetitions);
  if (!cold_seq.ok()) return Result<nlohmann::json>(cold_seq.status());
  auto cold_index = MeasureCold(files.indexed, query, repetitions);
  if (!cold_index.ok()) return Result<nlohmann::json>(cold_index.status());
  auto compiler = BuildCompilerTrace(query, expression_left, target_key,
                                     seq_plan.value(), index_plan.value());
  if (!compiler.ok()) return Result<nlohmann::json>(compiler.status());

  const double cold_seq_ms = MedianElapsed(cold_seq.value());
  const double cold_index_ms = MedianElapsed(cold_index.value());
  auto scenarios = nlohmann::json::array();
  scenarios.push_back(ScenarioJson("cold-seq", "首次查询 · 全表扫描", "cold",
                                   "SeqScan", seq_plan.value(), cold_seq.value()));
  scenarios.push_back(ScenarioJson("cold-index", "首次查询 · B+ 树索引", "cold",
                                   "IndexScan", index_plan.value(), cold_index.value()));
  return Result<nlohmann::json>(nlohmann::json{
      {"metadata",
       {{"row_count", row_count},
        {"target_key", target_key},
        {"pool_size", kPoolSize},
        {"repetitions", repetitions},
        {"query", query},
        {"timing", "server-side steady_clock median"}}},
      {"comparisons",
       {{"speedup", cold_index_ms == 0.0 ? 0.0 : cold_seq_ms / cold_index_ms},
        {"saved_ms", cold_seq_ms - cold_index_ms}}},
      {"compiler", std::move(compiler.value())},
      {"scenarios", std::move(scenarios)}});
}

Result<nlohmann::json> PerformanceBenchmarkService::RunSelectivityComparison(
    std::size_t row_count, std::size_t repetitions) const {
  if (row_count < 100 || row_count > 20000 || row_count % 10 != 0) {
    return Result<nlohmann::json>(Status::InvalidArgument(
        "row_count must be a multiple of 10 between 100 and 20000"));
  }
  if (repetitions < 1 || repetitions > 7) {
    return Result<nlohmann::json>(Status::InvalidArgument(
        "repetitions must be between 1 and 7"));
  }

  TempDatabases files;
  auto fixture = BuildSelectivityFixture(files.indexed, row_count);
  if (!fixture.ok()) return Result<nlohmann::json>(fixture);
  const std::string common_query =
      "SELECT * FROM student WHERE gender = 1;";
  const std::string rare_query =
      "SELECT * FROM student WHERE gender = 0;";
  auto common_plan = ReadPlan(files.indexed, common_query);
  if (!common_plan.ok()) return Result<nlohmann::json>(common_plan.status());
  auto rare_plan = ReadPlan(files.indexed, rare_query);
  if (!rare_plan.ok()) return Result<nlohmann::json>(rare_plan.status());
  if (common_plan.value().find("SeqScanPlan") == std::string::npos ||
      rare_plan.value().find("IndexScanPlan") == std::string::npos) {
    return Result<nlohmann::json>(Status::InternalError(
        "Cost optimizer did not choose the expected access paths"));
  }
  auto common = MeasureCold(files.indexed, common_query, repetitions);
  if (!common.ok()) return Result<nlohmann::json>(common.status());
  auto rare = MeasureCold(files.indexed, rare_query, repetitions);
  if (!rare.ok()) return Result<nlohmann::json>(rare.status());

  const std::size_t rare_matches = row_count / 10;
  const std::size_t common_matches = row_count - rare_matches;
  const double tree_cost = std::ceil(std::log2(
      static_cast<double>(std::max<std::size_t>(2, row_count))));
  auto scenarios = nlohmann::json::array();
  auto common_json = ScenarioJson(
      "common", "高频值 · 命中 90%", "cold", "SeqScan",
      common_plan.value(), common.value());
  common_json["matching_rows"] = common_matches;
  common_json["selectivity"] = 0.9;
  common_json["seq_cost"] = static_cast<double>(row_count);
  common_json["index_cost"] = tree_cost + 4.0 * common_matches;
  scenarios.push_back(std::move(common_json));
  auto rare_json = ScenarioJson(
      "rare", "低频值 · 命中 10%", "cold", "IndexScan",
      rare_plan.value(), rare.value());
  rare_json["matching_rows"] = rare_matches;
  rare_json["selectivity"] = 0.1;
  rare_json["seq_cost"] = static_cast<double>(row_count);
  rare_json["index_cost"] = tree_cost + 4.0 * rare_matches;
  scenarios.push_back(std::move(rare_json));

  return Result<nlohmann::json>(nlohmann::json{
      {"metadata", {{"row_count", row_count},
                     {"repetitions", repetitions},
                     {"index", "idx_student_gender"},
                     {"cost_formula", "IndexCost = ceil(log2(N)) + 4 × matches"}}},
      {"scenarios", std::move(scenarios)}});
}

Result<nlohmann::json> PerformanceBenchmarkService::RunBufferComparison(
    const std::string &access_pattern) const {
  if (access_pattern != "sequential" && access_pattern != "random" &&
      access_pattern != "hotspot") {
    return Result<nlohmann::json>(Status::InvalidArgument(
        "access_pattern must be sequential, random, or hotspot"));
  }
  TempBufferDatabase database;
  auto fixture = BuildBufferFixture(database.path);
  if (!fixture.ok()) return Result<nlohmann::json>(fixture);
  const auto requests = BufferRequests(access_pattern);
  auto fifo = MeasureBufferPolicy(database.path, requests,
                                  ReplacementPolicy::FIFO, "FIFO");
  if (!fifo.ok()) return Result<nlohmann::json>(fifo.status());
  auto lru = MeasureBufferPolicy(database.path, requests,
                                 ReplacementPolicy::LRU, "LRU");
  if (!lru.ok()) return Result<nlohmann::json>(lru.status());
  auto clock = MeasureBufferPolicy(database.path, requests,
                                   ReplacementPolicy::CLOCK, "CLOCK");
  if (!clock.ok()) return Result<nlohmann::json>(clock.status());
  return Result<nlohmann::json>(nlohmann::json{
      {"metadata", {{"access_pattern", access_pattern},
                     {"page_count", 32}, {"request_count", requests.size()},
                     {"pool_size", 4}, {"seed", 20260908}}},
      {"policies", nlohmann::json::array(
          {std::move(fifo.value()), std::move(lru.value()),
           std::move(clock.value())})}});
}

}  // namespace oursql
