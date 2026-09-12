#include "oursql/execution/database_engine.h"

#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
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

struct CliOptions {
  std::filesystem::path database_path{"demo.oursql"};
  std::size_t pool_size{16};
  oursql::ReplacementPolicy policy{oursql::ReplacementPolicy::LRU};
  oursql::FlushPolicy flush_policy{oursql::FlushPolicy::WriteBack};
  bool help{false};
};

template <typename CharT>
bool IsAsciiToken(std::basic_string_view<CharT> value, std::string_view expected) {
  if (value.size() != expected.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto character = static_cast<unsigned int>(value[i]);
    const auto expected_character = static_cast<unsigned int>(expected[i]);
    if (character == expected_character) continue;
    if (character >= 'A' && character <= 'Z' && character + ('a' - 'A') == expected_character) {
      continue;
    }
    return false;
  }
  return true;
}

template <typename CharT>
bool ParsePoolSize(std::basic_string_view<CharT> text, std::size_t *value) {
  if (text.empty()) return false;
  std::size_t parsed = 0;
  for (const auto character : text) {
    const auto digit = static_cast<unsigned int>(character);
    if (digit < '0' || digit > '9') return false;
    const auto number = static_cast<std::size_t>(digit - '0');
    if (parsed > (std::numeric_limits<std::size_t>::max() - number) / 10) return false;
    parsed = parsed * 10 + number;
  }
  if (parsed == 0) return false;
  *value = parsed;
  return true;
}

template <typename CharT>
bool ParseReplacementPolicy(std::basic_string_view<CharT> text,
                            oursql::ReplacementPolicy *policy) {
  if (IsAsciiToken(text, "fifo")) {
    *policy = oursql::ReplacementPolicy::FIFO;
    return true;
  }
  if (IsAsciiToken(text, "lru")) {
    *policy = oursql::ReplacementPolicy::LRU;
    return true;
  }
  if (IsAsciiToken(text, "clock")) {
    *policy = oursql::ReplacementPolicy::CLOCK;
    return true;
  }
  return false;
}

template <typename CharT>
bool ParseFlushPolicy(std::basic_string_view<CharT> text, oursql::FlushPolicy *policy) {
  if (IsAsciiToken(text, "write-back") || IsAsciiToken(text, "writeback")) {
    *policy = oursql::FlushPolicy::WriteBack;
    return true;
  }
  if (IsAsciiToken(text, "write-through") || IsAsciiToken(text, "writethrough")) {
    *policy = oursql::FlushPolicy::WriteThrough;
    return true;
  }
  return false;
}

template <typename CharT>
bool ParseOptions(int argc, CharT **argv, CliOptions *options, std::string *error) {
  bool database_seen = false;
  for (int index = 1; index < argc; ++index) {
    const std::basic_string_view<CharT> argument(argv[index]);
    if (IsAsciiToken(argument, "--help") || IsAsciiToken(argument, "-h")) {
      options->help = true;
      continue;
    }
    if (IsAsciiToken(argument, "--pool-size")) {
      if (++index >= argc ||
          !ParsePoolSize(std::basic_string_view<CharT>(argv[index]), &options->pool_size)) {
        *error = "--pool-size requires a positive integer";
        return false;
      }
      continue;
    }
    if (IsAsciiToken(argument, "--policy")) {
      if (++index >= argc ||
          !ParseReplacementPolicy(std::basic_string_view<CharT>(argv[index]), &options->policy)) {
        *error = "--policy requires fifo, lru, or clock";
        return false;
      }
      continue;
    }
    if (IsAsciiToken(argument, "--flush-policy")) {
      if (++index >= argc ||
          !ParseFlushPolicy(std::basic_string_view<CharT>(argv[index]), &options->flush_policy)) {
        *error = "--flush-policy requires write-back or write-through";
        return false;
      }
      continue;
    }
    if (!database_seen && argument.empty() == false && argument.front() != CharT('-')) {
      options->database_path = std::filesystem::path(argv[index]);
      database_seen = true;
      continue;
    }
    *error = "unknown or misplaced command-line argument";
    return false;
  }
  return true;
}

const char *PolicyName(oursql::ReplacementPolicy policy) {
  switch (policy) {
    case oursql::ReplacementPolicy::FIFO: return "FIFO";
    case oursql::ReplacementPolicy::LRU: return "LRU";
    case oursql::ReplacementPolicy::CLOCK: return "CLOCK";
  }
  return "UNKNOWN";
}

const char *FlushPolicyName(oursql::FlushPolicy policy) {
  return policy == oursql::FlushPolicy::WriteBack ? "WriteBack" : "WriteThrough";
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

void PrintBuffer(const oursql::DatabaseEngine &engine) {
  std::cout << "Policy: " << PolicyName(engine.GetReplacementPolicy()) << '\n'
            << "Flush policy: " << FlushPolicyName(engine.GetFlushPolicy()) << '\n'
            << "Pool size: " << engine.GetPoolSize() << '\n';
  for (const auto &snapshot : engine.GetBufferSnapshots()) {
    std::cout << "Frame " << snapshot.frame_id << ": ";
    if (snapshot.page_id.has_value()) {
      std::cout << "page=" << *snapshot.page_id;
    } else {
      std::cout << "empty";
    }
    std::cout << " pin=" << snapshot.pin_count << " dirty="
              << (snapshot.is_dirty ? "true" : "false") << " evictable="
              << (snapshot.is_evictable ? "true" : "false") << '\n';
  }
}

void PrintEvictions(const oursql::DatabaseEngine &engine) {
  const auto log = engine.GetEvictionLog();
  std::cout << "Policy: " << PolicyName(engine.GetReplacementPolicy()) << '\n'
            << "Evicted pages: ";
  if (log.empty()) {
    std::cout << "none\n";
    return;
  }
  for (std::size_t i = 0; i < log.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << log[i];
  }
  std::cout << '\n';
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
    if (argument == "reset") {
      auto status = engine->ResetStatistics();
      if (!status.ok()) {
        PrintCliError(status);
        return false;
      }
      std::cout << "Statistics reset\n";
      return true;
    }
    if (!argument.empty()) {
      std::cout << "Usage: .stats [reset]\n" << std::flush;
      return false;
    }
    PrintStats(engine->GetStatistics());
    return true;
  }
  if (command == ".buffer") {
    if (!argument.empty()) {
      std::cout << "Usage: .buffer\n" << std::flush;
      return false;
    }
    PrintBuffer(*engine);
    return true;
  }
  if (command == ".evictions") {
    if (!argument.empty()) {
      std::cout << "Usage: .evictions\n" << std::flush;
      return false;
    }
    PrintEvictions(*engine);
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
    CliOptions options;
    std::string parse_error;
    if (!ParseOptions(argc, argv, &options, &parse_error) || options.help) {
      if (!parse_error.empty()) std::cout << "Error: " << parse_error << '\n';
      std::cout << "Usage: oursql [database.oursql] [--pool-size N]"
                   " [--policy fifo|lru|clock]"
                   " [--flush-policy write-back|write-through]\n";
      return parse_error.empty() ? 0 : 1;
    }
    oursql::DatabaseEngine engine(options.database_path, options.pool_size, options.policy,
                                  options.flush_policy);
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
