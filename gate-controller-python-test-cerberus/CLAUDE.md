# gate-controller-python-test-cerberus

Python HTTP test server for the timing gate system (codename: Cerberus / Kronos).

## Platform

- Python 3, standard library only (no dependencies to install)
- Run: `python server.py`
- Default port: 8000

## Architecture

Four concurrent threads:

1. **HTTP server** - receives GET requests from gates
2. **System health monitor** - watchdog; marks gates OFFLINE after `HEARTBEAT_TIMEOUT_SEC` silence
3. **Window processor** - matches paired TRIGGER events within `MATCH_THRESHOLD_US`
4. **Async disk writer** - sole thread that writes to disk; consumes from `log_queue`

## Key parameters (top of `server.py`)

| Constant | Default | Meaning |
|----------|---------|---------|
| `MATCH_THRESHOLD_US` | 200 000 | Max gap between paired triggers to count as a match |
| `WINDOW_PROCESS_INTERVAL_SEC` | 0.5 | How often the matcher runs |
| `HEARTBEAT_TIMEOUT_SEC` | 15.0 | Silence before a gate is flagged OFFLINE |

## Outputs

- Console: timestamped log lines
- `forensics.log`: append-only log written asynchronously
- Additional forensics logs named by date/trial are preserved in this folder

## Off-limits files

- `*.log`, `*.txt` files in this folder are captured test data - do not delete or overwrite
