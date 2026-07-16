import configparser
import sys

# Keys that are meant to merge across `extends` parents (as opposed to scalar
# overrides like board_build.flash_mode, which are meant to fully replace).
COMPOSE_KEYS = ("build_flags", "lib_deps")

DEFAULT_FILES = ["base-boards.ini"]


def load_raw_config(path):
    """Parses without interpolation so ${section.option} stays literal text."""
    parser = configparser.RawConfigParser()
    parser.read(path)
    return parser


def defining_parents(parser, parents, key):
    return [
        p for p in parents
        if parser.has_section(p) and parser.has_option(p, key) and parser.get(p, key).strip()
    ]


def check_file(path):
    parser = load_raw_config(path)
    issues = []

    for section in parser.sections():
        if not parser.has_option(section, "extends"):
            continue

        parents = [p.strip() for p in parser.get(section, "extends").split(",") if p.strip()]

        for key in COMPOSE_KEYS:
            parents_with_key = defining_parents(parser, parents, key)
            if not parents_with_key:
                continue

            if parser.has_option(section, key):
                child_value = parser.get(section, key)
                for parent in parents_with_key:
                    ref = f"${{{parent}.{key}}}"
                    if ref not in child_value:
                        issues.append(
                            f"{path}: [{section}] defines {key} but does not reference "
                            f"{ref} (inherited via extends={parent})"
                        )
            elif len(parents_with_key) > 1:
                issues.append(
                    f"{path}: [{section}] does not define {key} itself but extends multiple "
                    f"parents that define it ({', '.join(parents_with_key)}) - merge order is "
                    f"not guaranteed, compose it explicitly"
                )

    return issues


def main():
    files = sys.argv[1:] or DEFAULT_FILES
    all_issues = []
    for f in files:
        all_issues.extend(check_file(f))

    if all_issues:
        print("Found composition issues:\n")
        for issue in all_issues:
            print(f"  - {issue}")
        sys.exit(1)

    print("No composition issues found.")


if __name__ == "__main__":
    main()
