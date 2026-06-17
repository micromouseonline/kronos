#!/usr/bin/env python3
"""
================================================================================
          KRONOS HIGH-THROUGHPUT NON-BLOCKING ASYNC TEST SERVER
================================================================================
"""

import http.server
import urllib.parse
import datetime
import threading
import queue
import time

PORT = 8000
MATCH_THRESHOLD_US = 200000 
WINDOW_PROCESS_INTERVAL_SEC = 0.5
HEARTBEAT_TIMEOUT_SEC = 15.0

# --- Thread-Safe Memory Architecture ---
ledger_lock = threading.Lock()
event_ledger = []
last_event_tsf = {}  # {(gate_id, packet_type): tsf_value} — deduplication for retried events
system_registry = {
    'GATE_01': {'last_seen': datetime.datetime.now(), 'status': 'ONLINE', 'clock_mode': 'UNKNOWN', 'bssid': 'UNKNOWN'},
    'GATE_02': {'last_seen': datetime.datetime.now(), 'status': 'ONLINE', 'clock_mode': 'UNKNOWN', 'bssid': 'UNKNOWN'}
}

# New Non-Blocking Async Logging Queue
log_queue = queue.Queue()
log_file_path = "forensics.log"

def append_to_log(text):
    """Drops log text into a RAM queue instantly, avoiding Disk I/O blocking."""
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
    log_line = f"[ {timestamp} ] {text}\n"
    print(log_line, end="")
    log_queue.put(log_line) # Rapid RAM push

# --- THREAD 4: DEDICATED ASYNC DISK WRITER ---
def disk_writer_worker():
    """The only thread allowed to touch the hard drive. Runs in the background."""
    with open(log_file_path, "a", encoding="utf-8", buffering=1) as f:
        while True:
            log_line = log_queue.get() # Blocks until data is available in RAM
            f.write(log_line)
            log_queue.task_done()

# --- THREAD 1: ACTIVE WATCHDOG FAULT MONITOR ---
def system_health_monitor():
    while True:
        time.sleep(1.0)
        now = datetime.datetime.now()
        pending_logs = []
        with ledger_lock:
            for gate_id, registry in system_registry.items():
                if registry['status'] == 'ONLINE':
                    silence_duration = (now - registry['last_seen']).total_seconds()
                    if silence_duration > HEARTBEAT_TIMEOUT_SEC:
                        registry['status'] = 'OFFLINE'
                        pending_logs.append(
                            f"!!! CRITICAL FAULT ALERT !!! Node [{gate_id}] offline for {silence_duration:.1f}s. "
                            f"Baseline: Mode={registry['clock_mode']} | BSSID={registry['bssid']}"
                        )
        for msg in pending_logs:
            append_to_log(msg)

# --- THREAD 2: RETROSPECTIVE PAIRING ENGINE ---
def process_ledger_window():
    global event_ledger
    while True:
        time.sleep(WINDOW_PROCESS_INTERVAL_SEC)
        pending_logs = []
        with ledger_lock:
            now = datetime.datetime.now()
            if len(event_ledger) < 2:
                retained = []
                for ev in event_ledger:
                    if (now - ev['arrival_time']).total_seconds() > 3.0:
                        pending_logs.append(f"UNMATCHED  EVENT (TIMEOUT) Triggered By: {ev['id']} (Mode: {ev['mode']}) | TSF: {ev['tsf']}")
                    else:
                        retained.append(ev)
                event_ledger = retained
            else:
                event_ledger.sort(key=lambda x: x['tsf'])
                retained_events = []
                matched_indices = set()

                for i in range(len(event_ledger)):
                    if i in matched_indices:
                        continue
                    current_ev = event_ledger[i]
                    match_found = False

                    for j in range(i + 1, len(event_ledger)):
                        if j in matched_indices:
                            continue
                        next_ev = event_ledger[j]
                        tsf_gap = abs(current_ev['tsf'] - next_ev['tsf'])

                        if tsf_gap <= MATCH_THRESHOLD_US:
                            if current_ev['id'] != next_ev['id']:
                                gap_ms = tsf_gap / 1000.0
                                status_signature = f"[{current_ev['id']}:{current_ev['mode']}, {next_ev['id']}:{next_ev['mode']}]"
                                pending_logs.append(f"SUCCESSFUL MATCH {status_signature}: TSF Gap: {tsf_gap:>3} µs ({gap_ms:.4f} ms)")
                                matched_indices.add(i)
                                matched_indices.add(j)
                                match_found = True
                                break
                        else:
                            break

                    if not match_found:
                        if (now - current_ev['arrival_time']).total_seconds() > 3.0:
                            pending_logs.append(f"UNMATCHED  EVENT Triggered By: {current_ev['id']} (Mode: {current_ev['mode']}) | TSF: {current_ev['tsf']}")
                            matched_indices.add(i)
                        else:
                            retained_events.append(current_ev)

                event_ledger = retained_events
        for msg in pending_logs:
            append_to_log(msg)

# --- THREAD 3: HTTP NETWORK LISTENER INTERFACE ---
class TimingGateHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        query_params = urllib.parse.parse_qs(parsed_url.query)
        
        gate_id = query_params.get('id', ['UNKNOWN'])[0]
        tsf_raw = query_params.get('tsf', [None])[0]
        ap_bssid = query_params.get('bssid', ['UNKNOWN'])[0]
        clock_mode = query_params.get('mode', ['TSF'])[0]
        packet_type = query_params.get('type', ['TRA'])[0]
        
        if tsf_raw is None:
            self.send_response(400)
            self.end_headers()
            return
        
        try:
            tsf_value = int(tsf_raw)
        except ValueError:
            self.send_response(400)
            self.end_headers()
            return
        if tsf_value == 0:
            self.send_response(200)
            self.end_headers()
            return
        
        # Super fast registration loop - minimises lock holding time
        now = datetime.datetime.now()
        reconnect_msg = None
        is_heartbeat = False
        with ledger_lock:
            if gate_id in system_registry:
                if system_registry[gate_id]['status'] == 'OFFLINE':
                    reconnect_msg = f"INFO: Node [{gate_id}] has re-established communication."
                system_registry[gate_id]['last_seen'] = now
                system_registry[gate_id]['status'] = 'ONLINE'
                system_registry[gate_id]['clock_mode'] = clock_mode
                system_registry[gate_id]['bssid'] = ap_bssid

            if packet_type == 'HB':
                is_heartbeat = True
            else:
                key = (gate_id, packet_type)
                is_duplicate = last_event_tsf.get(key) == tsf_value
                if not is_duplicate:
                    last_event_tsf[key] = tsf_value
                    event_ledger.append({
                        'id': gate_id, 'tsf': tsf_value, 'bssid': ap_bssid, 'mode': clock_mode, 'arrival_time': now
                    })

        # Log and respond outside the lock - avoids holding it during I/O
        if reconnect_msg:
            append_to_log(reconnect_msg)
        if is_heartbeat:
            self.send_response(200)
            self.end_headers()
            return

        # Echo the TSF back so the gate can confirm this specific event was received.
        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write(tsf_raw.encode())

    def log_message(self, format, *args):
        return

if __name__ == "__main__":
    with open(log_file_path, "w", encoding="utf-8") as f:
        f.write(f"=== TIMING SYSTEM INTEGRATED LEDGER START: {datetime.datetime.now()} ===\n")
    print(f"Server starting on port {PORT}...")

    # Start Async Storage Engine
    disk_thread = threading.Thread(target=disk_writer_worker, daemon=True)
    disk_thread.start()

    threading.Thread(target=system_health_monitor, daemon=True).start()
    threading.Thread(target=process_ledger_window, daemon=True).start()

    server = http.server.HTTPServer(('0.0.0.0', PORT), TimingGateHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server safely.")
        log_queue.join()   # flush pending log writes before daemon thread is killed
        server.server_close()