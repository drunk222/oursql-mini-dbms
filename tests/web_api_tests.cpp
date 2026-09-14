#include "oursql/execution/database_engine.h"
#include "oursql/service/query_service.h"
#include "oursql/web/api_types.h"
#include "oursql/web/web_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_web_test_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDb() {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".wal", error);
  }
};

struct RunningServer {
  oursql::WebServer *server{nullptr};
  int port{-1};
  std::thread thread;

  ~RunningServer() {
    if (server != nullptr) server->Stop();
    if (thread.joinable()) thread.join();
  }
};

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

bool WaitForServer(httplib::Client &client) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (client.Get("/api/health")) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool ParseJson(const httplib::Result &response, nlohmann::json *payload) {
  if (!response || payload == nullptr) return false;
  *payload = nlohmann::json::parse(response->body, nullptr, false);
  return !payload->is_discarded();
}

bool TestWebApi() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 4);
  if (!Check(engine.GetInitStatus().ok(), "Web test database should open")) return false;

  oursql::QueryService query_service(&engine);
  oursql::WebServerOptions options;
  options.host = "127.0.0.1";
  options.port = 0;
  options.web_root = std::filesystem::u8path(OURSQL_DEFAULT_WEB_ROOT);
  oursql::WebServer server(options, &query_service);
  if (!Check(server.Initialize().ok(), "Web server should initialize")) return false;

  RunningServer running;
  running.server = &server;
  running.port = server.BindToAnyPort();
  if (!Check(running.port > 0, "Web server should bind an ephemeral port")) return false;
  running.thread = std::thread([&server]() { (void)server.ListenAfterBind(); });

  httplib::Client client("127.0.0.1", running.port);
  client.set_connection_timeout(2, 0);
  client.set_read_timeout(5, 0);
  if (!Check(WaitForServer(client), "Web server should become ready")) return false;

  bool ok = true;
  {
    auto response = client.Get("/");
    ok = Check(response && response->status == 200 &&
                   response->get_header_value("Content-Type").find("text/html") !=
                       std::string::npos,
               "GET / should serve HTML") &&
         ok;
    response = client.Get("/app.js");
    ok = Check(response && response->status == 200,
               "GET /app.js should serve JavaScript") &&
         ok;
    response = client.Get("/style.css");
    ok = Check(response && response->status == 200,
               "GET /style.css should serve CSS") &&
         ok;
    response = client.Get("/assets/lucide/lucide.min.js");
    ok = Check(response && response->status == 200 &&
                   response->get_header_value("Content-Type").find("javascript") !=
                       std::string::npos,
               "GET /assets/* should serve vendored assets") &&
         ok;
    response = client.Get("/concurrency.html");
    ok = Check(response && response->status == 200,
               "GET /concurrency.html should serve the concurrency page") &&
         ok;
  }

  const nlohmann::json query_body = {
      {"sql",
       "CREATE TABLE student(id INT, name VARCHAR);\n"
       "INSERT INTO student VALUES(1, 'Alice');\n"
       "\n"
       "INSERT INTO student VALUES(2, 'Bob');\n"
       "SELECT * FROM student;"}};
  {
    auto response = client.Post("/api/query", query_body.dump(), "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true,
               "POST /api/query should return a successful JSON response") &&
         ok;
    if (parsed) {
      const auto &results = payload["data"]["results"];
      ok = Check(results.is_array() && results.size() == 4,
                 "Query response should contain one result per SQL statement") &&
           ok;
      if (results.is_array() && results.size() == 4) {
        double previous_duration = 0.0;
        for (const auto &result : results) {
          const double duration = result.value("duration_ms", 0.0);
          ok = Check(duration >= previous_duration,
                     "Query result durations should be cumulative") &&
               ok;
          previous_duration = duration;
        }
        ok = Check(results[0]["line"] == 1 && results[1]["line"] == 2 &&
                       results[2]["line"] == 4 && results[3]["line"] == 5,
                   "Query response should map each result to its source line") &&
             ok;
        const auto &rows = results[3]["rows"];
        ok = Check(rows.size() == 2 && rows[0][0]["type"] == "int" &&
                       rows[0][0]["value"] == "1" &&
                       rows[0][1]["type"] == "string" &&
                       rows[0][1]["value"] == "Alice",
                   "Query response should preserve typed values") &&
             ok;
      }
    }
  }

  {
    const nlohmann::json trace_body = {
        {"sql",
         "CREATE TABLE trace_should_not_exist(id INT);\n"
         "SELECT * FROM student;"}};
    auto response = client.Post("/api/trace", trace_body.dump(), "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true,
               "POST /api/trace should return a successful trace response") &&
         ok;
    if (parsed) {
      const auto &steps = payload["data"]["steps"];
      ok = Check(steps.is_array() && steps.size() == 4 &&
                     steps[0]["name"] == "Token" && steps[1]["name"] == "AST" &&
                     steps[2]["name"] == "语义检查" && steps[3]["name"] == "Plan",
                 "Trace response should preserve compilation stage order") &&
           ok;
      ok = Check(steps[0]["entries"].size() > 0 && steps[1]["entries"].size() == 2 &&
                     steps[2]["entries"].size() == 2 &&
                     steps[3]["entries"].size() == 2,
                 "Trace response should print every compilation stage") &&
           ok;
      ok = Check(steps[2]["ok"] == true && steps[3]["ok"] == true,
                 "Valid trace SQL should pass semantic checks and planning") &&
           ok;
    }
  }

  {
    const nlohmann::json concurrent_body = {
        {"sql",
         {"SELECT * FROM student WHERE id = 1;",
          "SELECT * FROM student WHERE id = 2;"}}};
    auto response =
        client.Post("/api/concurrency/run", concurrent_body.dump(), "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true,
               "POST /api/concurrency/run should execute independent sessions") &&
         ok;
    if (parsed) {
      const auto &results = payload["data"]["results"];
      ok = Check(results.is_array() && results.size() == 2 &&
                     results[0]["ok"] == true && results[1]["ok"] == true,
                 "Concurrent SQL should return one successful result per session") &&
           ok;
      if (results.is_array() && results.size() == 2) {
        ok = Check(results[0]["results"][0]["rows"].size() == 1 &&
                       results[0]["results"][0]["rows"][0][0]["value"] == "1" &&
                       results[1]["results"][0]["rows"].size() == 1 &&
                       results[1]["results"][0]["rows"][0][0]["value"] == "2",
                   "Concurrent SQL should preserve per-session query results") &&
             ok;
        ok = Check(results[0]["results"][0].contains("duration_ms") &&
                       results[1]["results"][0].contains("duration_ms"),
                   "Each concurrent SQL result should include its own duration") &&
             ok;
      }
    }
  }

  {
    const nlohmann::json advanced_body = {
        {"sql",
         "CREATE TABLE api_users(id INT, name VARCHAR(16));\n"
         "CREATE TABLE api_scores(user_id INT, score INT);\n"
         "INSERT INTO api_users(id, name) VALUES(2, 'Bob'), (1, 'Alice');\n"
         "INSERT INTO api_scores VALUES(1, 80), (3, 95);\n"
         "SELECT u.id, s.score FROM api_users AS u LEFT JOIN api_scores AS s "
         "ON u.id = s.user_id;\n"
         "SELECT id FROM api_users WHERE EXISTS "
         "(SELECT user_id FROM api_scores WHERE score = 80);\n"
         "ALTER TABLE api_users RENAME COLUMN name TO display_name;\n"
         "SELECT id, display_name FROM api_users;\n"
         "DROP TABLE api_scores;\n"
         "DROP TABLE api_users;"}};
    auto response = client.Post("/api/query", advanced_body.dump(), "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true,
               "Advanced SQL should execute through the Web API") &&
         ok;
    if (parsed) {
      const auto &results = payload["data"]["results"];
      ok = Check(results.is_array() && results.size() == 10,
                 "Advanced Web SQL should return one result per statement") &&
           ok;
      if (results.is_array() && results.size() == 10) {
        ok = Check(results[2]["affected_rows"] == 2 &&
                       results[3]["affected_rows"] == 2,
                   "Specified-column and multi-row INSERT should affect all rows") &&
             ok;
        const auto &joined = results[4]["rows"];
        bool saw_match = false;
        bool saw_unmatched = false;
        for (const auto &row : joined) {
          saw_match = saw_match ||
                      (row[0]["value"] == "1" && row[1]["value"] == "80");
          saw_unmatched = saw_unmatched ||
                          (row[0]["value"] == "2" && row[1]["type"] == "null");
        }
        ok = Check(joined.size() == 2 && saw_match && saw_unmatched,
                   "LEFT JOIN should preserve matched and unmatched left rows") &&
             ok;
        ok = Check(results[5]["rows"].size() == 2,
                   "EXISTS subquery should execute through the Web API") &&
             ok;
        const auto &altered = results[7]["rows"];
        bool saw_alice = false;
        bool saw_bob = false;
        for (const auto &row : altered) {
          saw_alice = saw_alice || row[1]["value"] == "Alice";
          saw_bob = saw_bob || row[1]["value"] == "Bob";
        }
        ok = Check(altered.size() == 2 && saw_alice && saw_bob,
                   "ALTER RENAME COLUMN should preserve existing data") && ok;
      }
    }
  }

  {
    auto response = client.Get("/api/tables");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true &&
                   payload["data"]["count"] == 1 &&
                   payload["data"]["tables"][0]["name"] == "student",
               "GET /api/tables should return table metadata") &&
         ok;
  }

  {
    auto response = client.Get("/api/schema?table=STUDENT");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true &&
                   payload["data"]["columns"].size() == 2 &&
                   payload["data"]["columns"][0]["name"] == "id",
               "GET /api/schema should normalize the table name") &&
         ok;
  }

  {
    auto response = client.Get("/api/stats");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true &&
                   payload["data"]["statistics"].contains("buffer_hits") &&
                   payload["data"]["buffer_snapshots"].is_array(),
               "GET /api/stats should return storage statistics") &&
         ok;
  }

  {
    const nlohmann::json transaction_body = {{"sql", "BEGIN;"}};
    auto response =
        client.Post("/api/query", transaction_body.dump(), "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 422 && payload["ok"] == false &&
                   payload["error"]["code"] == "NotImplemented",
               "Cross-request transactions should be rejected") &&
         ok;
  }

  {
    const nlohmann::json transaction_body = {
        {"sql", "BEGIN; INSERT INTO student VALUES(3, 'Cara'); ROLLBACK;"}};
    auto response =
        client.Post("/api/query", transaction_body.dump(), "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true &&
                   payload["data"]["results"].size() == 3,
               "A complete transaction batch should be allowed") &&
         ok;
  }

  {
    auto response = client.Post("/api/query", "{", "application/json");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 400 && payload["ok"] == false,
               "Malformed JSON should return 400") &&
         ok;
  }

  {
    auto response = client.Post("/api/flush");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true,
               "POST /api/flush should report success") &&
         ok;
  }

  {
    auto response = client.Get("/api/unknown");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 404 && payload["ok"] == false,
               "Unknown API routes should return JSON 404") &&
         ok;
  }

  {
    auto response = client.Post("/api/concurrency/demo");
    nlohmann::json payload;
    const bool parsed = ParseJson(response, &payload);
    ok = Check(parsed && response->status == 200 && payload["ok"] == true &&
                   payload["data"]["passed"] == true &&
                   payload["data"]["scenarios"].size() == 4,
               "Concurrency demo should execute all scenarios") &&
         ok;
  }

  running.server->Stop();
  if (running.thread.joinable()) running.thread.join();
  if (!Check(engine.Close().ok(), "Web test database should close")) ok = false;
  return ok;
}

}  // namespace

int main() {
  const bool ok = TestWebApi();
  if (ok) std::cout << "[PASS] OurSQL Web API\n";
  return ok ? 0 : 1;
}
