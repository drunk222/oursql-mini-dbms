#pragma once

#include "oursql/service/query_service.h"
#include "oursql/web/api_types.h"
#include "oursql/web/concurrency_demo.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <string>

namespace oursql {

class WebServer {
 public:
  WebServer(WebServerOptions options, QueryService *query_service);
  ~WebServer();

  WebServer(const WebServer &) = delete;
  WebServer &operator=(const WebServer &) = delete;

  [[nodiscard]] Status Initialize();
  [[nodiscard]] int BindToAnyPort();
  [[nodiscard]] bool ListenAfterBind();
  [[nodiscard]] bool Listen();
  void Stop();

  [[nodiscard]] const std::string &LastError() const noexcept;

 private:
  void RegisterRoutes();
  void HandleQuery(const httplib::Request &request, httplib::Response &response);
  void HandleTrace(const httplib::Request &request, httplib::Response &response);
  void HandleTables(const httplib::Request &request, httplib::Response &response);
  void HandleSchema(const httplib::Request &request, httplib::Response &response);
  void HandleStats(const httplib::Request &request, httplib::Response &response);
  void HandleFlush(const httplib::Request &request, httplib::Response &response);
  void HandleConcurrencyRun(const httplib::Request &request,
                            httplib::Response &response);
  void HandleConcurrencyDemo(const httplib::Request &request,
                             httplib::Response &response);

  [[nodiscard]] bool ServeStaticFile(const std::filesystem::path &relative_path,
                                     const std::string &content_type,
                                     httplib::Response &response) const;
  static void SendJson(httplib::Response &response, const nlohmann::json &payload,
                       int status = 200);
  static void SendStatus(httplib::Response &response, const Status &status);

  WebServerOptions options_;
  QueryService *query_service_{nullptr};
  ConcurrencyDemoService concurrency_demo_;
  httplib::Server server_;
  std::atomic<bool> initialized_{false};
  mutable std::string last_error_;
};

}  // namespace oursql
