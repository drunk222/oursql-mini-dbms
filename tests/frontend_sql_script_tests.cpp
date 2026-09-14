#include "oursql/execution/database_engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef OURSQL_FRONTEND_SQL_PATH
#error "OURSQL_FRONTEND_SQL_PATH must point to frontend_all_runnable.sql"
#endif

namespace {

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_frontend_sql_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDb() {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".wal", error);
  }
};

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

}  // namespace

int main() {
  std::ifstream input(std::filesystem::u8path(OURSQL_FRONTEND_SQL_PATH),
                      std::ios::binary);
  if (!Check(input.good(), "frontend_all_runnable.sql should be readable")) {
    return 1;
  }
  const std::string sql((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
  if (!Check(!sql.empty(), "frontend_all_runnable.sql should not be empty")) {
    return 1;
  }

  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 16);
  if (!Check(engine.GetInitStatus().ok(), "frontend SQL database should open")) {
    return 1;
  }

  auto results = engine.ExecuteSqlBatch(sql);
  if (!Check(results.ok(), "all frontend SQL statements should execute: " +
                               results.status().ToString())) {
    return 1;
  }
  if (!Check(!results.value().empty(), "frontend SQL should produce results")) {
    return 1;
  }
  if (!Check(engine.ListTables().empty(),
             "frontend SQL should clean up all test tables")) {
    return 1;
  }
  if (!Check(engine.Close().ok(), "frontend SQL database should close")) {
    return 1;
  }

  std::cout << "[PASS] " << results.value().size()
            << " frontend SQL results\n";
  return 0;
}
