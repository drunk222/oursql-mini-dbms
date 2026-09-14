"use strict";

const elements = {};
const sessions = [];
let busy = false;

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setBusy(nextBusy) {
  busy = nextBusy;
  elements.runConcurrentButton.disabled = nextBusy;
  elements.sessionCount.disabled = nextBusy;
  document.body.setAttribute("aria-busy", nextBusy ? "true" : "false");
}

function setMessage(text, kind = "") {
  elements.concurrencyMessage.replaceChildren();
  if (!text) return;
  const message = element("div", "message", text);
  if (kind) message.dataset.kind = kind;
  elements.concurrencyMessage.append(message);
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

function syncSqlFromDom() {
  const editors = elements.sessionGrid.querySelectorAll(".concurrent-sql-editor");
  editors.forEach((editor, index) => {
    if (sessions[index]) sessions[index].sql = editor.value;
  });
}

function renderCell(value) {
  const cell = document.createElement("td");
  if (value.type === "null") {
    cell.textContent = "NULL";
    cell.classList.add("null-cell");
  } else {
    cell.textContent = String(value.value ?? "");
  }
  cell.title = cell.textContent;
  return cell;
}

function renderExecutionResult(result) {
  const duration = `${Number(result.duration_ms || 0).toFixed(1)} ms`;
  if (!result.columns || result.columns.length === 0) {
    const line = element("div", "execution-line");
    const time = element("span", "concurrent-result-time", duration);
    time.title = "从脚本开始执行到该条语句完成";
    line.append(
      element("span", "", "执行成功"),
      time
    );
    return line;
  }

  const block = element("section", "result-block");
  const header = element("div", "result-block-header");
  const time = element("span", "concurrent-result-time", duration);
  time.title = "从脚本开始执行到该条语句完成";
  header.append(
    element("span", "", `${result.columns.length} 列`),
    element("span", "", `${result.rows?.length || 0} 行`),
    time
  );
  block.append(header);

  const scroll = element("div", "table-scroll");
  const table = element("table", "data-table");
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const column of result.columns) headRow.append(element("th", "", column));
  thead.append(headRow);
  table.append(thead);

  const tbody = document.createElement("tbody");
  for (const row of result.rows || []) {
    const tableRow = document.createElement("tr");
    for (const value of row) tableRow.append(renderCell(value));
    tbody.append(tableRow);
  }
  table.append(tbody);
  scroll.append(table);
  block.append(scroll);
  if (!result.rows?.length) {
    block.append(element("div", "empty-state", "查询成功，没有返回数据"));
  }
  return block;
}

function renderSession(session, index) {
  const panel = element("article", "concurrent-session");
  panel.dataset.session = String(index + 1);

  const header = element("div", "concurrent-session-header");
  const status = element("span", "concurrent-session-status", "等待运行");
  status.dataset.state = "idle";
  header.append(
    element("h2", "", `会话 ${index + 1}`),
    status
  );

  const editor = element("textarea", "concurrent-sql-editor");
  editor.value = session.sql;
  editor.placeholder = `SQL ${index + 1}`;
  editor.spellcheck = false;
  editor.setAttribute("aria-label", `会话 ${index + 1} SQL`);

  const results = element("div", "concurrent-session-results");
  results.setAttribute("aria-live", "polite");

  panel.append(header, editor, results);
  session.nodes = { panel, status, results };
  return panel;
}

function renderSessions() {
  syncSqlFromDom();
  const count = Number(elements.sessionCount.value);
  while (sessions.length < count) sessions.push({ sql: "", nodes: null });
  sessions.length = count;

  const fragment = document.createDocumentFragment();
  sessions.forEach((session, index) => {
    session.nodes = null;
    fragment.append(renderSession(session, index));
  });
  elements.sessionGrid.replaceChildren(fragment);
  elements.sessionGrid.style.setProperty("--session-count", String(count));
  if (window.lucide) window.lucide.createIcons();
}

function renderOutcome(index, outcome) {
  const session = sessions[index];
  if (!session?.nodes) return;
  const { status, results } = session.nodes;
  const succeeded = Boolean(outcome.ok);
  status.textContent = succeeded ? "完成" : "失败";
  status.dataset.state = succeeded ? "success" : "error";
  results.replaceChildren();

  if (!succeeded) {
    const message = element(
      "div",
      "message",
      outcome.error?.message || "执行失败"
    );
    message.dataset.kind = "error";
    const errorRow = element("div", "concurrent-result-error");
    errorRow.append(
      message,
      element(
        "span",
        "concurrent-result-time",
        `${Number(outcome.duration_ms || 0).toFixed(1)} ms`
      )
    );
    results.append(errorRow);
    return;
  }

  const executionResults = outcome.results || [];
  if (!executionResults.length) {
    const line = element("div", "execution-line");
    line.append(
      element("span", "", "执行成功"),
      element(
        "span",
        "concurrent-result-time",
        `${Number(outcome.duration_ms || 0).toFixed(1)} ms`
      )
    );
    results.append(line);
    return;
  }
  for (const result of executionResults) {
    results.append(renderExecutionResult(result));
  }
}

async function runConcurrent() {
  if (busy) return;
  syncSqlFromDom();
  setBusy(true);
  setMessage("正在并发执行");
  sessions.forEach((session) => {
    if (!session.nodes) return;
    session.nodes.status.textContent = "运行中";
    session.nodes.status.dataset.state = "running";
    session.nodes.results.replaceChildren();
  });

  try {
    const data = await request("/api/concurrency/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sql: sessions.map((session) => session.sql) }),
    });
    const outcomes = data.results || [];
    for (let index = 0; index < sessions.length; ++index) {
      renderOutcome(
        index,
        outcomes[index] || {
          ok: false,
          duration_ms: 0,
          error: { message: "服务器未返回该会话结果" },
        }
      );
    }
    const failed = outcomes.filter((outcome) => !outcome.ok).length;
    setMessage(
      failed ? `${failed} 个会话执行失败` : "所有会话执行完成",
      failed ? "error" : "success"
    );
  } catch (error) {
    setMessage(error.message, "error");
    sessions.forEach((session) => {
      if (!session.nodes) return;
      session.nodes.status.textContent = "失败";
      session.nodes.status.dataset.state = "error";
    });
  } finally {
    setBusy(false);
  }
}

function init() {
  elements.concurrencyMessage = document.getElementById("concurrencyMessage");
  elements.sessionGrid = document.getElementById("sessionGrid");
  elements.sessionCount = document.getElementById("sessionCount");
  elements.runConcurrentButton = document.getElementById("runConcurrentButton");
  elements.sessionCount.addEventListener("change", renderSessions);
  elements.runConcurrentButton.addEventListener("click", runConcurrent);
  renderSessions();
  if (window.lucide) window.lucide.createIcons();
}

document.addEventListener("DOMContentLoaded", init);
