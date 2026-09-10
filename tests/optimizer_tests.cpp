#include "oursql/catalog/catalog.h"
#include "oursql/common/schema.h"
#include "oursql/optimizer/optimizer.h"
#include "oursql/parser/parser.h"
#include "oursql/plan/plan.h"
#include "oursql/planner/planner.h"

#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

oursql::Catalog MakeCatalog() {
  oursql::Catalog catalog;
  const auto status = catalog.CreateTable(oursql::TableMetadata{
      "users",
      oursql::Schema{{{"id", oursql::DataType::Int},
                      {"name", oursql::DataType::Varchar, 8}}},
      oursql::INVALID_PAGE_ID});
  if (!status.ok()) {
    std::cerr << "catalog create failed: " << status.message() << '\n';
  }
  return catalog;
}

// 冻结形态：SELECT 的根算子必须是 Project(Filter(SeqScan))（无 WHERE 时省略 Filter）。
bool IsFrozenShape(const std::shared_ptr<const oursql::PlanNode> &root) {
  if (root == nullptr) return false;
  const auto *project = std::get_if<oursql::ProjectPlan>(&root->operation);
  if (project == nullptr || project->child == nullptr) return false;
  if (const auto *filter =
          std::get_if<oursql::FilterPlan>(&project->child->operation)) {
    return filter->child != nullptr &&
           std::holds_alternative<oursql::SeqScanPlan>(
               filter->child->operation);
  }
  return std::holds_alternative<oursql::SeqScanPlan>(
      project->child->operation);
}

bool TestSelectStarKeepsFrozenShape() {
  auto catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  oursql::Optimizer optimizer;

  auto parsed = parser.Parse("SELECT * FROM users;");
  if (!Check(parsed.ok(), "SELECT * 应可解析")) return false;
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok(), "SELECT * 应可规划")) return false;

  auto optimized = optimizer.Optimize(plan.value());
  if (!Check(optimized.ok(), "优化应成功")) return false;
  const auto &select = std::get<oursql::SelectPlan>(optimized.value());
  if (!Check(IsFrozenShape(select.root),
             "SELECT * 优化后仍必须是 Project(SeqScan)")) {
    return false;
  }
  const auto &project = std::get<oursql::ProjectPlan>(select.root->operation);
  if (!Check(project.select_all && project.columns.empty(),
             "SELECT * 的 ProjectPlan 应保留全列标记")) {
    return false;
  }
  const auto text = oursql::ToString(optimized.value());
  return Check(text.find("ProjectPlan") != std::string::npos &&
                   text.find("SeqScanPlan") != std::string::npos,
               "SELECT * 的计划文本应保留 ProjectPlan");
}

bool TestSelectWhereKeepsFrozenShape() {
  auto catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  oursql::Optimizer optimizer;

  auto parsed = parser.Parse("SELECT * FROM users WHERE id = 1;");
  if (!Check(parsed.ok(), "SELECT WHERE 应可解析")) return false;
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok(), "SELECT WHERE 应可规划")) return false;
  auto optimized = optimizer.Optimize(plan.value());
  if (!Check(optimized.ok(), "优化应成功")) return false;
  const auto &select = std::get<oursql::SelectPlan>(optimized.value());
  if (!Check(IsFrozenShape(select.root),
             "带 WHERE 时优化后必须是 Project(Filter(SeqScan))")) {
    return false;
  }
  const auto text = oursql::ToString(optimized.value());
  return Check(text.find("ProjectPlan") != std::string::npos &&
                   text.find("FilterPlan") != std::string::npos &&
                   text.find("SeqScanPlan") != std::string::npos,
               "带 WHERE 的计划文本应保留 Project/Filter/SeqScan 链");
}

bool TestExplicitProjectionKept() {
  auto catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  oursql::Optimizer optimizer;

  auto parsed = parser.Parse("SELECT id, name FROM users;");
  if (!Check(parsed.ok(), "显式投影应可解析")) return false;
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok(), "显式投影应可规划")) return false;
  auto optimized = optimizer.Optimize(plan.value());
  if (!Check(optimized.ok(), "优化应成功")) return false;
  const auto &select = std::get<oursql::SelectPlan>(optimized.value());
  const auto &project = std::get<oursql::ProjectPlan>(select.root->operation);
  return Check(project.columns == std::vector<std::string>{"id", "name"},
               "显式投影列不应被优化器改写");
}

bool TestNonConformingPlanRejected() {
  oursql::Optimizer optimizer;

  // 缺少 Project 根算子：SELECT * 的裁剪结果，必须被拒绝。
  oursql::Plan without_project = oursql::SelectPlan{
      "users", std::make_shared<oursql::PlanNode>(
                   oursql::SeqScanPlan{"users"})};
  auto missing_project = optimizer.Optimize(without_project);
  if (!Check(!missing_project.ok() &&
                 missing_project.status().code() ==
                     oursql::ErrorCode::InternalError,
             "缺少 Project 根算子的计划必须被拒绝")) {
    return false;
  }

  // Filter 缺少 SeqScan 子节点：同样是非法形态。
  auto dangling_filter = std::make_shared<oursql::PlanNode>(
      oursql::FilterPlan{nullptr, oursql::Predicate{"id", oursql::Value(1)}});
  oursql::Plan bad_filter = oursql::SelectPlan{
      "users",
      std::make_shared<oursql::PlanNode>(
          oursql::ProjectPlan{dangling_filter, {}, false})};
  auto rejected = optimizer.Optimize(bad_filter);
  return Check(!rejected.ok() &&
                   rejected.status().code() == oursql::ErrorCode::InternalError,
               "Filter 缺少 SeqScan 子节点的计划必须被拒绝");
}

bool TestOptimizeIsIdempotent() {
  auto catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  oursql::Optimizer optimizer;

  auto parsed = parser.Parse("SELECT * FROM users WHERE id = 1;");
  auto plan = planner.Build(parsed.value()[0], catalog);
  auto once = optimizer.Optimize(plan.value());
  auto twice = optimizer.Optimize(once.value());
  return Check(oursql::ToString(once.value()) == oursql::ToString(twice.value()),
               "重复优化不应改变计划");
}

bool TestNonSelectPlansUnchanged() {
  oursql::Optimizer optimizer;
  oursql::Plan create = oursql::CreateTablePlan{
      "t", oursql::Schema{{"id", oursql::DataType::Int}}};
  oursql::Plan insert = oursql::InsertPlan{
      "t", std::vector<oursql::Value>{oursql::Value(1)}};
  oursql::Plan remove = oursql::DeletePlan{"t", std::nullopt};
  const std::string create_text = oursql::ToString(create);
  const std::string insert_text = oursql::ToString(insert);
  const std::string remove_text = oursql::ToString(remove);
  auto optimized_create = optimizer.Optimize(create);
  auto optimized_insert = optimizer.Optimize(insert);
  auto optimized_remove = optimizer.Optimize(remove);
  return Check(optimized_create.ok() && optimized_insert.ok() &&
                   optimized_remove.ok() &&
                   oursql::ToString(optimized_create.value()) == create_text &&
                   oursql::ToString(optimized_insert.value()) == insert_text &&
                   oursql::ToString(optimized_remove.value()) == remove_text,
               "非 SELECT 计划应原样通过优化通道");
}

bool TestNestedProjectPruned() {
  oursql::Optimizer optimizer;
  auto scan = std::make_shared<oursql::PlanNode>(
      oursql::SeqScanPlan{"users"});
  auto inner = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{scan, {"id", "name"}, false});
  auto outer = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{inner, {"id"}, false});
  oursql::Plan plan = oursql::SelectPlan{"users", outer};

  auto optimized = optimizer.OptimizeWithStats(plan);
  if (!Check(optimized.ok(), "嵌套 Project 应可优化")) return false;
  const auto &select = std::get<oursql::SelectPlan>(optimized.value().plan);
  const auto &project =
      std::get<oursql::ProjectPlan>(select.root->operation);
  return Check(project.columns == std::vector<std::string>{"id"} &&
                   std::holds_alternative<oursql::SeqScanPlan>(
                       project.child->operation),
               "冗余内层 Project 应被裁剪") &&
         Check(optimized.value().stats.passes == 2,
               "一次改写后应再执行一轮稳定检查") &&
         Check(optimized.value().stats.rule_hits.at(
                   "R1:冗余内层Project裁剪") == 1,
               "规则命中次数应为 1");
}

bool TestSchedulerRepeatsUntilStable() {
  oursql::Optimizer optimizer;
  auto scan = std::make_shared<oursql::PlanNode>(
      oursql::SeqScanPlan{"users"});
  auto inner = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{scan, {"id", "name"}, false});
  auto middle = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{inner, {"id", "name"}, false});
  auto outer = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{middle, {"id"}, false});
  oursql::Plan plan = oursql::SelectPlan{"users", outer};

  auto optimized = optimizer.OptimizeWithStats(plan);
  if (!Check(optimized.ok(), "多层冗余 Project 应可优化")) return false;
  const auto &select =
      std::get<oursql::SelectPlan>(optimized.value().plan);
  const auto &project =
      std::get<oursql::ProjectPlan>(select.root->operation);
  return Check(project.child != nullptr &&
                   std::holds_alternative<oursql::SeqScanPlan>(
                       project.child->operation),
               "调度器应重复运行直到计划稳定") &&
         Check(optimized.value().stats.passes == 3,
               "两次改写后应进行第三轮稳定检查") &&
         Check(optimized.value().stats.rule_hits.at(
                   "R1:冗余内层Project裁剪") == 2,
               "连续两层冗余 Project 应累计两次规则命中");
}

bool TestSelectAllDoesNotPruneNarrowInnerProject() {
  oursql::Optimizer optimizer;
  auto scan = std::make_shared<oursql::PlanNode>(
      oursql::SeqScanPlan{"users"});
  auto inner = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{scan, {"id"}, false});
  auto outer = std::make_shared<oursql::PlanNode>(
      oursql::ProjectPlan{inner, {}, true});
  oursql::Plan plan = oursql::SelectPlan{"users", outer};

  auto optimized = optimizer.OptimizeWithStats(plan);
  // 如果 R1 错误地裁剪内层 Project，改写结果会满足冻结形态并成功返回；
  // 当前实现不能证明 SELECT * 的完整列集合仍然存在，因此必须拒绝原计划。
  return Check(!optimized.ok() &&
                   optimized.status().code() ==
                       oursql::ErrorCode::InternalError,
               "外层 SELECT * 不能裁掉提供窄列集合的内层 Project");
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) std::cout << "[PASS] " << name << '\n';
    else ++failures;
  };
  run("SELECT * keeps frozen shape", &TestSelectStarKeepsFrozenShape);
  run("SELECT WHERE keeps frozen shape", &TestSelectWhereKeepsFrozenShape);
  run("Explicit projection kept", &TestExplicitProjectionKept);
  run("Non-conforming plan rejected", &TestNonConformingPlanRejected);
  run("Optimize is idempotent", &TestOptimizeIsIdempotent);
  run("Non-SELECT plans unchanged", &TestNonSelectPlansUnchanged);
  run("Nested Project pruned", &TestNestedProjectPruned);
  run("Scheduler repeats until stable", &TestSchedulerRepeatsUntilStable);
  run("Unsafe SELECT * projection rejected",
      &TestSelectAllDoesNotPruneNarrowInnerProject);
  if (failures == 0) {
    std::cout << "All optimizer tests passed\n";
    return 0;
  }
  std::cerr << failures << " optimizer test(s) failed\n";
  return 1;
}
