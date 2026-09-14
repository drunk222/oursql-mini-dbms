"use strict";

const state = {
  busy: false,
  navigatorTables: [],
  expandedNodes: new Set(["connection", "database", "tables"]),
  selectedNode: "",
  databaseName: "当前数据库",
};

const elements = {};
const SIDEBAR_MIN_WIDTH = 220;
const SIDEBAR_MAX_WIDTH = 520;
const SIDEBAR_DEFAULT_WIDTH = 270;
const SIDEBAR_WIDTH_KEY = "oursql.sidebar.width";

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setConnection(stateName, text) {
  elements.connectionStatus.dataset.state = stateName;
  elements.connectionText.textContent = text;
}

function setMessage(text, kind = "") {
  elements.messageArea.replaceChildren();
  if (!text) return;
  const message = element("div", "message", text);
  if (kind) message.dataset.kind = kind;
  elements.messageArea.append(message);
}

function setBusy(busy) {
  state.busy = busy;
  elements.runButton.disabled = busy;
  elements.refreshButton.disabled = busy;
  elements.flushButton.disabled = busy;
  document.body.setAttribute("aria-busy", busy ? "true" : "false");
}

function setSidebarWidth(width, persist = true) {
  const next = Math.max(SIDEBAR_MIN_WIDTH, Math.min(SIDEBAR_MAX_WIDTH, Math.round(width)));
  document.documentElement.style.setProperty("--sidebar-width", `${next}px`);
  elements.sidebarResizer.setAttribute("aria-valuenow", String(next));
  if (persist) {
    try {
      localStorage.setItem(SIDEBAR_WIDTH_KEY, String(next));
    } catch {
      // The resize remains usable when browser storage is unavailable.
    }
  }
}

function restoreSidebarWidth() {
  let width = SIDEBAR_DEFAULT_WIDTH;
  try {
    const stored = Number(localStorage.getItem(SIDEBAR_WIDTH_KEY));
    if (Number.isFinite(stored) && stored > 0) width = stored;
  } catch {
    // Use the default width.
  }
  setSidebarWidth(width, false);
}

function initializeSidebarResize() {
  const resizer = elements.sidebarResizer;
  let dragging = false;

  const resizeFromPointer = (event) => {
    const workspaceLeft = elements.workspace.getBoundingClientRect().left;
    setSidebarWidth(event.clientX - workspaceLeft);
  };

  resizer.addEventListener("pointerdown", (event) => {
    if (window.matchMedia("(max-width: 900px)").matches) return;
    dragging = true;
    resizer.setPointerCapture(event.pointerId);
    document.body.classList.add("is-resizing-sidebar");
    event.preventDefault();
  });
  resizer.addEventListener("pointermove", (event) => {
    if (dragging) resizeFromPointer(event);
  });
  const stopDragging = (event) => {
    if (!dragging) return;
    dragging = false;
    if (resizer.hasPointerCapture(event.pointerId)) {
      resizer.releasePointerCapture(event.pointerId);
    }
    document.body.classList.remove("is-resizing-sidebar");
  };
  resizer.addEventListener("pointerup", stopDragging);
  resizer.addEventListener("pointercancel", stopDragging);
  resizer.addEventListener("dblclick", () => setSidebarWidth(SIDEBAR_DEFAULT_WIDTH));
  resizer.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") return;
    event.preventDefault();
    const delta = event.key === "ArrowLeft" ? -16 : 16;
    const current = Number(resizer.getAttribute("aria-valuenow")) || SIDEBAR_DEFAULT_WIDTH;
    setSidebarWidth(current + delta);
  });
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

async function loadHealth() {
  try {
    const data = await request("/api/health");
    state.databaseName = data.database || "当前数据库";
    setConnection("online", "已连接");
  } catch (error) {
    setConnection("offline", "未连接");
    throw error;
  }
}

function treeIcon(name) {
  const icon = document.createElement("i");
  icon.dataset.lucide = name;
  icon.setAttribute("aria-hidden", "true");
  return icon;
}

function makeTreeNode(options) {
  const node = element("div", "tree-node");
  node.dataset.nodeId = options.id;
  const row = element("button", "tree-row");
  row.type = "button";
  row.style.setProperty("--tree-level", String(options.level));
  row.setAttribute("role", "treeitem");
  row.setAttribute("aria-level", String(options.level + 1));
  const expanded = state.expandedNodes.has(options.id);
  if (options.expandable) row.setAttribute("aria-expanded", String(expanded));
  if (state.selectedNode === options.id) row.setAttribute("aria-selected", "true");

  const chevron = element("span", "tree-chevron");
  if (options.expandable) chevron.append(treeIcon(expanded ? "chevron-down" : "chevron-right"));
  row.append(chevron, treeIcon(options.icon), element("span", "tree-label", options.label));
  if (options.badge !== undefined) {
    row.append(element("span", "tree-badge", String(options.badge)));
  }

  row.addEventListener("click", () => {
    state.selectedNode = options.id;
    if (options.expandable) {
      if (state.expandedNodes.has(options.id)) state.expandedNodes.delete(options.id);
      else state.expandedNodes.add(options.id);
    }
    renderNavigatorTree();
  });
  row.addEventListener("keydown", (event) => {
    if (event.key === "ArrowRight" && options.expandable) {
      state.expandedNodes.add(options.id);
      renderNavigatorTree();
    } else if (event.key === "ArrowLeft" && options.expandable) {
      state.expandedNodes.delete(options.id);
      renderNavigatorTree();
    } else if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      row.click();
    }
  });
  node.append(row);

  if (options.expandable && expanded && options.children.length) {
    const children = element("div", "tree-children");
    children.setAttribute("role", "group");
    for (const child of options.children) children.append(makeTreeNode(child));
    node.append(children);
  }
  return node;
}

function renderNavigatorTree() {
  const filter = elements.navigatorFilter.value.trim().toLowerCase();
  const tables = state.navigatorTables.filter(
    (table) => !filter || table.name.toLowerCase().includes(filter)
  );
  const tableNodes = tables.map((table) => {
    const columnNodes = (table.columns || []).map((column) => ({
      id: `column:${table.name}:${column.name}`,
      label: `${column.name}  ${column.type}${column.length !== null ? `(${column.length})` : ""}`,
      icon: "columns-3",
      level: 4,
      expandable: false,
      children: [],
    }));
    return {
      id: `table:${table.name}`,
      label: table.name,
      icon: "table-2",
      level: 3,
      expandable: columnNodes.length > 0,
      badge: columnNodes.length,
      children: columnNodes.map((node) => ({ ...node, level: 4 })),
    };
  });
  if (filter && tables.length === 0) {
    tableNodes.push({
      id: "filter-empty",
      label: "无匹配表",
      icon: "search-x",
      level: 3,
      expandable: false,
      children: [],
    });
  }

  const root = {
    id: "connection",
    label: "OurSQL · 127.0.0.1",
    icon: "network",
    level: 0,
    expandable: true,
    children: [
      {
        id: "database",
        label: state.databaseName,
        icon: "database",
        level: 1,
        expandable: true,
        children: [
          {
            id: "tables",
            label: "表",
            icon: "folder",
            level: 2,
            badge: tables.length,
            expandable: true,
            children: tableNodes,
          },
        ],
      },
    ],
  };

  elements.databaseTree.replaceChildren(makeTreeNode(root));
  if (window.lucide) window.lucide.createIcons();
}

async function loadNavigator() {
  const data = await request("/api/tables");
  state.navigatorTables = data.tables || [];
  renderNavigatorTree();
}

function updateLineNumbers() {
  const lineCount = elements.sqlEditor.value.split(/\r\n|\r|\n/).length;
  const fragment = document.createDocumentFragment();
  for (let line = 1; line <= lineCount; ++line) {
    fragment.append(element("span", "line-number", String(line)));
  }
  elements.lineNumbers.replaceChildren(fragment);
  elements.lineNumbers.scrollTop = elements.sqlEditor.scrollTop;
}

function renderStats(data) {
  const statistics = data.statistics || {};
  elements.statHits.textContent = String(statistics.buffer_hits ?? "-");
  elements.statMisses.textContent = String(statistics.buffer_misses ?? "-");
  elements.statHitRate.textContent = `${((statistics.buffer_hit_rate ?? 0) * 100).toFixed(1)}%`;
  elements.statEvictions.textContent = String(statistics.evictions ?? "-");
  elements.statReads.textContent = String(statistics.disk_reads ?? "-");
  elements.statWrites.textContent = String(statistics.disk_writes ?? "-");
}

async function loadStats() {
  const data = await request("/api/stats");
  renderStats(data);
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

function renderResult(result) {
  const block = element("section", "result-block");
  const header = element("div", "result-block-header");
  const rowCount = result.rows ? result.rows.length : 0;
  header.append(
    element("span", "", result.columns?.length ? `${result.columns.length} 列` : "执行结果"),
    element("span", "", `${rowCount} 行`)
  );
  block.append(header);

  if (result.columns && result.columns.length) {
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

    if (rowCount === 0) {
      block.append(element("div", "empty-state", "查询成功，没有返回数据"));
    }
  } else {
    block.append(element("div", "empty-state", "执行成功"));
  }

  return block;
}

function renderResults(results) {
  elements.resultsArea.replaceChildren();
  if (!results || !results.length) {
    elements.resultsArea.append(element("div", "empty-state", "执行成功"));
    return;
  }
  for (const result of results) elements.resultsArea.append(renderResult(result));
}

async function runSql() {
  if (state.busy) return;
  const sql = elements.sqlEditor.value.trim();
  if (!sql) {
    setMessage("请输入 SQL", "error");
    elements.sqlEditor.focus();
    return;
  }

  setBusy(true);
  setMessage("正在执行");
  elements.resultSummary.textContent = "执行中";
  const startedAt = performance.now();

  try {
    const data = await request("/api/query", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sql }),
    });
    const elapsed = performance.now() - startedAt;
    const results = data.results || [];
    renderResults(results);
    const rowCount = results.reduce((count, result) => count + (result.rows?.length || 0), 0);
    elements.resultSummary.textContent = `${results.length} 条结果，${rowCount} 行`;
    setMessage(`执行完成，用时 ${elapsed.toFixed(1)} ms`, "success");
    elements.queryMeta.textContent = `最近执行 ${elapsed.toFixed(1)} ms`;
    await loadNavigator();
    await loadStats();
  } catch (error) {
    renderResults([]);
    elements.resultSummary.textContent = "执行失败";
    setMessage(error.message, "error");
    setConnection("offline", "请求失败");
  } finally {
    setBusy(false);
  }
}

async function flushDatabase() {
  if (state.busy) return;
  setBusy(true);
  setMessage("正在刷盘");
  try {
    await request("/api/flush", { method: "POST" });
    setMessage("缓存已写入磁盘", "success");
    await loadStats();
  } catch (error) {
    setMessage(error.message, "error");
  } finally {
    setBusy(false);
  }
}

async function refreshWorkspace() {
  if (state.busy) return;
  setBusy(true);
  setMessage("正在刷新");
  try {
    await loadHealth();
    await loadNavigator();
    await loadStats();
    setMessage("工作区已刷新", "success");
  } catch (error) {
    setMessage(error.message, "error");
  } finally {
    setBusy(false);
  }
}

function bindElements() {
  elements.connectionStatus = document.getElementById("connectionStatus");
  elements.connectionText = document.getElementById("connectionText");
  elements.workspace = document.querySelector(".workspace");
  elements.sidebarResizer = document.getElementById("sidebarResizer");
  elements.refreshButton = document.getElementById("refreshButton");
  elements.flushButton = document.getElementById("flushButton");
  elements.navigatorFilter = document.getElementById("navigatorFilter");
  elements.databaseTree = document.getElementById("databaseTree");
  elements.lineNumbers = document.getElementById("lineNumbers");
  elements.statHits = document.getElementById("statHits");
  elements.statMisses = document.getElementById("statMisses");
  elements.statHitRate = document.getElementById("statHitRate");
  elements.statEvictions = document.getElementById("statEvictions");
  elements.statReads = document.getElementById("statReads");
  elements.statWrites = document.getElementById("statWrites");
  elements.queryMeta = document.getElementById("queryMeta");
  elements.sqlEditor = document.getElementById("sqlEditor");
  elements.runButton = document.getElementById("runButton");
  elements.clearButton = document.getElementById("clearButton");
  elements.resultSummary = document.getElementById("resultSummary");
  elements.messageArea = document.getElementById("messageArea");
  elements.resultsArea = document.getElementById("resultsArea");
}

function bindEvents() {
  elements.runButton.addEventListener("click", runSql);
  elements.clearButton.addEventListener("click", () => {
    elements.sqlEditor.value = "";
    updateLineNumbers();
    elements.sqlEditor.focus();
    setMessage("");
  });
  elements.refreshButton.addEventListener("click", refreshWorkspace);
  elements.flushButton.addEventListener("click", flushDatabase);
  elements.navigatorFilter.addEventListener("input", renderNavigatorTree);
  elements.sqlEditor.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
      event.preventDefault();
      runSql();
    }
  });
  elements.sqlEditor.addEventListener("input", updateLineNumbers);
  elements.sqlEditor.addEventListener("scroll", () => {
    elements.lineNumbers.scrollTop = elements.sqlEditor.scrollTop;
  });
}

async function init() {
  bindElements();
  bindEvents();
  restoreSidebarWidth();
  initializeSidebarResize();
  updateLineNumbers();
  if (window.lucide) window.lucide.createIcons();
  await refreshWorkspace();
}

document.addEventListener("DOMContentLoaded", init);
