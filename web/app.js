"use strict";

const state = {
  busy: false,
  navigatorTables: [],
  expandedNodes: new Set(["connection", "database", "tables"]),
  selectedNode: "",
  selectedTable: "",
  tableDetailView: "data",
  tableDetailRequestId: 0,
  traceMode: false,
  databaseName: "当前数据库",
};

const elements = {};
const SIDEBAR_MIN_WIDTH = 220;
const SIDEBAR_MAX_WIDTH = 520;
const SIDEBAR_DEFAULT_WIDTH = 270;
const SIDEBAR_WIDTH_KEY = "oursql.sidebar.width";
const EDITOR_MIN_HEIGHT = 160;
const EDITOR_DEFAULT_HEIGHT = 268;
const EDITOR_MAX_HEIGHT = 720;
const EDITOR_HEIGHT_KEY = "oursql.editor.height";

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
  elements.traceButton.disabled = busy;
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

function editorMaxHeight() {
  return Math.max(EDITOR_MIN_HEIGHT, Math.min(EDITOR_MAX_HEIGHT, window.innerHeight - 180));
}

function setEditorHeight(height, persist = true) {
  const next = Math.max(
    EDITOR_MIN_HEIGHT,
    Math.min(editorMaxHeight(), Math.round(height))
  );
  document.documentElement.style.setProperty("--editor-height", `${next}px`);
  elements.editorResizer.setAttribute("aria-valuenow", String(next));
  if (persist) {
    try {
      localStorage.setItem(EDITOR_HEIGHT_KEY, String(next));
    } catch {
      // The resize remains usable when browser storage is unavailable.
    }
  }
}

function restoreEditorHeight() {
  let height = EDITOR_DEFAULT_HEIGHT;
  try {
    const stored = Number(localStorage.getItem(EDITOR_HEIGHT_KEY));
    if (Number.isFinite(stored) && stored > 0) height = stored;
  } catch {
    // Use the default height.
  }
  setEditorHeight(height, false);
}

function initializeEditorResize() {
  const resizer = elements.editorResizer;
  let dragging = false;
  let startY = 0;
  let startHeight = 0;

  resizer.addEventListener("pointerdown", (event) => {
    dragging = true;
    startY = event.clientY;
    startHeight = elements.editorShell.getBoundingClientRect().height;
    resizer.setPointerCapture(event.pointerId);
    document.body.classList.add("is-resizing-editor");
    event.preventDefault();
  });
  resizer.addEventListener("pointermove", (event) => {
    if (!dragging) return;
    setEditorHeight(startHeight + event.clientY - startY);
  });
  const stopDragging = (event) => {
    if (!dragging) return;
    dragging = false;
    if (resizer.hasPointerCapture(event.pointerId)) {
      resizer.releasePointerCapture(event.pointerId);
    }
    document.body.classList.remove("is-resizing-editor");
  };
  resizer.addEventListener("pointerup", stopDragging);
  resizer.addEventListener("pointercancel", stopDragging);
  resizer.addEventListener("dblclick", () => setEditorHeight(EDITOR_DEFAULT_HEIGHT));
  resizer.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowUp" && event.key !== "ArrowDown") return;
    event.preventDefault();
    const delta = event.key === "ArrowUp" ? -16 : 16;
    const current = Number(resizer.getAttribute("aria-valuenow")) || EDITOR_DEFAULT_HEIGHT;
    setEditorHeight(current + delta);
  });
  window.addEventListener("resize", () => {
    const current = Number(resizer.getAttribute("aria-valuenow")) || EDITOR_DEFAULT_HEIGHT;
    setEditorHeight(current, false);
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
    if (state.selectedNode === options.id && options.onClose) {
      state.selectedNode = "";
      options.onClose();
      return;
    }
    state.selectedNode = options.id;
    if (options.expandable) {
      if (state.expandedNodes.has(options.id)) state.expandedNodes.delete(options.id);
      else state.expandedNodes.add(options.id);
    }
    renderNavigatorTree();
    if (options.onSelect) void options.onSelect();
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
      onSelect: () => openTableDetails(table),
      onClose: closeTableDetails,
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
  if (state.selectedTable &&
      !state.navigatorTables.some((table) => table.name === state.selectedTable)) {
    closeTableDetails(false);
  }
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
  if (!result.columns || result.columns.length === 0) {
    return element("div", "execution-line", "执行成功");
  }

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
  }

  return block;
}

function renderResults(results) {
  elements.resultsArea.replaceChildren();
  if (!results || !results.length) {
    elements.resultsArea.append(element("div", "empty-state", "执行成功"));
    return;
  }
  for (let index = 0; index < results.length; ++index) {
    const result = results[index];
    const line = Number.isInteger(result.line) && result.line > 0 ? result.line : index + 1;
    const row = element("div", "result-row");
    const lineNumber = element("div", "result-line-number", String(line));
    lineNumber.title = `对应 SQL 第 ${line} 行`;
    row.append(lineNumber, renderResult(result));
    elements.resultsArea.append(row);
  }
}

function renderTrace(trace) {
  elements.resultsArea.replaceChildren();
  const steps = trace?.steps || [];
  if (!steps.length) {
    elements.resultsArea.append(element("div", "empty-state", "链路没有返回阶段结果"));
    return;
  }
  for (let index = 0; index < steps.length; ++index) {
    const step = steps[index];
    const section = element("section", "trace-step");
    section.dataset.ok = String(Boolean(step.ok));
    const header = element("div", "trace-step-header");
    header.append(
      element("span", "trace-step-index", String(index + 1)),
      element("span", "trace-step-name", step.name || `阶段 ${index + 1}`),
      element("span", "trace-step-status", step.ok ? "通过" : "失败")
    );
    section.append(header);
    if (step.summary) section.append(element("div", "trace-step-summary", step.summary));
    section.append(
      element("pre", "trace-content", (step.entries || []).join("\n") || "无输出")
    );
    elements.resultsArea.append(section);
  }
}

function updateTraceButton() {
  elements.traceButton.setAttribute("aria-pressed", String(state.traceMode));
  elements.traceButton.classList.toggle("is-active", state.traceMode);
}

function formatColumnType(column) {
  return `${column.type || ""}${column.length !== null && column.length !== undefined
    ? `(${column.length})`
    : ""}`;
}

function renderTableUml(table) {
  const diagram = element("div", "uml-diagram");
  const node = element("section", "uml-node");
  const title = element("div", "uml-node-title");
  title.append(treeIcon("table-2"), element("span", "", table.name));
  node.append(title);

  const members = element("div", "uml-members");
  for (const column of table.columns || []) {
    const member = element("div", "uml-member");
    const identity = element("div", "uml-member-identity");
    identity.append(element("span", "uml-member-name", column.name));
    if (column.primary_key) identity.append(element("span", "uml-badge", "PK"));
    if (column.unique && !column.primary_key) identity.append(element("span", "uml-badge", "UQ"));

    const details = element("div", "uml-member-meta");
    details.append(element("span", "", formatColumnType(column)));
    details.append(element("span", "", column.nullable ? "NULL" : "NOT NULL"));
    member.append(identity, details);
    members.append(member);
  }
  if (!members.childElementCount) {
    members.append(element("div", "empty-state", "表中没有字段"));
  }
  node.append(members);
  diagram.append(node);
  return diagram;
}

function setTableDetailTab(view) {
  state.tableDetailView = view === "uml" ? "uml" : "data";
  const showData = state.tableDetailView === "data";
  elements.tableDataTab.setAttribute("aria-selected", String(showData));
  elements.tableUmlTab.setAttribute("aria-selected", String(!showData));
  elements.tableDataPanel.hidden = !showData;
  elements.tableUmlPanel.hidden = showData;
}

function quoteTableIdentifier(name) {
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    throw new Error("表名包含当前工作台不支持的字符");
  }
  return name;
}

async function loadTableData(table) {
  const requestId = ++state.tableDetailRequestId;
  elements.refreshTableButton.disabled = true;
  elements.tableDataPanel.replaceChildren(element("div", "empty-state", "正在加载表数据"));
  try {
    const data = await request("/api/query", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sql: `SELECT * FROM ${quoteTableIdentifier(table.name)};` }),
    });
    if (requestId !== state.tableDetailRequestId || state.selectedTable !== table.name) return;
    const result = (data.results || []).find(
      (item) => Array.isArray(item.columns) && item.columns.length > 0
    );
    if (!result) throw new Error("表数据查询没有返回结果");
    elements.tableDataPanel.replaceChildren(renderResult(result));
  } catch (error) {
    if (requestId !== state.tableDetailRequestId) return;
    const message = element("div", "message", error.message);
    message.dataset.kind = "error";
    elements.tableDataPanel.replaceChildren(message);
  } finally {
    if (requestId === state.tableDetailRequestId) {
      elements.refreshTableButton.disabled = false;
    }
  }
}

async function openTableDetails(table) {
  state.selectedTable = table.name;
  elements.tableDetailTitle.textContent = table.name;
  elements.tableDetailSection.hidden = false;
  elements.tableUmlPanel.replaceChildren(renderTableUml(table));
  if (window.lucide) window.lucide.createIcons();
  setTableDetailTab("data");
  await loadTableData(table);
}

function closeTableDetails(clearSelection = true) {
  ++state.tableDetailRequestId;
  state.selectedTable = "";
  elements.tableDetailSection.hidden = true;
  elements.tableDataPanel.replaceChildren();
  elements.tableUmlPanel.replaceChildren();
  if (clearSelection) {
    state.selectedNode = "";
    renderNavigatorTree();
  }
}

async function refreshTableDetails() {
  const table = state.navigatorTables.find((item) => item.name === state.selectedTable);
  if (!table) {
    closeTableDetails();
    return;
  }
  elements.tableUmlPanel.replaceChildren(renderTableUml(table));
  if (window.lucide) window.lucide.createIcons();
  await loadTableData(table);
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
    if (state.traceMode) {
      const trace = await request("/api/trace", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ sql }),
      });
      const elapsed = performance.now() - startedAt;
      renderTrace(trace);
      elements.resultSummary.textContent = `${trace.steps?.length || 0} 个链路阶段`;
      setMessage(`链路展示完成，用时 ${elapsed.toFixed(1)} ms`, "success");
      elements.queryMeta.textContent = `最近链路展示 ${elapsed.toFixed(1)} ms`;
      return;
    }

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
    const selectedTable = state.navigatorTables.find(
      (table) => table.name === state.selectedTable
    );
    if (selectedTable) await refreshTableDetails();
    await loadStats();
  } catch (error) {
    elements.resultsArea.replaceChildren(element("div", "empty-state", "执行失败"));
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

async function loadSampleSql() {
  if (state.busy) return;
  try {
    const response = await fetch("/assets/frontend_all_runnable.sql");
    if (!response.ok) throw new Error(`载入失败 (${response.status})`);
    elements.sqlEditor.value = await response.text();
    updateLineNumbers();
    elements.queryMeta.textContent = "已载入全部可运行 SQL";
    setMessage("");
  } catch (error) {
    setMessage(error.message, "error");
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
  elements.editorShell = document.querySelector(".editor-shell");
  elements.editorResizer = document.getElementById("editorResizer");
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
  elements.loadSampleButton = document.getElementById("loadSampleButton");
  elements.traceButton = document.getElementById("traceButton");
  elements.tableDetailSection = document.getElementById("tableDetailSection");
  elements.tableDetailTitle = document.getElementById("tableDetailTitle");
  elements.tableDataTab = document.getElementById("tableDataTab");
  elements.tableUmlTab = document.getElementById("tableUmlTab");
  elements.tableDataPanel = document.getElementById("tableDataPanel");
  elements.tableUmlPanel = document.getElementById("tableUmlPanel");
  elements.refreshTableButton = document.getElementById("refreshTableButton");
  elements.closeTableButton = document.getElementById("closeTableButton");
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
  elements.loadSampleButton.addEventListener("click", loadSampleSql);
  elements.traceButton.addEventListener("click", () => {
    state.traceMode = !state.traceMode;
    updateTraceButton();
    setMessage(state.traceMode ? "链路展示已开启" : "");
  });
  elements.tableDataTab.addEventListener("click", () => setTableDetailTab("data"));
  elements.tableUmlTab.addEventListener("click", () => setTableDetailTab("uml"));
  elements.refreshTableButton.addEventListener("click", refreshTableDetails);
  elements.closeTableButton.addEventListener("click", () => closeTableDetails());
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
  restoreEditorHeight();
  initializeEditorResize();
  updateLineNumbers();
  if (window.lucide) window.lucide.createIcons();
  await refreshWorkspace();
}

document.addEventListener("DOMContentLoaded", init);
