#pragma once

#include "oursql/ast/statement.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oursql {

// DDL 计划：执行器根据 Schema 创建物理表和 Catalog 元数据。
struct CreateTablePlan {
  std::string table_name;
  Schema schema;
};

// DML 插入计划：columns 为空时按表定义顺序写入所有列；否则按指定列顺序写入。
// rows 中的每个值已经在编译阶段完成数量、类型和长度检查。
struct InsertPlan {
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

// 表扫描是 SELECT 计划树的叶子，执行器只按表名获取 HeapTable。
struct SeqScanPlan {
  std::string table_name;
};

// 等值索引点查。key 的下标来自索引列，执行器仍需用 Filter 复核条件。
struct IndexScanPlan {
  std::string table_name;
  std::string index_name;
  std::string column_name;
  Value key;
};

struct PlanNode;

// 过滤节点只表达数据库当前可执行的等值 Predicate；复杂 WHERE 不会伪装成
// 该节点进入执行层。
struct FilterPlan {
  std::shared_ptr<const PlanNode> child;
  Predicate predicate;
};

// 投影节点携带最终输出列，或 select_all 全列标记。
// child 使用 shared_ptr<const>，保证 Executor 无法修改计划内容。
struct ProjectPlan {
  std::shared_ptr<const PlanNode> child;
  std::vector<std::string> columns;
  bool select_all{false};
};

// 编译层分组节点。当前 Executor 会明确返回 NotImplemented。
struct GroupByPlan {
  std::shared_ptr<const PlanNode> child;
  std::vector<std::string> columns;
};

// 编译层排序节点。keys 保持 SQL 中的优先级顺序。
struct OrderByPlan {
  std::shared_ptr<const PlanNode> child;
  std::vector<OrderKey> keys;
};

// 计划树节点使用变体组合所有逻辑算子，避免 Executor 依据 SQL 字符串猜测行为。
struct PlanNode {
  using Operation =
      std::variant<SeqScanPlan, IndexScanPlan, FilterPlan, GroupByPlan, OrderByPlan,
                   ProjectPlan>;

  explicit PlanNode(Operation operation_value) : operation(std::move(operation_value)) {}
  Operation operation;
};

// SELECT 的根节点固定为 Project，其下按 Planner 产生的算子链读取数据。
// distinct 和 limit 是编译层支持的根计划修饰符；当前 Executor 会明确拒绝。
// projection_aliases 与 root Project 的 columns 等长；table_alias 保存 AS 表别名。
// projection_expressions 和 having 保存聚合相关的编译层表达式。
struct SelectPlan {
  std::string table_name;
  std::shared_ptr<const PlanNode> root;
  bool distinct{false};
  std::optional<std::size_t> limit;
  std::vector<std::string> projection_aliases;
  std::string table_alias;
  std::vector<CompileExprPtr> projection_expressions;
  CompileExprPtr having;
  bool has_aggregates{false};
};

// 删除计划。where 为空表示删除整表；复杂 WHERE 在执行前会被 Planner 拒绝。
struct DeletePlan {
  std::string table_name;
  std::optional<Predicate> where;
};

// DROP TABLE 计划。执行层依次释放索引、数据页链和表元数据。
struct DropTablePlan {
  std::string table_name;
};

// CREATE INDEX 计划，执行时交给 IndexManager 构建索引并登记元数据。
struct CreateIndexPlan {
  std::string index_name;
  std::string table_name;
  std::string column_name;
  bool is_unique{false};
};

// DROP INDEX 计划。
struct DropIndexPlan {
  std::string index_name;
};

// EXPLAIN 不执行内部 SELECT，只输出其逻辑计划文本。
struct ExplainPlan {
  SelectPlan select;
};

// 更新计划。当前只完成编译层表示和语义检查，执行层明确返回 NotImplemented。
struct UpdatePlan {
  std::string table_name;
  std::vector<UpdateAssignment> assignments;
  std::optional<Predicate> where;
};

// 顶层计划集合。传给 Executor 后，Executor 只按节点字段执行，不重新解析 SQL。
using Plan = std::variant<CreateTablePlan, InsertPlan, SelectPlan, DeletePlan,
                          DropTablePlan, CreateIndexPlan, DropIndexPlan,
                          ExplainPlan, UpdatePlan>;

// 调用者：调试器或测试；作用：以稳定格式打印逻辑计划；返回：计划文本，不参与执行。
[[nodiscard]] std::string ToString(const Plan &plan);

}  // namespace oursql
