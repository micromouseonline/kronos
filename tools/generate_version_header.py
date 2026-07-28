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


def get_git_describe():
    """git-describe-style build id: <commit count>-g<short hash>[-dirty].

    The repo has no tags, so this stands in for `git describe --long`:
    total commit count on HEAD gives a monotonic serial (increases every
    commit, unlike the hash alone), followed by the short hash and a
    -dirty suffix if the working tree has uncommitted changes.
    Falls back to 'nogit' if git is unavailable.
    """
    project_dir = env.subst("$PROJECT_DIR")
    try:
        count = subprocess.check_output(
            ["git", "rev-list", "--count", "HEAD"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        short_hash = subprocess.check_output(
            ["git", "rev-parse", "--short=7", "HEAD"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        dirty = subprocess.check_output(
            ["git", "status", "--porcelain"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "nogit"

    suffix = "-dirty" if dirty else ""
    return f"{count}-g{short_hash}{suffix}"


def write_version_header():
    version_string = f"{get_release_version()}-{get_git_describe()}"
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
