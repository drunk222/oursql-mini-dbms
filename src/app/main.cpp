#include "oursql/execution/database_engine.h"

#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string LowerAscii(std::string text) {
  for (char &character : text) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return text;
}

bool HasStatementTerminator(std::string_view text) {
  bool in_string = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\'') {
      if (in_string && i + 1 < text.size() && text[i + 1] == '\'') {
        ++i;
        continue;
      }
      in_string = !in_string;
    } else if (!in_string && text[i] == ';') {
      return true;
    }
  }
  return false;
}

void PrintResult(const oursql::ExecutionResult &result) {
  if (!result.column_names.empty()) {
    for (std::size_t i = 0; i < result.column_names.size(); ++i) {
      if (i != 0) std::cout << " | ";
      std::cout << result.column_names[i];
    }
    std::cout << '\n';
    for (const auto &row : result.rows) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) std::cout << " | ";
        std::cout << row[i].ToString();
      }
      std::cout << '\n';
    }
    std::cout << "(" << result.rows.size() << " row(s))\n";
  }
  if (result.column_names.empty() && !result.rids.empty()) {
    for (const auto &rid : result.rids) {
      std::cout << "RID: (page=" << rid.page_id << ", slot=" << rid.slot_id << ")\n";
    }
  }
  if (result.affected_rows != 0) {
    std::cout << "Affected rows: " << result.affected_rows << '\n';
  } else if (result.column_names.empty() && result.rows.empty() && result.rids.empty()) {
    std::cout << "OK\n";
  }
}

void PrintStats(const oursql::DatabaseStatistics &stats) {
  std::cout << "buffer accesses: " << stats.buffer_accesses << '\n'
            << "buffer hits: " << stats.buffer_hits << '\n'
            << "buffer misses: " << stats.buffer_misses << '\n'
            << "buffer hit rate: " << std::fixed << std::setprecision(4)
            << stats.buffer_hit_rate << '\n'
            << "evictions: " << stats.evictions << '\n'
            << "disk reads: " << stats.disk_reads << '\n'
            << "disk writes: " << stats.disk_writes << '\n';
}

bool HandleMetaCommand(std::string_view line, oursql::DatabaseEngine *engine, bool *should_quit) {
  std::istringstream input{std::string(line)};
  std::string command;
  std::string argument;
  input >> command >> argument;
  if (command == ".quit") {
    *should_quit = true;
    auto status = engine->Close();
    if (!status.ok()) std::cerr << status.ToString() << '\n';
    return status.ok();
  }
  if (command == ".tables") {
    for (const auto &table : engine->ListTables()) std::cout << table.name << '\n';
    return true;
  }
  if (command == ".schema") {
    if (argument.empty()) {
      std::cerr << "Usage: .schema table_name\n";
      return false;
    }
    auto metadata = engine->GetTableMetadata(LowerAscii(argument));
    if (!metadata.ok()) {
      std::cerr << metadata.status().ToString() << '\n';
      return false;
    }
    for (const auto &column : metadata.value().schema.columns()) {
      std::cout << column.name << " "
                << (column.type == oursql::DataType::Int ? "INT" : "VARCHAR");
      if (column.length.has_value()) std::cout << "(" << *column.length << ")";
      std::cout << '\n';
    }
    return true;
  }
  if (command == ".stats") {
    PrintStats(engine->GetStatistics());
    return true;
  }
  if (command == ".flush") {
    auto status = engine->Flush();
    if (!status.ok()) std::cerr << status.ToString() << '\n';
    else std::cout << "Flushed\n";
    return status.ok();
  }
  std::cerr << "Unknown command: " << command << '\n';
  return false;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  try {
#ifdef _WIN32
    const bool help = argc == 2 &&
                      (std::wstring_view(argv[1]) == L"--help" ||
                       std::wstring_view(argv[1]) == L"-h");
#else
    const bool help = argc == 2 &&
                      (std::string_view(argv[1]) == "--help" ||
                       std::string_view(argv[1]) == "-h");
#endif
    if (argc > 2 || help) {
      std::cout << "Usage: oursql [database.oursql]\n";
      return argc > 2 ? 1 : 0;
    }
    const std::filesystem::path database_path = argc == 2 ? std::filesystem::path(argv[1])
                                                          : "demo.oursql";
    oursql::DatabaseEngine engine(database_path);
    if (!engine.GetInitStatus().ok()) {
      std::cerr << engine.GetInitStatus().ToString() << '\n';
      return 1;
    }

    std::string sql_buffer;
    std::string line;
    bool should_quit = false;
    while (!should_quit) {
      std::cout << (sql_buffer.empty() ? "oursql> " : "     ...> ") << std::flush;
      if (!std::getline(std::cin, line)) break;
      if (sql_buffer.empty() && !line.empty() && line.front() == '.') {
        HandleMetaCommand(line, &engine, &should_quit);
        continue;
      }
      sql_buffer += line;
      sql_buffer.push_back('\n');
      if (!HasStatementTerminator(sql_buffer)) continue;
      auto result = engine.ExecuteSql(sql_buffer);
      if (!result.ok()) std::cerr << result.status().ToString() << '\n';
      else PrintResult(result.value());
      sql_buffer.clear();
    }
    if (!sql_buffer.empty() && !should_quit) {
      auto result = engine.ExecuteSql(sql_buffer);
      if (!result.ok()) std::cerr << result.status().ToString() << '\n';
      else PrintResult(result.value());
    }
    const auto status = engine.Close();
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "OurSQL error: " << error.what() << '\n';
    return 1;
  }
}
