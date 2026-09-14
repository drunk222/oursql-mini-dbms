"use strict";

const elements = {};

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setBusy(busy) {
  elements.runDemoButton.disabled = busy;
  document.body.setAttribute("aria-busy", busy ? "true" : "false");
}

function setMessage(text, kind = "") {
  elements.demoMessage.replaceChildren();
  if (!text) return;
  const message = element("div", "message", text);
  if (kind) message.dataset.kind = kind;
  elements.demoMessage.append(message);
}

async function request(path, options = {}) {
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

function renderSummary(data) {
  elements.demoSummary.replaceChildren();
  const scenarios = data.scenarios || [];
  const passed = scenarios.filter((scenario) => scenario.passed).length;
  const duration = scenarios.reduce(
    (total, scenario) => total + Number(scenario.duration_ms || 0),
    0
  );

  const passedItem = element("div", "summary-stat");
  passedItem.append(
    element("span", "summary-value", `${passed}/${scenarios.length}`),
    element("span", "summary-label", "场景通过")
  );
  const durationItem = element("div", "summary-stat");
  durationItem.append(
    element("span", "summary-value", `${duration} ms`),
    element("span", "summary-label", "总耗时")
  );
  elements.demoSummary.append(passedItem, durationItem);
}

function renderMetrics(metrics) {
  const list = element("dl", "demo-metrics");
  for (const metric of metrics || []) {
    list.append(
      element("dt", "", metric.label),
      element("dd", "", metric.value)
    );
  }
  return list;
}

function renderTimeline(events) {
  const scroll = element("div", "timeline-scroll");
  const table = element("table", "timeline-table");
  const head = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const label of ["时间", "会话", "动作", "详情", "状态"]) {
    headRow.append(element("th", "", label));
  }
  head.append(headRow);

  const body = document.createElement("tbody");
  for (const event of events || []) {
    const row = document.createElement("tr");
    row.append(
      element("td", "timeline-time", `${Number(event.offset_ms || 0).toFixed(1)} ms`),
      element("td", "", event.session || "-"),
      element("td", "timeline-action", event.action || "-"),
      element("td", "", event.detail || "-")
    );
    const statusCell = document.createElement("td");
    const status = element(
      "span",
      `event-status event-status--${event.status || "info"}`,
      event.status || "info"
    );
    statusCell.append(status);
    row.append(statusCell);
    body.append(row);
  }
  table.append(head, body);
  scroll.append(table);
  return scroll;
}

function renderScenario(scenario) {
  const section = element("section", "scenario");
  const header = element("div", "scenario-header");
  const title = element("div");
  title.append(
    element("h2", "", scenario.title),
    element("p", "scenario-purpose", scenario.purpose)
  );
  const status = element(
    "span",
    `status-pill ${scenario.passed ? "status-pill--success" : "status-pill--error"}`,
    scenario.passed ? "通过" : "失败"
  );
  const duration = element("span", "scenario-duration", `${scenario.duration_ms} ms`);
  header.append(title, status, duration);
  section.append(header);

  if (scenario.error) {
    section.append(element("div", "message", scenario.error));
  }
  section.append(renderMetrics(scenario.metrics), renderTimeline(scenario.timeline));
  return section;
}

async function runDemo() {
  setBusy(true);
  setMessage("正在运行并发场景");
  elements.scenarioList.replaceChildren();

  try {
    const data = await request("/api/concurrency/demo", { method: "POST" });
    renderSummary(data);
    for (const scenario of data.scenarios || []) {
      elements.scenarioList.append(renderScenario(scenario));
    }
    setMessage(
      data.passed ? "全部并发场景已完成" : "部分并发场景未通过",
      data.passed ? "success" : "error"
    );
  } catch (error) {
    elements.demoSummary.replaceChildren(element("span", "muted", "运行失败"));
    setMessage(error.message, "error");
  } finally {
    setBusy(false);
  }
}

function init() {
  elements.runDemoButton = document.getElementById("runDemoButton");
  elements.demoSummary = document.getElementById("demoSummary");
  elements.demoMessage = document.getElementById("demoMessage");
  elements.scenarioList = document.getElementById("scenarioList");
  elements.runDemoButton.addEventListener("click", runDemo);
  if (window.lucide) window.lucide.createIcons();
  runDemo();
}

document.addEventListener("DOMContentLoaded", init);
