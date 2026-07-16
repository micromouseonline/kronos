import os
import shutil
import configparser
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
platform = env.PioPlatform()
board = env.BoardConfig()

def get_firmware_version():
    """Reads the centralized version number from the metadata env block."""
    config = configparser.ConfigParser()
    paths = ["platformio.ini", "../base-boards.ini"]
    if not os.path.exists("platformio.ini") and os.path.exists("src"):
        paths = ["../platformio.ini", "../../base-boards.ini"]
    config.read(paths)
    
    if config.has_section("env:version_metadata"):
        return config.get("env:version_metadata", "release_version", fallback="1.0.0").strip('"')
    return "1.0.0"

def bld_merged_bin(source, target, env, **kwargs):
    # Fetch paths to the generated binaries
    build_dir = env.subst("$BUILD_DIR")
    fw_version = get_firmware_version()
    env_name = env.subst("$PIOENV")
    
    # Define destination folder
    dist_dir = os.path.join(env.subst("$PROJECT_DIR"), "dist")
    os.makedirs(dist_dir, exist_ok=True)
    
    output_bin_name = f"{env_name}-v{fw_version}-all-in-one.bin"
    output_path = os.path.join(dist_dir, output_bin_name)

    # Gather commands platformio used to flash the board to discover offsets
    flash_images = env.get("FLASH_EXTRA_IMAGES", [])
    app_offset = env.subst("$ESP32_APP_OFFSET")
    app_bin = os.path.join(build_dir, "firmware.bin")

    # Build the esptool merge command line arguments using dynamic chip selection
    cmd = [
        '"$PYTHONEXE"', '"$PROJECT_PACKAGES_DIR/tool-esptoolpy/esptool.py"',
        "--chip", env.subst("$BOARD_MCU"),
        "merge_bin",
        "-o", f'"{output_path}"',
    ]

    # Add bootloader, partition table, and otadata (if present) with their offsets
    for offset, image in flash_images:
        cmd.extend([offset, f'"{image}"'])

    # Add the primary application firmware offset and file
    cmd.extend([app_offset, f'"{app_bin}"'])

    print(f"\n[INFO] Generating All-In-One Factory Binary for {env_name}...")
    
    # Execute the esptool merge process
    rc = env.Execute(" ".join(cmd))
    if rc == 0:
        print(f"[SUCCESS] Saved factory image to: {output_path}\n")
    else:
        print(f"[ERROR] Failed to generate merged binary.\n")

# Hook into PlatformIO's build pipeline *after* firmware.bin creation
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", bld_merged_bin)