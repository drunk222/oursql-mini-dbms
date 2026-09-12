#pragma once

#include "oursql/ast/statement.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oursql {

// 逻辑计划输出的编译层类型。数据库执行层只存 INT/VARCHAR；BOOL 只可能来自
// SELECT 表达式或子查询内部，用于编译期类型推导。
enum class PlanValueType { Int, Varchar, Bool, Null };

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

// 二元 JOIN：左右子树分别产出行，按指定列等值连接。多表 JOIN 使用左深
// JoinPlan 组合，左侧可以是另一个 JoinPlan；下标均为对应输入 Schema 的列下标。
struct JoinPlan {
  std::shared_ptr<const PlanNode> left;
  std::shared_ptr<const PlanNode> right;
  JoinType join_type{JoinType::Inner};
  std::string left_table;
  std::string left_column;
  std::string right_table;
  std::string right_column;
  std::optional<std::size_t> left_column_index;
  std::optional<std::size_t> right_column_index;
};

// 投影节点携带最终输出列，或 select_all 全列标记。
// child 使用 shared_ptr<const>，保证 Executor 无法修改计划内容。
struct ProjectPlan {
  std::shared_ptr<const PlanNode> child;
  std::vector<std::string> columns;
  bool select_all{false};
  // 当非空时表示输入 Row 中要读取的列下标；空时兼容按名称查找。
  std::vector<std::size_t> input_indexes;
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
      std::variant<SeqScanPlan, IndexScanPlan, FilterPlan, JoinPlan,
                   GroupByPlan, OrderByPlan,
                   ProjectPlan>;

  explicit PlanNode(Operation operation_value) : operation(std::move(operation_value)) {}
  Operation operation;
};

// SELECT 的根节点固定为 Project，其下按 Planner 产生的算子链读取数据。
// distinct 和 limit 是编译层支持的根计划修饰符；当前 Executor 会明确拒绝。
// projection_aliases 与 root Project 的 columns 等长；table_alias 保存 AS 表别名。
// projection_expressions 和 having 保存聚合相关的编译层表达式。
// output_types 与 Project 输出列等长，供外层子查询做类型推导；执行器不依赖它。
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
  std::vector<PlanValueType> output_types;
};

// 删除计划。where 为空表示删除整表；复杂 WHERE 在执行前会被 Planner 拒绝。
struct DeletePlan {
  std::string table_name;
  std::optional<Predicate> where;
};

// DROP TABLE 计划。当前执行层明确返回 NotImplemented。
struct DropTablePlan {
  std::string table_name;
};

// ALTER TABLE 计划。RenameTable/RenameColumn 使用 new_name；AddColumn 使用
// column 和可选 default_value。执行阶段负责原子更新 Catalog 与已有数据。
struct AlterTablePlan {
  std::string table_name;
  AlterTableAction action{AlterTableAction::RenameTable};
  std::string new_name;
  std::string column_name;
  Column column;
  std::optional<Value> default_value;
};

// 事务控制计划。DatabaseEngine 作为单会话协调器解释该节点，ExecutionEngine
// 本身不保存事务状态。
struct TransactionPlan {
  TransactionAction action{TransactionAction::Begin};
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
                          ExplainPlan, UpdatePlan, AlterTablePlan,
                          TransactionPlan>;

// 调用者：调试器或测试；作用：以稳定格式打印逻辑计划；返回：计划文本，不参与执行。
[[nodiscard]] std::string ToString(const Plan &plan);

}  // namespace oursql
