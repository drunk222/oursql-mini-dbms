#pragma once

#include "oursql/ast/statement.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// 编译层逻辑计划数据模型。
//
// Plan 是 Planner 语义检查后的结构化结果。SELECT 的计划节点使用
// shared_ptr<const PlanNode> 连接，执行阶段只能读取；DDL/DML 计划则保存
// 已规范化的表名、Schema、行值和约束信息。Plan 不保存页面、Guard 或执行器
// 对象，具体数据访问由 ExecutionEngine 完成。
namespace oursql {

// 逻辑计划输出的编译层类型。数据库执行层只存 INT/VARCHAR；BOOL 只可能来自
// SELECT 表达式或子查询内部，用于编译期类型推导。
enum class PlanValueType {
  Int,      // 编译层 INT 表达式
  Varchar,  // 编译层 VARCHAR 表达式
  Bool,     // 谓词、比较或逻辑表达式
  Null      // 常量 NULL 或仅由 NULL 推导出的结果
};

// DDL 计划：执行器根据 Schema 创建物理表和 Catalog 元数据。
struct CreateTablePlan {
  std::string table_name;
  Schema schema;
};

// DML 插入计划：columns 为空时按表定义顺序对应全列；columns 非空时按指定
// 列顺序对应。rows 中的值已经在编译阶段完成数量、类型、NULL、长度和默认值
// 规范化。当前执行器只接受“一行、无显式列列表”的形态，其余计划会显式
// 返回 NotImplemented，避免多行写入被静默截断。
struct InsertPlan {
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

// 表扫描是 SELECT 计划树的叶子，执行器只按表名获取 HeapTable。
struct SeqScanPlan {
  std::string table_name;
};

// 等值索引点查计划。当前 Optimizer 只在 INT 列等值谓词存在匹配索引时生成；
// 执行器仍保留外层 Filter，用同一条件复核索引返回的 RID，不能只相信索引。
struct IndexScanPlan {
  std::string table_name;
  std::string index_name;
  std::string column_name;
  Value key;
};

struct PlanNode;

// predicate 保留可供索引优化的简单条件；expression 保留完整 WHERE
// 表达式供执行器逐行求值。
struct FilterPlan {
  std::shared_ptr<const PlanNode> child;
  Predicate predicate;
  CompileExprPtr expression;
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

// 分组节点。GROUP BY 的列会形成分组键；带聚合函数时由聚合执行路径按组
// 计算，HAVING 在聚合完成后过滤分组。
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
// distinct 和 limit 是根计划修饰符，由执行器在投影或聚合完成后应用：
// DISTINCT 按输出行去重，LIMIT 按行数截断。
// projection_aliases 与 root Project 的 columns 等长；table_alias 保存 AS 表别名。
// projection_expressions 与 root Project 的 columns 一一对应，可保存列引用、
// 常量或一般表达式；having 保存聚合后的分组过滤表达式。
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

// 删除计划。where 为空表示删除整表；当前 DeleteExecutor 只执行可提取为
// 简单 Predicate 的 WHERE，复杂表达式会在 Planner 阶段明确拒绝。
struct DeletePlan {
  std::string table_name;
  std::optional<Predicate> where;
};

// DROP TABLE 计划。执行器按“删除索引、释放数据页链、删除 Catalog 表记录”
// 的顺序完成删除；Planner 只负责确认目标表存在。
struct DropTablePlan {
  std::string table_name;
};

// ALTER TABLE 计划。RenameTable/RenameColumn 使用 new_name；AddColumn 使用
// column 和可选 default_value。编译阶段已完成语义检查，但执行器尚未接入
// Catalog/已有数据的原子改写，因此当前会明确返回 NotImplemented。
struct AlterTablePlan {
  std::string table_name;
  AlterTableAction action{AlterTableAction::RenameTable};
  std::string new_name;
  std::string column_name;
  Column column;
  std::optional<Value> default_value;
};

// DatabaseEngine 调度的事务控制 Plan；ExecutionEngine 不维护事务状态。
struct BeginPlan {};
struct CommitPlan {};
struct RollbackPlan {};

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

// 更新计划。assignments 保存 SET 列和右值表达式，where 保存可执行的简单
// Predicate。UpdateExecutor 会更新 Heap 并同步维护现有索引；复杂 WHERE
// 目前仍在 Planner 阶段被拒绝。
struct UpdatePlan {
  std::string table_name;
  std::vector<UpdateAssignment> assignments;
  std::optional<Predicate> where;
};

// 顶层计划集合。传给 Executor 后，Executor 只按节点字段执行，不重新解析 SQL。
using Plan = std::variant<CreateTablePlan, InsertPlan, SelectPlan, DeletePlan,
                          DropTablePlan, CreateIndexPlan, DropIndexPlan,
                          ExplainPlan, UpdatePlan, AlterTablePlan,
                          BeginPlan, CommitPlan, RollbackPlan>;

// 调用者：调试器或测试；作用：以稳定格式打印逻辑计划；返回：计划文本，不参与执行。
[[nodiscard]] std::string ToString(const Plan &plan);

}  // namespace oursql
