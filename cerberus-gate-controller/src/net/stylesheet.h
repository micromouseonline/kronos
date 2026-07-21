#pragma once

const char COMMON_STYLE[] PROGMEM = R"rawliteral(
  :root {
    --bg-color: #0f172a;
    --card-bg: #1e293b;
    --text-main: #f8fafc;
    --text-muted: #94a3b8;
    --border-color: #334155;
    --accent-gold: #f59e0b;
    --accent-gold-bg: rgba(245, 158, 11, 0.1);
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background-color: var(--bg-color);
    color: var(--text-main);
    padding: 24px;
    display: flex;
    flex-direction: column;
    align-items: center;
  }

  h1 {
    font-size: 1.5rem;
    font-weight: 800;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    color: var(--text-main);
    margin-bottom: 20px;
  }

  .card {
    background-color: var(--card-bg);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    padding: 20px;
    width: 100%;
    max-width: 480px;
    box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.3);
  }

  /* Leaderboard Table */
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.95rem;
  }

  th {
    text-transform: uppercase;
    font-size: 0.75rem;
    letter-spacing: 0.08em;
    color: var(--text-muted);
    padding: 12px 16px;
    text-align: left;
    border-bottom: 2px solid var(--border-color);
  }

  td {
    padding: 14px 16px;
    border-bottom: 1px solid var(--border-color);
  }

  tr:last-child td {
    border-bottom: none;
  }

  /* Column Alignments */
  th:first-child, td:first-child { width: 48px; text-align: center; font-weight: 700; }
  th:last-child, td:last-child { text-align: right; font-family: monospace; font-size: 1.05rem; font-weight: 600; }

  /* First Place Highlight */
  tr.leader {
    background-color: var(--accent-gold-bg);
  }
  tr.leader td:first-child,
  tr.leader td:last-child {
    color: var(--accent-gold);
    font-weight: 800;
  }

  /* Clock Specific */
  #clock {
    font-size: 1.8rem;
    font-weight: 700;
    font-family: monospace;
    letter-spacing: -0.02em;
    color: #38bdf8;
    margin: 8px 0;
  }

  .small {
    color: var(--text-muted);
    font-size: 0.85rem;
  }
)rawliteral";