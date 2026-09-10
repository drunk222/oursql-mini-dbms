#include "oursql/execution/database_engine.h"

#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

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

void PrintCliError(const oursql::Status &status) {
  // CLI 的提示符走 stdout，错误也在输出前刷新，避免两个流在终端中乱序。
  std::cout.flush();
  std::cout << status.ToString() << '\n' << std::flush;
}

#ifdef _WIN32
void ConfigureConsoleEncoding() {
  // 源码和错误信息都是 UTF-8；切换代码页后，Windows 终端才能正确显示中文。
  (void)::SetConsoleOutputCP(CP_UTF8);
  (void)::SetConsoleCP(CP_UTF8);
}
#endif

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
    if (!status.ok()) PrintCliError(status);
    return status.ok();
  }
  if (command == ".tables") {
    for (const auto &table : engine->ListTables()) std::cout << table.name << '\n';
    return true;
  }
  if (command == ".schema") {
    if (argument.empty()) {
      std::cout << "Usage: .schema table_name\n" << std::flush;
      return false;
    }
    auto metadata = engine->GetTableMetadata(LowerAscii(argument));
    if (!metadata.ok()) {
      PrintCliError(metadata.status());
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
    if (!status.ok()) PrintCliError(status);
    else std::cout << "Flushed\n";
    return status.ok();
  }
  std::cout << "Unknown command: " << command << '\n' << std::flush;
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
    ConfigureConsoleEncoding();
#endif
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
      PrintCliError(engine.GetInitStatus());
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
      auto results = engine.ExecuteSqlBatch(sql_buffer);
      if (!results.ok()) {
        PrintCliError(results.status());
      } else {
        for (const auto &result : results.value()) PrintResult(result);
      }
      sql_buffer.clear();
    }
    if (!sql_buffer.empty() && !should_quit) {
      auto results = engine.ExecuteSqlBatch(sql_buffer);
      if (!results.ok()) {
        PrintCliError(results.status());
      } else {
        for (const auto &result : results.value()) PrintResult(result);
      }
    }
    const auto status = engine.Close();
    if (!status.ok()) {
      PrintCliError(status);
      return 1;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cout << "OurSQL error: " << error.what() << '\n' << std::flush;
    return 1;
  }
}
