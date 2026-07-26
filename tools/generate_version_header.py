import os
import subprocess
import configparser
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()


def get_release_version():
    """Reads the centralized version number from the metadata env block."""
    config = configparser.ConfigParser()
    paths = ["platformio.ini", "../base-boards.ini"]
    if not os.path.exists("platformio.ini") and os.path.exists("src"):
        paths = ["../platformio.ini", "../../base-boards.ini"]
    config.read(paths)

    if config.has_section("env:version_metadata"):
        return config.get("env:version_metadata", "release_version", fallback="1.0.0").strip('"')
    return "1.0.0"


def get_git_hash():
    """Last 6 hex digits of the current commit, or 'nogit' if unavailable."""
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short=6", "HEAD"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "nogit"


def write_version_header():
    version_string = f"{get_release_version()}-{get_git_hash()}"
    header_path = os.path.join(env.subst("$PROJECT_DIR"), "src", "version-generated.h")

    contents = (
        "#pragma once\n"
        f'#define FIRMWARE_VERSION_STRING "{version_string}"\n'
    )

    # Skip the write if unchanged, so the header's mtime doesn't force a
    # rebuild of everything that includes it when the version hasn't moved.
    if os.path.exists(header_path):
        with open(header_path, "r") as f:
            if f.read() == contents:
                return

    with open(header_path, "w") as f:
        f.write(contents)
    print(f"[INFO] Generated src/version-generated.h -- {version_string}")


# A "pre:" extra_script runs immediately as SCons loads it, before any
# sources are compiled -- unlike AddPreAction/AddPostAction, which only hook
# a specific later build step. That's what's needed here: main.cpp includes
# version-generated.h, so it must exist before compilation starts, not just
# before the final .bin is linked.
write_version_header()
