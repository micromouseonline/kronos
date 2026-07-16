import csv
import os
import re
import subprocess
import time
import serial
import serial.tools.list_ports

CSV_FILE = "esp32_inventory.csv"
CSV_HEADERS = ["MAC Address", "Name", "Chip Type", "Flash Size", "RAM", "PSRAM"]

# RAM sizes by chip family (from Espressif datasheets)
CHIP_RAM = {
    "ESP32-S3": "512 KB SRAM",
    "ESP32-S2": "320 KB SRAM",
    "ESP32-C6": "320 KB SRAM",
    "ESP32-C3": "400 KB SRAM",
    "ESP32-H2": "320 KB SRAM",
    "ESP32":    "520 KB SRAM",
}

def find_esp32_port():
    """Attempts to auto-detect the serial port of the connected ESP32."""
    ports = list(serial.tools.list_ports.comports())
    keywords = ["CP210", "CH340", "CH341", "FTDI", "UART", "USB", "Espressif", "ACM"]

    detected_ports = []
    for port in ports:
        if any(kw.lower() in (port.description + port.device).lower() for kw in keywords):
            detected_ports.append(port.device)

    if not detected_ports:
        return None
    if len(detected_ports) == 1:
        return detected_ports[0]

    print("\nMultiple potential devices found:")
    for i, p in enumerate(detected_ports):
        print(f"[{i}] {p}")
    choice = input("Select the port index: ")
    try:
        return detected_ports[int(choice)]
    except (ValueError, IndexError):
        return None

def get_before_mode(port):
    """Returns the esptool --before flag appropriate for the device type.

    ESP32-S3 and ESP32-C6 boards connected via built-in USB-JTAG/Serial
    (VID=0x303A, PID=0x1001) need 'usb_reset'. DTR/RTS on that interface are
    virtual and do not connect to EN/BOOT, so the classic reset sequence does not
    work and can destabilize the port.
    """
    ESPRESSIF_VID = 0x303A
    USB_JTAG_SERIAL_PID = 0x1001
    for p in serial.tools.list_ports.comports():
        if p.device == port and p.vid == ESPRESSIF_VID and p.pid == USB_JTAG_SERIAL_PID:
            return "usb_reset"
    return "default_reset"

def get_esp32_details(port):
    """Runs esptool with retries to read chip identity and flash size.

    Uses --after no_reset on chip_id to keep the chip in download mode, then
    --before no_reset on flash_id to avoid a second reset/sync cycle. This
    eliminates the most common failure point on S3 and C6 native USB devices.
    """
    output = None
    flash_output = None
    before_mode = get_before_mode(port)

    for attempt in range(1, 6):
        print(f"Connecting to ESP32 on {port} (Attempt {attempt}/5)...")

        try:
            result = subprocess.run(
                [
                    "esptool", "--port", port, "--baud", "115200",
                    "--before", before_mode, "--after", "no_reset",
                    "chip_id",
                ],
                capture_output=True, text=True, timeout=20, check=True
            )
            output = result.stdout

            flash_result = subprocess.run(
                [
                    "esptool", "--port", port, "--baud", "115200",
                    "--before", "no_reset",
                    "flash_id",
                ],
                capture_output=True, text=True, timeout=20, check=True
            )
            flash_output = flash_result.stdout
            break

        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            print("  Failed to sync. Retrying...")
            time.sleep(1.0)

    if not output or not flash_output:
        print("\nError: Failed to communicate with ESP32.")
        print("Hardware fix: press and hold BOOT, tap RST, release BOOT, then try again.")
        return None

    mac = re.search(r"MAC:\s*([0-9a-fA-F:]+)", output)
    chip_type = re.search(r"Detecting chip type\.\.\.\s*(.+)", output)
    flash_size = re.search(r"Detected flash size:\s*(.+)", flash_output)
    features = re.search(r"Features:\s*(.+)", output)

    mac_str = mac.group(1).upper() if mac else "UNKNOWN"
    chip_str = chip_type.group(1).strip() if chip_type else "ESP32"
    flash_str = flash_size.group(1).strip() if flash_size else "UNKNOWN"

    ram_str = "520 KB SRAM"
    for family, ram in CHIP_RAM.items():
        if family in chip_str:
            ram_str = ram
            break

    features_str = features.group(1) if features else ""
    psram_str = "Embedded PSRAM" if "PSRAM" in features_str else "None detected"

    return {
        "mac": mac_str,
        "chip": chip_str,
        "flash": flash_str,
        "ram": ram_str,
        "psram": psram_str
    }

def is_duplicate(mac, filename):
    if not os.path.exists(filename):
        return False
    with open(filename, mode='r', newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row.get("MAC Address") == mac:
                return row.get("Name")
    return False

def init_csv(filename):
    if not os.path.exists(filename):
        with open(filename, mode='w', newline='', encoding='utf-8') as f:
            writer = csv.writer(f)
            writer.writerow(CSV_HEADERS)

def catalog_single_board():
    port = find_esp32_port()
    if not port:
        port = input("Could not auto-detect port. Enter manually (or leave blank to cancel): ").strip()
        if not port:
            return False

    details = get_esp32_details(port)
    if not details:
        return False

    print(f"\n--- Detected Board ---")
    print(f"MAC Address : {details['mac']}")
    print(f"Chip Type   : {details['chip']}")
    print(f"Flash Size  : {details['flash']}")
    print(f"Base RAM    : {details['ram']}")
    print(f"PSRAM       : {details['psram']}")
    print("----------------------")

    existing_name = is_duplicate(details['mac'], CSV_FILE)
    if existing_name:
        print(f"\nWARNING: Already cataloged as '{existing_name}'!")
        choice = input("Overwrite? (y/N): ").strip().lower()
        if choice != 'y':
            print("Skipping.")
            return True

    while True:
        name = input("\nEnter name (max 20 chars): ").strip()
        if 0 < len(name) <= 20:
            break
        print("Invalid name length.")

    with open(CSV_FILE, mode='a', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow([details['mac'], name, details['chip'], details['flash'], details['ram'], details['psram']])

    print(f"Added '{name}'.")
    return True

def main():
    init_csv(CSV_FILE)
    print("========================================")
    print("       ESP32 Inventory Cataloger        ")
    print("========================================")

    while True:
        print("\n--- Ready for Next Board ---")
        user_input = input("Plug in a board and press ENTER to scan (or type 'q' to quit): ").strip().lower()

        if user_input == 'q':
            break

        catalog_single_board()

    print("\nInventory complete. Saved to:", os.path.abspath(CSV_FILE))

if __name__ == "__main__":
    main()
