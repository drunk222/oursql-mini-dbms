#include "oursql/web/web_server.h"

#include "oursql/web/json_codec.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>

namespace oursql {
namespace {

std::string LowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text;
}

bool FileExists(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

std::string ContentTypeFor(const std::filesystem::path &path) {
  const std::string extension = path.extension().string();
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js") return "application/javascript; charset=utf-8";
  if (extension == ".sql") return "text/plain; charset=utf-8";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".woff2") return "font/woff2";
  return "application/octet-stream";
}

}  // namespace

WebServer::WebServer(WebServerOptions options, QueryService *query_service)
    : options_(std::move(options)), query_service_(query_service) {}

WebServer::~WebServer() {
  Stop();
}

Status WebServer::Initialize() {
  if (query_service_ == nullptr) {
    return Status::InvalidArgument("QueryService is not available");
  }
  if (options_.host.empty()) {
    return Status::InvalidArgument("HTTP host must not be empty");
  }
  if (options_.port < 0 || options_.port > 65535) {
    return Status::InvalidArgument("HTTP port is invalid");
  }

  const auto index_path = options_.web_root / "index.html";
  const auto app_path = options_.web_root / "app.js";
  const auto style_path = options_.web_root / "style.css";
  const auto concurrency_html_path = options_.web_root / "concurrency.html";
  const auto concurrency_js_path = options_.web_root / "concurrency.js";
  const auto lucide_path = options_.web_root / "assets" / "lucide" / "lucide.min.js";
  if (!FileExists(index_path) || !FileExists(app_path) || !FileExists(style_path) ||
      !FileExists(concurrency_html_path) || !FileExists(concurrency_js_path) ||
      !FileExists(lucide_path)) {
    return Status::NotFound("Web assets are incomplete under " + options_.web_root.string());
  }

  server_.set_payload_max_length(kMaxWebRequestBodyBytes);
  server_.set_read_timeout(10, 0);
  server_.set_write_timeout(10, 0);
  RegisterRoutes();
  initialized_.store(true);
  return Status::Ok();
}

int WebServer::BindToAnyPort() {
  if (!initialized_.load()) {
    last_error_ = "WebServer is not initialized";
    return -1;
  }
  const int port = server_.bind_to_any_port(options_.host);
  if (port <= 0) {
    last_error_ = "Unable to bind the HTTP server to any port";
  }
  return port;
}

bool WebServer::ListenAfterBind() {
  if (!initialized_.load()) {
    last_error_ = "WebServer is not initialized";
    return false;
  }
  const bool ok = server_.listen_after_bind();
  if (!ok && initialized_.load()) {
    last_error_ = "HTTP server stopped unexpectedly";
  }
  return ok;
}

bool WebServer::Listen() {
  if (!initialized_.load()) {
    last_error_ = "WebServer is not initialized";
    return false;
  }
  const bool ok = server_.listen(options_.host, options_.port);
  if (!ok && initialized_.load()) {
    last_error_ = "Unable to listen on " + options_.host + ":" +
                  std::to_string(options_.port);
  }
  return ok;
}

void WebServer::Stop() {
  if (initialized_.exchange(false)) {
    server_.stop();
  }
}

const std::string &WebServer::LastError() const noexcept {
  return last_error_;
}

void WebServer::RegisterRoutes() {
  server_.Get("/", [this](const httplib::Request &, httplib::Response &response) {
    (void)ServeStaticFile("index.html", "text/html; charset=utf-8", response);
  });
  server_.Get("/app.js", [this](const httplib::Request &, httplib::Response &response) {
    (void)ServeStaticFile("app.js", "application/javascript; charset=utf-8", response);
  });
  server_.Get("/style.css", [this](const httplib::Request &, httplib::Response &response) {
    (void)ServeStaticFile("style.css", "text/css; charset=utf-8", response);
  });
  server_.Get("/concurrency.html",
              [this](const httplib::Request &, httplib::Response &response) {
                (void)ServeStaticFile("concurrency.html", "text/html; charset=utf-8",
                                      response);
              });
  server_.Get("/concurrency.js",
              [this](const httplib::Request &, httplib::Response &response) {
                (void)ServeStaticFile("concurrency.js",
                                      "application/javascript; charset=utf-8",
                                      response);
              });
  server_.Get("/assets/.*",
              [this](const httplib::Request &request, httplib::Response &response) {
                const std::string relative = request.path.substr(1);
                if (relative.find("..") != std::string::npos ||
                    relative.find('\\') != std::string::npos) {
                  SendStatus(response, Status::InvalidArgument("Invalid asset path"));
                  return;
                }
                (void)ServeStaticFile(relative, ContentTypeFor(relative), response);
              });

  server_.Get("/api/health", [this](const httplib::Request &, httplib::Response &response) {
    SendJson(response,
             SuccessPayload({{"status", "ok"},
                             {"database", options_.database_path.filename().string()}}));
  });
  server_.Post("/api/query",
               [this](const httplib::Request &request, httplib::Response &response) {
                 HandleQuery(request, response);
               });
  server_.Post("/api/trace",
               [this](const httplib::Request &request, httplib::Response &response) {
                 HandleTrace(request, response);
               });
  server_.Get("/api/tables",
              [this](const httplib::Request &request, httplib::Response &response) {
                HandleTables(request, response);
              });
  server_.Get("/api/schema",
              [this](const httplib::Request &request, httplib::Response &response) {
                HandleSchema(request, response);
              });
  server_.Get("/api/stats",
              [this](const httplib::Request &request, httplib::Response &response) {
                HandleStats(request, response);
              });
  server_.Post("/api/flush",
               [this](const httplib::Request &request, httplib::Response &response) {
                 HandleFlush(request, response);
               });
  server_.Post("/api/concurrency/demo",
               [this](const httplib::Request &request, httplib::Response &response) {
                 HandleConcurrencyDemo(request, response);
               });
  server_.Post("/api/concurrency/run",
               [this](const httplib::Request &request, httplib::Response &response) {
                 HandleConcurrencyRun(request, response);
               });

  server_.set_error_handler([](const httplib::Request &request, httplib::Response &response) {
    if (request.path.rfind("/api/", 0) == 0 && response.body.empty()) {
      SendJson(response,
               ErrorPayload(Status::NotFound("Unknown API route: " + request.path)),
               404);
    }
  });
}

void WebServer::HandleQuery(const httplib::Request &request, httplib::Response &response) {
  auto body = nlohmann::json::parse(request.body, nullptr, false);
  if (body.is_discarded() || !body.is_object() || !body.contains("sql") ||
      !body["sql"].is_string()) {
    SendStatus(response, Status::InvalidArgument("Request body must contain string field 'sql'"));
    return;
  }
  auto result = query_service_->ExecuteSql(body["sql"].get<std::string>());
  if (!result.ok()) {
    SendStatus(response, result.status());
    return;
  }

  auto results = nlohmann::json::array();
  for (const auto &item : result.value()) results.push_back(ExecutionResultToJson(item));
  SendJson(response, SuccessPayload({{"results", std::move(results)}}));
}

void WebServer::HandleTrace(const httplib::Request &request, httplib::Response &response) {
  auto body = nlohmann::json::parse(request.body, nullptr, false);
  if (body.is_discarded() || !body.is_object() || !body.contains("sql") ||
      !body["sql"].is_string()) {
    SendStatus(response, Status::InvalidArgument("Request body must contain string field 'sql'"));
    return;
  }
  auto result = query_service_->TraceSql(body["sql"].get<std::string>());
  if (!result.ok()) {
    SendStatus(response, result.status());
    return;
  }
  SendJson(response, SuccessPayload(SqlTraceToJson(result.value())));
}

void WebServer::HandleTables(const httplib::Request &, httplib::Response &response) {
  auto tables = query_service_->ListTables();
  if (!tables.ok()) {
    SendStatus(response, tables.status());
    return;
  }
  auto payload = nlohmann::json::array();
  for (const auto &table : tables.value()) {
    payload.push_back(TableMetadataToJson(table));
  }
  SendJson(response,
           SuccessPayload({{"tables", std::move(payload)}, {"count", tables.value().size()}}));
}

void WebServer::HandleSchema(const httplib::Request &request, httplib::Response &response) {
  const std::string table_name = request.get_param_value("table");
  if (table_name.empty()) {
    SendStatus(response, Status::InvalidArgument("Query parameter 'table' is required"));
    return;
  }
  auto table = query_service_->GetTableMetadata(LowerAscii(table_name));
  if (!table.ok()) {
    SendStatus(response, table.status());
    return;
  }
  SendJson(response, SuccessPayload(TableMetadataToJson(table.value())));
}

void WebServer::HandleStats(const httplib::Request &, httplib::Response &response) {
  auto statistics = query_service_->GetStatistics();
  if (!statistics.ok()) {
    SendStatus(response, statistics.status());
    return;
  }
  auto snapshots = query_service_->GetBufferSnapshots();
  if (!snapshots.ok()) {
    SendStatus(response, snapshots.status());
    return;
  }
  auto evictions = query_service_->GetEvictionLog();
  if (!evictions.ok()) {
    SendStatus(response, evictions.status());
    return;
  }

  auto snapshot_json = nlohmann::json::array();
  for (const auto &snapshot : snapshots.value()) {
    snapshot_json.push_back(FrameSnapshotToJson(snapshot));
  }
  SendJson(response,
           SuccessPayload({{"statistics", StatisticsToJson(statistics.value())},
                           {"buffer_snapshots", std::move(snapshot_json)},
                           {"eviction_log", evictions.value()}}));
}

void WebServer::HandleFlush(const httplib::Request &, httplib::Response &response) {
  auto status = query_service_->Flush();
  if (!status.ok()) {
    SendStatus(response, status);
    return;
  }
  SendJson(response, SuccessPayload({{"message", "Flushed"}}));
}

void WebServer::HandleConcurrencyRun(const httplib::Request &request,
                                     httplib::Response &response) {
  auto body = nlohmann::json::parse(request.body, nullptr, false);
  if (body.is_discarded() || !body.is_object() || !body.contains("sql") ||
      !body["sql"].is_array()) {
    SendStatus(response,
               Status::InvalidArgument("Request body must contain array field 'sql'"));
    return;
  }
  std::vector<std::string> statements;
  statements.reserve(body["sql"].size());
  for (const auto &item : body["sql"]) {
    if (!item.is_string()) {
      SendStatus(response, Status::InvalidArgument("Every concurrent SQL item must be a string"));
      return;
    }
    statements.push_back(item.get<std::string>());
  }

  auto result = query_service_->ExecuteConcurrentSql(statements);
  if (!result.ok()) {
    SendStatus(response, result.status());
    return;
  }
  auto payload = nlohmann::json::array();
  for (const auto &item : result.value()) {
    payload.push_back(ConcurrentSqlResultToJson(item));
  }
  SendJson(response, SuccessPayload({{"results", std::move(payload)}}));
}

void WebServer::HandleConcurrencyDemo(const httplib::Request &,
                                      httplib::Response &response) {
  auto result = concurrency_demo_.Run();
  if (!result.ok()) {
    SendStatus(response, result.status());
    return;
  }
  SendJson(response, SuccessPayload(result.value()));
}

bool WebServer::ServeStaticFile(const std::filesystem::path &relative_path,
                                const std::string &content_type,
                                httplib::Response &response) const {
  std::ifstream input(options_.web_root / relative_path, std::ios::binary);
  if (!input) {
    SendStatus(response, Status::NotFound("Static asset was not found"));
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  response.set_header("Cache-Control", "no-cache");
  response.set_content(std::move(content), content_type);
  return true;
}

void WebServer::SendJson(httplib::Response &response, const nlohmann::json &payload,
                         int status) {
  response.status = status;
  response.set_header("Cache-Control", "no-store");
  response.set_content(payload.dump(), "application/json; charset=utf-8");
}

void WebServer::SendStatus(httplib::Response &response, const Status &status) {
  SendJson(response, ErrorPayload(status), HttpStatusFor(status.code()));
}

}  // namespace oursql
