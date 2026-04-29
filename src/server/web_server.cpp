#include "server/web_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <variant>

#include "parser/parser.h"

namespace litedb::server {

namespace {

constexpr const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LiteDB Console</title>
  <style>
    :root {
      --ink: #1d211c;
      --muted: #637064;
      --panel: rgba(255, 255, 255, 0.78);
      --line: rgba(29, 33, 28, 0.14);
      --accent: #e86f3a;
      --accent-dark: #9d3d1b;
      --green: #2f8062;
      --blue: #335c8a;
      --danger: #a1271c;
      --shadow: 0 24px 80px rgba(68, 44, 23, 0.18);
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100vh;
      color: var(--ink);
      font-family: Georgia, "Times New Roman", serif;
      background:
        radial-gradient(circle at 15% 10%, rgba(232, 111, 58, 0.24), transparent 32rem),
        radial-gradient(circle at 88% 18%, rgba(47, 128, 98, 0.18), transparent 26rem),
        linear-gradient(135deg, #fff7e5 0%, #f5ead8 48%, #eaf2df 100%);
    }

    button {
      border: 0;
      border-radius: 999px;
      padding: 11px 16px;
      cursor: pointer;
      color: #fff8ec;
      background: linear-gradient(135deg, var(--accent), var(--accent-dark));
      font: 800 0.86rem ui-monospace, SFMono-Regular, Menlo, monospace;
      box-shadow: 0 12px 24px rgba(157, 61, 27, 0.22);
    }

    button.secondary {
      color: var(--ink);
      background: rgba(255, 255, 255, 0.72);
      box-shadow: none;
      border: 1px solid var(--line);
    }

    button.ghost {
      color: var(--muted);
      background: transparent;
      border: 1px solid var(--line);
      box-shadow: none;
    }

    button:disabled {
      cursor: wait;
      opacity: 0.6;
    }

    .shell {
      width: min(1380px, calc(100vw - 32px));
      margin: 0 auto;
      padding: 36px 0;
    }

    header {
      display: grid;
      grid-template-columns: 1fr auto;
      gap: 24px;
      align-items: end;
      margin-bottom: 24px;
    }

    h1 {
      margin: 0;
      font-size: clamp(2.6rem, 7vw, 6.4rem);
      line-height: 0.86;
      letter-spacing: -0.075em;
    }

    .tagline {
      max-width: 680px;
      margin: 18px 0 0;
      color: var(--muted);
      font-size: 1.06rem;
      line-height: 1.55;
    }

    .top-actions {
      display: flex;
      gap: 10px;
      align-items: center;
      flex-wrap: wrap;
      justify-content: flex-end;
    }

    .status {
      border: 1px solid var(--line);
      background: rgba(255, 255, 255, 0.52);
      border-radius: 999px;
      padding: 10px 15px;
      color: var(--green);
      font: 700 0.82rem ui-monospace, SFMono-Regular, Menlo, monospace;
      white-space: nowrap;
    }

    .workspace {
      display: grid;
      grid-template-columns: 320px minmax(0, 1fr);
      gap: 20px;
      align-items: start;
    }

    .workspace.history-hidden {
      grid-template-columns: minmax(0, 1fr);
    }

    .card {
      border: 1px solid var(--line);
      border-radius: 30px;
      background: var(--panel);
      box-shadow: var(--shadow);
      backdrop-filter: blur(18px);
      overflow: hidden;
    }

    .history-card {
      max-height: 760px;
      display: flex;
      flex-direction: column;
    }

    .history-hidden .history-card {
      display: none;
    }

    .history-list {
      padding: 14px;
      display: grid;
      gap: 10px;
      overflow: auto;
    }

    .history-entry {
      border: 1px solid var(--line);
      border-radius: 18px;
      background: rgba(255, 255, 255, 0.58);
      padding: 12px;
      cursor: pointer;
      transition: transform 150ms ease, border-color 150ms ease, background 150ms ease;
    }

    .history-entry:hover {
      transform: translateY(-1px);
      border-color: rgba(232, 111, 58, 0.36);
      background: rgba(255, 255, 255, 0.75);
    }

    .history-entry strong {
      display: block;
      color: var(--ink);
      font: 800 0.78rem/1.35 ui-monospace, SFMono-Regular, Menlo, monospace;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .history-entry span {
      display: block;
      margin-top: 8px;
      color: var(--muted);
      font: 700 0.72rem ui-monospace, SFMono-Regular, Menlo, monospace;
    }

    .history-meta {
      display: flex;
      align-items: center;
      gap: 7px;
      flex-wrap: wrap;
      margin-top: 10px;
    }

    .history-meta span {
      display: inline-flex;
      margin-top: 0;
    }

    .pill {
      display: inline-flex;
      align-items: center;
      border-radius: 999px;
      padding: 5px 8px;
      color: var(--green);
      background: rgba(47, 128, 98, 0.12);
      font: 800 0.68rem ui-monospace, SFMono-Regular, Menlo, monospace;
      text-transform: uppercase;
      letter-spacing: 0.06em;
    }

    .pill.error {
      color: var(--danger);
      background: rgba(161, 39, 28, 0.1);
    }

    .history-entry.error {
      border-color: rgba(161, 39, 28, 0.35);
    }

    .grid {
      display: grid;
      grid-template-columns: minmax(0, 0.92fr) minmax(0, 1.08fr);
      gap: 20px;
    }

    .editor, .result {
      min-height: 690px;
      display: flex;
      flex-direction: column;
    }

    .bar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 14px;
      padding: 18px 20px;
      border-bottom: 1px solid var(--line);
      color: var(--muted);
      font-size: 0.88rem;
    }

    .bar-actions {
      display: flex;
      gap: 8px;
      align-items: center;
      flex-wrap: wrap;
      justify-content: flex-end;
    }

    .tabbar {
      display: flex;
      gap: 8px;
      align-items: center;
      overflow-x: auto;
      padding: 14px 16px;
      border-bottom: 1px solid var(--line);
      background: rgba(255, 255, 255, 0.32);
    }

    .tab {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      max-width: 220px;
      border: 1px solid var(--line);
      border-radius: 999px;
      padding: 8px 10px 8px 13px;
      background: rgba(255, 255, 255, 0.58);
      cursor: pointer;
      font: 800 0.78rem ui-monospace, SFMono-Regular, Menlo, monospace;
    }

    .tab.active {
      color: #fff8ec;
      background: linear-gradient(135deg, var(--green), #214f3e);
    }

    .tab-label {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .tab-close {
      border: 0;
      padding: 0;
      width: 20px;
      height: 20px;
      line-height: 18px;
      color: inherit;
      background: rgba(255, 255, 255, 0.22);
      box-shadow: none;
    }

    .add-tab {
      min-width: 36px;
      padding: 8px 12px;
    }

    textarea {
      flex: 1;
      width: 100%;
      min-height: 390px;
      resize: vertical;
      border: 0;
      outline: 0;
      padding: 22px;
      color: #172018;
      background: transparent;
      font: 600 1rem/1.6 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    }

    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      padding: 18px 20px 20px;
      border-top: 1px solid var(--line);
    }

    .result-body {
      flex: 1;
      padding: 20px;
      overflow: auto;
    }

    .summary {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 10px;
      margin-bottom: 18px;
    }

    .metric {
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 14px;
      background: rgba(255, 255, 255, 0.56);
    }

    .metric span {
      display: block;
      color: var(--muted);
      font: 800 0.68rem ui-monospace, SFMono-Regular, Menlo, monospace;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    .metric strong {
      display: block;
      margin-top: 7px;
      color: var(--ink);
      font: 800 1.2rem/1.1 ui-monospace, SFMono-Regular, Menlo, monospace;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .result-footer {
      display: none;
      justify-content: flex-end;
      gap: 10px;
      padding: 16px 20px;
      border-top: 1px solid var(--line);
      background: rgba(255, 255, 255, 0.28);
    }

    .result-footer.visible {
      display: flex;
    }

    .message {
      margin: 0 0 16px;
      color: var(--muted);
      font: 700 0.88rem ui-monospace, SFMono-Regular, Menlo, monospace;
    }

    .empty-state {
      display: grid;
      place-items: center;
      min-height: 310px;
      border: 1px dashed rgba(29, 33, 28, 0.2);
      border-radius: 24px;
      background:
        radial-gradient(circle at 50% 0%, rgba(232, 111, 58, 0.14), transparent 18rem),
        rgba(255, 255, 255, 0.34);
      text-align: center;
      padding: 24px;
    }

    .empty-state strong {
      display: block;
      margin-bottom: 8px;
      font-size: 1.25rem;
    }

    .empty-state p {
      max-width: 380px;
      margin: 0;
      color: var(--muted);
      line-height: 1.5;
    }

    .message.error {
      color: var(--danger);
    }

    .error-box {
      border: 1px solid rgba(161, 39, 28, 0.24);
      border-radius: 20px;
      padding: 16px;
      color: var(--danger);
      background: rgba(161, 39, 28, 0.08);
      font: 800 0.9rem/1.55 ui-monospace, SFMono-Regular, Menlo, monospace;
      white-space: pre-wrap;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      overflow: hidden;
      border-radius: 18px;
      background: rgba(255, 255, 255, 0.58);
    }

    th, td {
      padding: 13px 14px;
      border-bottom: 1px solid var(--line);
      text-align: left;
      vertical-align: top;
    }

    th {
      color: #4b563f;
      font: 800 0.78rem ui-monospace, SFMono-Regular, Menlo, monospace;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      background: rgba(234, 242, 223, 0.75);
    }

    td {
      font: 600 0.95rem ui-monospace, SFMono-Regular, Menlo, monospace;
    }

    .examples {
      display: grid;
      gap: 8px;
      padding: 0 20px 20px;
    }

    .example {
      color: var(--accent-dark);
      background: rgba(255, 255, 255, 0.58);
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 10px 12px;
      cursor: pointer;
      font: 700 0.78rem/1.4 ui-monospace, SFMono-Regular, Menlo, monospace;
    }

    @media (max-width: 1080px) {
      .workspace, .workspace.history-hidden, .grid {
        grid-template-columns: 1fr;
      }

      .history-card {
        max-height: 320px;
      }

      .summary {
        grid-template-columns: 1fr;
      }
    }

    @media (max-width: 720px) {
      header { grid-template-columns: 1fr; }
      .top-actions { justify-content: flex-start; }
      .editor, .result { min-height: auto; }
    }
  </style>
</head>
<body>
  <main class="shell">
    <header>
      <div>
        <h1>LiteDB<br>Console</h1>
        <p class="tagline">Run SQL against the embedded C++ engine. Tabs, query history, and exports stay in your browser.</p>
      </div>
      <div class="top-actions">
        <button class="ghost" id="toggle-history">Hide history</button>
        <div class="status" id="status">connected</div>
      </div>
    </header>

    <section class="workspace" id="workspace">
      <aside class="card history-card">
        <div class="bar">
          <strong>Query history</strong>
          <button class="ghost" id="clear-history">Clear</button>
        </div>
        <div class="history-list" id="history-list">
          <p class="message">No queries yet.</p>
        </div>
      </aside>

      <section class="grid">
        <div class="card editor">
          <div class="bar">
            <strong>SQL editor</strong>
            <span>Ctrl/Cmd + Enter to run</span>
          </div>
          <div class="tabbar" id="tabbar"></div>
          <textarea id="sql" spellcheck="false"></textarea>
          <div class="actions">
            <button id="run">Run query</button>
            <button class="secondary" id="run-selection">Run selected</button>
            <button class="secondary" id="clear">Clear results</button>
          </div>
          <div class="examples">
            <div class="example">CREATE TABLE people (id INT, name TEXT)</div>
            <div class="example">INSERT INTO people VALUES (1, 'Ada'), (2, 'Grace')</div>
            <div class="example">SELECT * FROM people WHERE id >= 1</div>
            <div class="example">UPDATE people SET name = 'Ada Lovelace' WHERE id = 1</div>
            <div class="example">DELETE FROM people WHERE id = 2</div>
          </div>
        </div>

        <div class="card result">
          <div class="bar">
            <strong>Result</strong>
            <div class="bar-actions">
              <span id="last-status">waiting</span>
            </div>
          </div>
          <div class="result-body" id="result">
            <div class="empty-state">
              <div>
                <strong>No result yet</strong>
                <p>Run a query to see status metrics, result rows, and export options.</p>
              </div>
            </div>
          </div>
          <div class="result-footer" id="result-footer">
            <button class="secondary" id="export-csv">Export CSV</button>
            <button class="secondary" id="export-json">Export JSON</button>
            <button class="secondary" id="copy-json">Copy JSON</button>
          </div>
        </div>
      </section>
    </section>
  </main>

  <script>
    const TAB_KEY = 'litedb_tabs';
    const ACTIVE_TAB_KEY = 'litedb_active_tab';
    const HISTORY_KEY = 'litedb_history';
    const HISTORY_VISIBLE_KEY = 'litedb_history_visible';

    const sql = document.querySelector('#sql');
    const run = document.querySelector('#run');
    const runSelection = document.querySelector('#run-selection');
    const clear = document.querySelector('#clear');
    const result = document.querySelector('#result');
    const resultFooter = document.querySelector('#result-footer');
    const exportCsv = document.querySelector('#export-csv');
    const exportJson = document.querySelector('#export-json');
    const copyJson = document.querySelector('#copy-json');
    const lastStatus = document.querySelector('#last-status');
    const status = document.querySelector('#status');
    const tabbar = document.querySelector('#tabbar');
    const historyList = document.querySelector('#history-list');
    const clearHistory = document.querySelector('#clear-history');
    const toggleHistory = document.querySelector('#toggle-history');
    const workspace = document.querySelector('#workspace');

    let tabs = loadTabs();
    let activeTabId = localStorage.getItem(ACTIVE_TAB_KEY) || tabs[0].id;
    let history = loadHistory();
    let historyVisible = localStorage.getItem(HISTORY_VISIBLE_KEY) !== 'false';

    function newId() {
      if (crypto.randomUUID) return crypto.randomUUID();
      return `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    }

    function defaultTab() {
      return {
        id: 'tab-1',
        label: 'Query 1',
        sql: 'CREATE TABLE people (id INT, name TEXT)',
        result: null
      };
    }

    function loadTabs() {
      try {
        const saved = JSON.parse(localStorage.getItem(TAB_KEY) || '[]');
        return Array.isArray(saved) && saved.length ? saved : [defaultTab()];
      } catch (_) {
        return [defaultTab()];
      }
    }

    function loadHistory() {
      try {
        const saved = JSON.parse(localStorage.getItem(HISTORY_KEY) || '[]');
        return Array.isArray(saved) ? saved : [];
      } catch (_) {
        return [];
      }
    }

    function saveTabs() {
      localStorage.setItem(TAB_KEY, JSON.stringify(tabs));
      localStorage.setItem(ACTIVE_TAB_KEY, activeTabId);
    }

    function saveHistory() {
      localStorage.setItem(HISTORY_KEY, JSON.stringify(history.slice(0, 100)));
    }

    function activeTab() {
      return tabs.find((tab) => tab.id === activeTabId) || tabs[0];
    }

    function escapeHtml(value) {
      return String(value)
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#039;');
    }

    function resultObjects(data) {
      if (!data || !data.success || !data.columns || !data.rows || !data.rows.length) {
        return [];
      }
      return data.rows.map((row) =>
        Object.fromEntries(data.columns.map((column, index) => [column, row[index] ?? '']))
      );
    }

    function renderTabs() {
      tabbar.innerHTML = tabs.map((tab) => `
        <div class="tab ${tab.id === activeTabId ? 'active' : ''}" data-tab-id="${tab.id}">
          <span class="tab-label" title="Double-click to rename">${escapeHtml(tab.label)}</span>
          <button class="tab-close" title="Close tab" ${tabs.length === 1 ? 'disabled' : ''}>x</button>
        </div>
      `).join('') + '<button class="secondary add-tab" id="add-tab">+</button>';

      tabbar.querySelectorAll('.tab').forEach((node) => {
        const id = node.dataset.tabId;
        node.addEventListener('click', (event) => {
          if (event.target.classList.contains('tab-close')) return;
          activeTabId = id;
          renderApp();
        });
        node.querySelector('.tab-label').addEventListener('dblclick', () => {
          const tab = tabs.find((item) => item.id === id);
          const label = prompt('Rename query tab', tab.label);
          if (label && label.trim()) {
            tab.label = label.trim();
            saveTabs();
            renderTabs();
          }
        });
        node.querySelector('.tab-close').addEventListener('click', () => closeTab(id));
      });

      document.querySelector('#add-tab').addEventListener('click', addTab);
    }

    function addTab() {
      const tab = {
        id: newId(),
        label: `Query ${tabs.length + 1}`,
        sql: '',
        result: null
      };
      tabs.push(tab);
      activeTabId = tab.id;
      renderApp();
    }

    function closeTab(id) {
      if (tabs.length === 1) return;
      const index = tabs.findIndex((tab) => tab.id === id);
      tabs = tabs.filter((tab) => tab.id !== id);
      if (activeTabId === id) {
        activeTabId = tabs[Math.max(0, index - 1)].id;
      }
      renderApp();
    }

    function renderHistory() {
      if (!history.length) {
        historyList.innerHTML = '<p class="message">No queries yet.</p>';
        return;
      }
      historyList.innerHTML = history.map((entry) => `
        <div class="history-entry ${entry.status === 'error' ? 'error' : ''}" data-history-id="${entry.id}">
          <strong>${escapeHtml(entry.sql)}</strong>
          <div class="history-meta">
            <span class="pill ${entry.status === 'error' ? 'error' : ''}">${escapeHtml(entry.status)}</span>
            <span>${escapeHtml(new Date(entry.timestamp).toLocaleString())}</span>
            <span>${entry.rowCount} rows</span>
          </div>
        </div>
      `).join('');
      historyList.querySelectorAll('.history-entry').forEach((node) => {
        node.addEventListener('click', () => {
          const entry = history.find((item) => item.id === node.dataset.historyId);
          if (!entry) return;
          activeTab().sql = entry.sql;
          renderApp();
          sql.focus();
        });
      });
    }

    function renderResult(data) {
      lastStatus.textContent = data?.status || (data?.success ? 'OK' : data ? 'ERROR' : 'waiting');
      resultFooter.classList.toggle('visible', resultObjects(data).length > 0);
      if (!data) {
        result.innerHTML = `
          <div class="empty-state">
            <div>
              <strong>No result yet</strong>
              <p>Run a query to see status metrics, result rows, and export options.</p>
            </div>
          </div>`;
        return;
      }
      if (!data.success) {
        result.innerHTML = `
          <div class="summary">
            <div class="metric"><span>Status</span><strong>Error</strong></div>
            <div class="metric"><span>Rows</span><strong>0</strong></div>
            <div class="metric"><span>Columns</span><strong>0</strong></div>
          </div>
          <div class="error-box">${escapeHtml(data.error)}</div>`;
        return;
      }
      let html = `
        <div class="summary">
          <div class="metric"><span>Status</span><strong>${escapeHtml(data.status || 'OK')}</strong></div>
          <div class="metric"><span>Rows</span><strong>${(data.rows || []).length}</strong></div>
          <div class="metric"><span>Columns</span><strong>${(data.columns || []).length}</strong></div>
        </div>`;
      if (data.columns && data.columns.length) {
        html += '<table><thead><tr>';
        html += data.columns.map((column) => `<th>${escapeHtml(column)}</th>`).join('');
        html += '</tr></thead><tbody>';
        for (const row of data.rows || []) {
          html += '<tr>' + row.map((value) => `<td>${escapeHtml(value)}</td>`).join('') + '</tr>';
        }
        html += '</tbody></table>';
      } else {
        html += '<div class="empty-state"><div><strong>Command completed</strong><p>This statement did not return rows.</p></div></div>';
      }
      result.innerHTML = html;
    }

    function renderApp() {
      if (!tabs.some((tab) => tab.id === activeTabId)) activeTabId = tabs[0].id;
      workspace.classList.toggle('history-hidden', !historyVisible);
      toggleHistory.textContent = historyVisible ? 'Hide history' : 'Show history';
      sql.value = activeTab().sql;
      renderTabs();
      renderHistory();
      renderResult(activeTab().result);
      saveTabs();
      saveHistory();
      localStorage.setItem(HISTORY_VISIBLE_KEY, historyVisible ? 'true' : 'false');
    }

    function saveToHistory(entry) {
      history.unshift(entry);
      history = history.slice(0, 100);
      saveHistory();
      renderHistory();
    }

    function selectedSql() {
      const selected = sql.value.slice(sql.selectionStart, sql.selectionEnd).trim();
      return selected || sql.value;
    }

    async function runQuery(onlySelection = false) {
      const tab = activeTab();
      tab.sql = sql.value;
      const queryText = onlySelection ? selectedSql() : tab.sql;
      run.disabled = true;
      runSelection.disabled = true;
      status.textContent = 'running';
      let data;
      try {
        const response = await fetch('/query', {
          method: 'POST',
          headers: { 'Content-Type': 'text/plain' },
          body: queryText
        });
        data = await response.json();
        status.textContent = 'connected';
      } catch (err) {
        data = { success: false, error: err.message || String(err) };
        status.textContent = 'error';
      } finally {
        run.disabled = false;
        runSelection.disabled = false;
      }
      tab.result = data;
      saveToHistory({
        id: newId(),
        sql: queryText,
        timestamp: new Date().toISOString(),
        rowCount: data.rows ? data.rows.length : 0,
        status: data.success ? 'success' : 'error'
      });
      renderApp();
    }

    function csvCell(value) {
      const text = String(value ?? '');
      if (/[",\n\r]/.test(text)) return `"${text.replaceAll('"', '""')}"`;
      return text;
    }

    function download(name, type, body) {
      const blob = new Blob([body], { type });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = name;
      a.click();
      URL.revokeObjectURL(url);
    }

    function exportActiveCSV() {
      const rows = resultObjects(activeTab().result);
      if (!rows.length) return;
      const headers = Object.keys(rows[0]);
      const csv = [
        headers.map(csvCell).join(','),
        ...rows.map((row) => headers.map((header) => csvCell(row[header])).join(','))
      ].join('\n');
      download(`litedb_export_${Date.now()}.csv`, 'text/csv', csv);
    }

    function exportActiveJSON() {
      const rows = resultObjects(activeTab().result);
      if (!rows.length) return;
      download(
        `litedb_export_${Date.now()}.json`,
        'application/json',
        JSON.stringify(rows, null, 2)
      );
    }

    async function copyActiveJSON() {
      const rows = resultObjects(activeTab().result);
      if (!rows.length) return;
      await navigator.clipboard.writeText(JSON.stringify(rows, null, 2));
      status.textContent = 'copied JSON';
      setTimeout(() => {
        status.textContent = 'connected';
      }, 1300);
    }

    run.addEventListener('click', () => runQuery(false));
    runSelection.addEventListener('click', () => runQuery(true));
    sql.addEventListener('input', () => {
      activeTab().sql = sql.value;
      saveTabs();
    });
    sql.addEventListener('keydown', (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') runQuery(false);
    });
    clear.addEventListener('click', () => {
      activeTab().result = null;
      renderApp();
    });
    exportCsv.addEventListener('click', exportActiveCSV);
    exportJson.addEventListener('click', exportActiveJSON);
    copyJson.addEventListener('click', copyActiveJSON);
    clearHistory.addEventListener('click', () => {
      history = [];
      renderApp();
    });
    toggleHistory.addEventListener('click', () => {
      historyVisible = !historyVisible;
      renderApp();
    });
    document.querySelectorAll('.example').forEach((node) => {
      node.addEventListener('click', () => {
        activeTab().sql = node.textContent;
        renderApp();
        sql.focus();
      });
    });

    renderApp();
  </script>
</body>
</html>)HTML";

bool send_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += ' ';
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string http_response(const std::string& status,
                          const std::string& content_type,
                          const std::string& body) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Cache-Control: no-store\r\n\r\n"
      << body;
  return out.str();
}

std::string request_body(const std::string& request) {
  auto pos = request.find("\r\n\r\n");
  if (pos == std::string::npos) return {};
  return request.substr(pos + 4);
}

}  // namespace

WebServer::WebServer(uint16_t port, executor::Executor& executor)
    : port_(port), executor_(executor) {}

WebServer::~WebServer() {
  stop();
  if (accept_thread_.joinable()) accept_thread_.join();
  for (auto& t : client_threads_) {
    if (t.joinable()) t.join();
  }
}

void WebServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "web socket");
  }

  int opt = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    int saved = errno;
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw std::system_error(saved, std::generic_category(), "web bind");
  }

  if (::listen(listen_fd_, 16) < 0) {
    int saved = errno;
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw std::system_error(saved, std::generic_category(), "web listen");
  }

  running_.store(true);
  accept_thread_ = std::thread(&WebServer::accept_loop, this);
}

void WebServer::stop() {
  running_.store(false);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void WebServer::accept_loop() {
  while (running_.load()) {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = ::accept(listen_fd_,
                             reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0) {
      if (!running_.load()) break;
      continue;
    }
    client_threads_.emplace_back(&WebServer::handle_client, this, client_fd);
  }
}

void WebServer::handle_client(int client_fd) {
  std::string request;
  char buffer[4096];
  while (request.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      ::close(client_fd);
      return;
    }
    request.append(buffer, static_cast<std::size_t>(n));
    if (request.size() > 1024 * 1024) break;
  }

  std::size_t content_length = 0;
  auto header_end = request.find("\r\n\r\n");
  auto content_length_pos = request.find("Content-Length:");
  if (content_length_pos != std::string::npos &&
      content_length_pos < header_end) {
    auto value_start = content_length_pos + std::strlen("Content-Length:");
    auto value_end = request.find("\r\n", value_start);
    content_length = static_cast<std::size_t>(
        std::stoul(request.substr(value_start, value_end - value_start)));
  }

  while (header_end != std::string::npos &&
         request.size() < header_end + 4 + content_length) {
    ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    request.append(buffer, static_cast<std::size_t>(n));
  }

  send_all(client_fd, handle_request(request));
  ::close(client_fd);
}

std::string WebServer::handle_request(const std::string& request) {
  std::istringstream first_line(request);
  std::string method;
  std::string path;
  first_line >> method >> path;

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    return http_response("200 OK", "text/html; charset=utf-8", kIndexHtml);
  }
  if (method == "GET" && path == "/health") {
    return http_response("200 OK", "application/json",
                         R"({"success":true,"status":"OK"})");
  }
  if (method == "POST" && path == "/query") {
    return http_response("200 OK", "application/json",
                         execute_sql(request_body(request)));
  }
  return http_response("404 Not Found", "application/json",
                       R"({"success":false,"error":"not found"})");
}

std::string WebServer::execute_sql(const std::string& sql) {
  try {
    auto stmt = parser::parse_sql(sql);
    if (std::holds_alternative<parser::BeginStmt>(stmt) ||
        std::holds_alternative<parser::CommitStmt>(stmt) ||
        std::holds_alternative<parser::RollbackStmt>(stmt)) {
      return R"({"success":false,"error":"browser queries are auto-commit; transaction control is only available through the PostgreSQL port"})";
    }

    auto result = executor_.execute(stmt);
    std::ostringstream out;
    out << "{\"success\":" << (result.success ? "true" : "false");
    if (!result.success) {
      out << ",\"error\":\"" << json_escape(result.error) << "\"}";
      return out.str();
    }

    out << ",\"status\":\"" << json_escape(result.status_message) << "\"";
    out << ",\"columns\":[";
    for (std::size_t i = 0; i < result.column_names.size(); ++i) {
      if (i > 0) out << ',';
      out << '"' << json_escape(result.column_names[i]) << '"';
    }
    out << "],\"rows\":[";
    for (std::size_t r = 0; r < result.rows.size(); ++r) {
      if (r > 0) out << ',';
      out << '[';
      for (std::size_t c = 0; c < result.rows[r].size(); ++c) {
        if (c > 0) out << ',';
        const auto& value = result.rows[r][c];
        if (std::holds_alternative<int64_t>(value)) {
          out << '"' << std::get<int64_t>(value) << '"';
        } else {
          out << '"' << json_escape(std::get<std::string>(value)) << '"';
        }
      }
      out << ']';
    }
    out << "]}";
    return out.str();
  } catch (const std::exception& e) {
    return std::string(R"({"success":false,"error":")") + json_escape(e.what()) +
           R"("})";
  }
}

}  // namespace litedb::server
