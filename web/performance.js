"use strict";

const elements = {};

function byId(id) {
  return document.getElementById(id);
}

function make(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function number(value, maximumFractionDigits = 1) {
  return new Intl.NumberFormat("zh-CN", { maximumFractionDigits }).format(value);
}

function milliseconds(value) {
  if (value < 0.1) return `${number(value, 3)} ms`;
  if (value < 10) return `${number(value, 2)} ms`;
  return `${number(value, 1)} ms`;
}

async function request(path, options) {
  const response = await fetch(path, options);
  let payload;
  try {
    payload = await response.json();
  } catch {
    throw new Error(`服务器返回了无效响应 (${response.status})`);
  }
  if (!response.ok || !payload.ok) {
    throw new Error(payload?.error?.message || `请求失败 (${response.status})`);
  }
  return payload.data;
}

function setBusy(busy) {
  elements.runButton.disabled = busy;
  elements.rowCount.disabled = busy;
  elements.repetitions.disabled = busy;
  elements.runButton.querySelector("span").textContent = busy ? "正在运行…" : "运行真实对比";
  document.body.setAttribute("aria-busy", busy ? "true" : "false");
}

function showMessage(text, kind = "") {
  elements.message.replaceChildren();
  if (!text) return;
  const message = make("div", "message", text);
  if (kind) message.dataset.kind = kind;
  elements.message.append(message);
}

function setControlBusy(button, controls, busy, idleText, busyText) {
  button.disabled = busy;
  controls.forEach((control) => { control.disabled = busy; });
  button.querySelector("span").textContent = busy ? busyText : idleText;
  document.body.setAttribute("aria-busy", busy ? "true" : "false");
}

function showPanelMessage(container, text, kind = "") {
  container.replaceChildren();
  if (!text) return;
  const message = make("div", "message", text);
  if (kind) message.dataset.kind = kind;
  container.append(message);
}

function scenarioMap(data) {
  return new Map(data.scenarios.map((scenario) => [scenario.id, scenario]));
}

function renderLatencyChart(data) {
  elements.latencyChart.replaceChildren();
  const maximum = Math.max(...data.scenarios.map((scenario) => scenario.median_ms), 0.001);
  for (const scenario of data.scenarios) {
    const row = make("div", `latency-row latency-row-${scenario.access_path.toLowerCase()}`);
    const label = make("div", "latency-label", scenario.label);
    const track = make("div", "latency-track");
    const bar = make("div", "latency-bar");
    bar.style.width = `${Math.max(1.5, scenario.median_ms / maximum * 100)}%`;
    track.append(bar);
    const value = make("strong", "latency-value", milliseconds(scenario.median_ms));
    row.append(label, track, value);
    elements.latencyChart.append(row);
  }
}

function metric(label, value) {
  const item = make("div", "path-metric");
  item.append(make("span", "", label), make("strong", "", value));
  return item;
}

function renderPathCard(title, scenario, kind) {
  const card = make("article", `path-card path-card-${kind}`);
  const heading = make("div", "path-card-heading");
  const titleGroup = make("div");
  titleGroup.append(make("span", "path-kicker", kind === "before" ? "优化前" : "优化后"));
  titleGroup.append(make("h3", "", title));
  const badge = make("span", "access-path-badge", scenario.access_path);
  heading.append(titleGroup, badge);

  const plan = make("pre", "plan-output");
  plan.append(make("code", "", scenario.plan));
  const stats = scenario.statistics;
  const metrics = make("div", "path-metrics");
  metrics.append(
    metric("查询耗时", milliseconds(scenario.median_ms)),
    metric("Buffer 访问", number(stats.buffer_accesses)),
    metric("磁盘读取", number(stats.disk_reads))
  );
  card.append(heading, plan, metrics);
  return card;
}

function renderPathComparison(data) {
  const scenarios = scenarioMap(data);
  const sequential = scenarios.get("cold-seq");
  const indexed = scenarios.get("cold-index");
  elements.pathComparison.replaceChildren(
    renderPathCard("全表逐行扫描", sequential, "before"),
    renderPathCard("B+ 树定位记录", indexed, "after")
  );
  const correct = sequential.rows === indexed.rows && sequential.rows === 1;
  elements.correctnessCheck.textContent = correct
    ? "✓ 两条路径均返回相同的 1 行"
    : "结果一致性检查失败";
  elements.correctnessCheck.dataset.state = correct ? "success" : "error";
}

function renderCompiler(data) {
  const compiler = data.compiler;
  elements.compilerTokens.replaceChildren();
  for (const token of compiler.tokens) {
    const item = make("span", "token-item");
    item.append(
      make("strong", "", token.lexeme || token.type),
      make("small", "", token.type)
    );
    elements.compilerTokens.append(item);
  }
  elements.compilerAst.textContent = compiler.ast;
  elements.expressionBefore.textContent = compiler.expression_before;
  elements.expressionAfter.textContent = compiler.expression_after;
  elements.optimizerRules.replaceChildren();
  for (const rule of compiler.rules) {
    const item = document.createElement("li");
    item.textContent = rule;
    elements.optimizerRules.append(item);
  }
  elements.compilerSummary.textContent =
    `${compiler.tokens.length} Tokens · 4 个编译阶段`;
  if (window.lucide) window.lucide.createIcons();
}

function renderMetrics(data) {
  elements.metricsBody.replaceChildren();
  for (const scenario of data.scenarios) {
    const stats = scenario.statistics;
    const row = document.createElement("tr");
    const values = [
      scenario.label,
      milliseconds(scenario.median_ms),
      number(stats.buffer_accesses),
      `${number(stats.buffer_hit_rate * 100, 1)}%`,
      number(stats.evictions),
      number(stats.disk_reads),
      number(stats.disk_writes),
    ];
    values.forEach((value, index) => {
      const cell = document.createElement(index === 0 ? "th" : "td");
      if (index === 0) cell.scope = "row";
      cell.textContent = value;
      row.append(cell);
    });
    elements.metricsBody.append(row);
  }
}

function renderResults(data) {
  const scenarios = scenarioMap(data);
  const coldSeq = scenarios.get("cold-seq");
  const coldIndex = scenarios.get("cold-index");
  const accessReduction = coldSeq.statistics.buffer_accesses === 0
    ? 0
    : Math.max(0, 1 - coldIndex.statistics.buffer_accesses /
        coldSeq.statistics.buffer_accesses);

  elements.speedup.textContent = `${number(data.comparisons.speedup, 1)}×`;
  elements.accessReduction.textContent = `${number(accessReduction * 100, 1)}%`;
  elements.query.textContent = data.metadata.query;
  elements.method.textContent = `${number(data.metadata.row_count, 0)} 行 · ` +
    `${data.metadata.repetitions} 次取中位数 · ${data.metadata.pool_size} frames`;

  renderLatencyChart(data);
  renderCompiler(data);
  renderPathComparison(data);
  renderMetrics(data);
  elements.empty.hidden = true;
  elements.results.hidden = false;
}

async function runBenchmark() {
  setBusy(true);
  showMessage("正在创建等价数据副本并运行四组查询，数据规模越大耗时越久…");
  try {
    const started = performance.now();
    const data = await request("/api/benchmark/index", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        row_count: Number(elements.rowCount.value),
        repetitions: Number(elements.repetitions.value),
      }),
    });
    renderResults(data);
    showMessage(`实验完成，完整实验耗时 ${milliseconds(performance.now() - started)}`, "success");
  } catch (error) {
    showMessage(error.message, "error");
  } finally {
    setBusy(false);
  }
}

function costDecisionCard(scenario) {
  const card = make("article", "cost-decision-card");
  const heading = make("div", "cost-decision-heading");
  const title = make("div");
  title.append(make("span", "path-kicker", scenario.id === "common" ? "高选择率" : "低选择率"));
  title.append(make("h3", "", scenario.id === "common" ? "gender = 1" : "gender = 0"));
  heading.append(title, make("span", "access-path-badge", scenario.access_path));

  const explanation = make(
    "p",
    "cost-explanation",
    scenario.access_path === "SeqScan"
      ? `预计返回 ${number(scenario.matching_rows, 0)} 行，走索引回表代价更高。`
      : `预计只返回 ${number(scenario.matching_rows, 0)} 行，B+ 树定位更划算。`
  );
  const bars = make("div", "cost-bars");
  const maximum = Math.max(scenario.seq_cost, scenario.index_cost);
  for (const [label, value] of [["SeqScan Cost", scenario.seq_cost], ["IndexScan Cost", scenario.index_cost]]) {
    const row = make("div", "cost-row");
    row.append(make("span", "", label));
    const track = make("div", "cost-track");
    const bar = make("div", `cost-bar ${label.startsWith(scenario.access_path) ? "is-chosen" : ""}`);
    bar.style.width = `${value / maximum * 100}%`;
    track.append(bar);
    row.append(track, make("strong", "", number(value, 0)));
    bars.append(row);
  }
  const plan = make("pre", "plan-output cost-plan");
  plan.append(make("code", "", scenario.plan));
  card.append(heading, explanation, bars, plan);
  return card;
}

function renderSelectivity(data) {
  elements.costFormula.textContent = data.metadata.cost_formula;
  elements.selectivityMethod.textContent =
    `${number(data.metadata.row_count, 0)} 行 · ${data.metadata.repetitions} 次取中位数`;
  elements.costDecisionGrid.replaceChildren(
    ...data.scenarios.map(costDecisionCard)
  );
  elements.selectivityMetricsBody.replaceChildren();
  for (const scenario of data.scenarios) {
    const row = document.createElement("tr");
    const values = [
      scenario.id === "common" ? "gender = 1" : "gender = 0",
      `${number(scenario.selectivity * 100, 0)}%`,
      scenario.access_path,
      number(scenario.seq_cost, 0),
      number(scenario.index_cost, 0),
      milliseconds(scenario.median_ms),
      number(scenario.rows, 0),
    ];
    values.forEach((value, index) => {
      const cell = document.createElement(index === 0 ? "th" : "td");
      if (index === 0) cell.scope = "row";
      cell.textContent = value;
      row.append(cell);
    });
    elements.selectivityMetricsBody.append(row);
  }
  elements.selectivityEmpty.hidden = true;
  elements.selectivityResults.hidden = false;
}

async function runSelectivity() {
  setControlBusy(
    elements.runSelectivity,
    [elements.selectivityRowCount, elements.selectivityRepetitions],
    true,
    "运行成本决策",
    "正在运行…"
  );
  showPanelMessage(elements.selectivityMessage, "正在生成 90/10 数据分布并验证成本决策…");
  try {
    const data = await request("/api/benchmark/selectivity", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        row_count: Number(elements.selectivityRowCount.value),
        repetitions: Number(elements.selectivityRepetitions.value),
      }),
    });
    renderSelectivity(data);
    showPanelMessage(elements.selectivityMessage, "成本优化实验完成。", "success");
  } catch (error) {
    showPanelMessage(elements.selectivityMessage, error.message, "error");
  } finally {
    setControlBusy(
      elements.runSelectivity,
      [elements.selectivityRowCount, elements.selectivityRepetitions],
      false,
      "运行成本决策",
      "正在运行…"
    );
  }
}

function renderBuffer(data) {
  const sorted = [...data.policies].sort((left, right) =>
    right.hit_rate - left.hit_rate || left.disk_reads - right.disk_reads
  );
  elements.bestHitRate.textContent = `${number(sorted[0].hit_rate * 100, 1)}%`;
  elements.bestPolicy.textContent = `${sorted[0].policy} · 当前访问模式`;
  elements.fewestReads.textContent = number(
    Math.min(...data.policies.map((policy) => policy.disk_reads)),
    0
  );
  const patternNames = { hotspot: "80/20 热点", random: "随机", sequential: "顺序" };
  elements.bufferMethod.textContent =
    `${patternNames[data.metadata.access_pattern]}访问 · ${data.metadata.pool_size} frames · ` +
    `${data.metadata.request_count} 次请求`;

  elements.bufferPolicyChart.replaceChildren();
  for (const policy of data.policies) {
    const item = make("article", "buffer-policy-item");
    const heading = make("div", "buffer-policy-heading");
    heading.append(make("h3", "", policy.policy),
                   make("strong", "", `${number(policy.hit_rate * 100, 1)}%`));
    const track = make("div", "buffer-hit-track");
    const bar = make("div", "buffer-hit-bar");
    bar.style.width = `${policy.hit_rate * 100}%`;
    track.append(bar);
    const detail = make(
      "p",
      "",
      `${number(policy.hits, 0)} 次命中 · ${number(policy.evictions, 0)} 次淘汰 · ` +
        `${number(policy.disk_reads, 0)} 次磁盘读取`
    );
    item.append(heading, track, detail);
    elements.bufferPolicyChart.append(item);
  }

  elements.bufferMetricsBody.replaceChildren();
  for (const policy of data.policies) {
    const row = document.createElement("tr");
    const values = [
      policy.policy,
      `${number(policy.hit_rate * 100, 1)}%`,
      number(policy.hits, 0),
      number(policy.misses, 0),
      number(policy.evictions, 0),
      number(policy.disk_reads, 0),
      milliseconds(policy.elapsed_ms),
    ];
    values.forEach((value, index) => {
      const cell = document.createElement(index === 0 ? "th" : "td");
      if (index === 0) cell.scope = "row";
      cell.textContent = value;
      row.append(cell);
    });
    elements.bufferMetricsBody.append(row);
  }
  elements.bufferEmpty.hidden = true;
  elements.bufferResults.hidden = false;
}

async function runBuffer() {
  setControlBusy(elements.runBuffer, [elements.accessPattern], true,
                 "运行策略对比", "正在运行…");
  showPanelMessage(elements.bufferMessage, "正在重放固定页面访问序列…");
  try {
    const data = await request("/api/benchmark/buffer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ access_pattern: elements.accessPattern.value }),
    });
    renderBuffer(data);
    showPanelMessage(elements.bufferMessage, "页面替换策略实验完成。", "success");
  } catch (error) {
    showPanelMessage(elements.bufferMessage, error.message, "error");
  } finally {
    setControlBusy(elements.runBuffer, [elements.accessPattern], false,
                   "运行策略对比", "正在运行…");
  }
}

function selectExperiment(name) {
  document.querySelectorAll(".experiment-tab").forEach((button) => {
    const selected = button.dataset.experiment === name;
    button.classList.toggle("is-active", selected);
    button.setAttribute("aria-selected", String(selected));
  });
  document.querySelectorAll(".experiment-panel").forEach((panel) => {
    panel.hidden = panel.id !== `${name}Experiment`;
  });
}

function initialize() {
  elements.runButton = byId("runBenchmark");
  elements.rowCount = byId("rowCount");
  elements.repetitions = byId("repetitions");
  elements.message = byId("benchmarkMessage");
  elements.empty = byId("benchmarkEmpty");
  elements.results = byId("benchmarkResults");
  elements.speedup = byId("speedup");
  elements.accessReduction = byId("accessReduction");
  elements.query = byId("benchmarkQuery");
  elements.method = byId("methodLabel");
  elements.latencyChart = byId("latencyChart");
  elements.compilerTokens = byId("compilerTokens");
  elements.compilerAst = byId("compilerAst");
  elements.expressionBefore = byId("expressionBefore");
  elements.expressionAfter = byId("expressionAfter");
  elements.optimizerRules = byId("optimizerRules");
  elements.compilerSummary = byId("compilerSummary");
  elements.pathComparison = byId("pathComparison");
  elements.correctnessCheck = byId("correctnessCheck");
  elements.metricsBody = byId("metricsBody");
  elements.selectivityRowCount = byId("selectivityRowCount");
  elements.selectivityRepetitions = byId("selectivityRepetitions");
  elements.runSelectivity = byId("runSelectivity");
  elements.selectivityMessage = byId("selectivityMessage");
  elements.selectivityEmpty = byId("selectivityEmpty");
  elements.selectivityResults = byId("selectivityResults");
  elements.costFormula = byId("costFormula");
  elements.selectivityMethod = byId("selectivityMethod");
  elements.costDecisionGrid = byId("costDecisionGrid");
  elements.selectivityMetricsBody = byId("selectivityMetricsBody");
  elements.accessPattern = byId("accessPattern");
  elements.runBuffer = byId("runBuffer");
  elements.bufferMessage = byId("bufferMessage");
  elements.bufferEmpty = byId("bufferEmpty");
  elements.bufferResults = byId("bufferResults");
  elements.bestHitRate = byId("bestHitRate");
  elements.bestPolicy = byId("bestPolicy");
  elements.fewestReads = byId("fewestReads");
  elements.bufferMethod = byId("bufferMethod");
  elements.bufferPolicyChart = byId("bufferPolicyChart");
  elements.bufferMetricsBody = byId("bufferMetricsBody");
  elements.runButton.addEventListener("click", runBenchmark);
  elements.runSelectivity.addEventListener("click", runSelectivity);
  elements.runBuffer.addEventListener("click", runBuffer);
  document.querySelectorAll(".experiment-tab").forEach((button) => {
    button.addEventListener("click", () => selectExperiment(button.dataset.experiment));
  });
  if (window.lucide) window.lucide.createIcons();
}

document.addEventListener("DOMContentLoaded", initialize);
