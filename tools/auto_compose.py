import os
import configparser

def get_config():
    config = configparser.ConfigParser()
    paths = ["platformio.ini", "../boards.ini"]
    if not os.path.exists("platformio.ini") and os.path.exists("src"):
        paths = ["../platformio.ini", "../boards.ini"]
    config.read(paths)
    return config

def resolve_inheritance(config, section, seen=None):
    if seen is None: seen = []
    if not config.has_section(section) or section in seen:
        return seen
    seen.append(section)
    extends_val = config.get(section, "extends", fallback="")
    if extends_val:
        for parent in [p.strip() for p in extends_val.split(",")]:
            resolve_inheritance(config, parent, seen)
    return seen

def gather_flags_and_deps(config, target_section):
    all_sections = resolve_inheritance(config, target_section)
    all_sections.reverse()
    flags, deps = [], []
    for section in all_sections:
        if config.has_option(section, "build_flags"):
            lines = config.get(section, "build_flags").strip().splitlines()
            flags.extend([f.strip() for f in lines if f.strip() and not f.strip().startswith("$")])
        if config.has_option(section, "lib_deps"):
            lines = config.get(section, "lib_deps").strip().splitlines()
            deps.extend([d.strip() for d in lines if d.strip() and not d.strip().startswith("$")])
    return flags, deps

def generate_markdown(config, output_path="ARDUINO_GUIDE.md"):
    """Generates the Markdown guide for Arduino IDE users by resolving inherited properties."""
    with open(output_path, "w") as f:
        f.write("# Arduino IDE Setup Reference Guide\n")
        f.write("> **Note:** This file is auto-generated from `platformio.ini` and `boards.ini`.\n\n")
        
        for section in config.sections():
            if not section.startswith("env:"): 
                continue
            
            env_name = section.replace("env:", "")
            f.write(f"## Target Environment: `{env_name}`\n")
            
            # Resolve the parent sections to look up inherited hardware properties
            all_sections = resolve_inheritance(config, section)
            
            # Find the first section in the inheritance chain that defines 'board'
            board = "Unknown"
            for s in all_sections:
                if config.has_option(s, "board"):
                    board = config.get(s, "board")
                    break
                    
            # Find the first section in the inheritance chain that defines 'flash_size'
            flash_size = "Default"
            for s in all_sections:
                if config.has_option(s, "board_upload.flash_size"):
                    flash_size = config.get(s, "board_upload.flash_size")
                    break
            
            f.write("### 1. Arduino IDE Menu Settings\n")
            f.write(f"* **Target Board:** Select the menu item corresponding to platformio board target: `{board}`\n")
            f.write(f"* **Flash Size:** {flash_size}\n")
            
            flags, deps = gather_flags_and_deps(config, section)
            
            if flags:
                f.write("\n### 2. Required Compiler Macros\n")
                f.write("Create a file named `config_flags.h` in your sketch and paste:\n")
                f.write("```cpp\n")
                for flag in flags:
                    if flag.startswith("-D"):
                        parts = flag.replace("-D", "").strip().split("=")
                        key = parts[0]
                        val = parts[1] if len(parts) > 1 else "1"
                        f.write(f"#define {key} {val}\n")
                f.write("```\n")
                
            if deps:
                f.write("\n### 3. Required Libraries (Install via Library Manager)\n")
                for dep in deps: 
                    f.write(f"* {dep}\n")
                    
            f.write("\n---\n\n")

# PlatformIO environment hook execution
if "__main__" != __name__:
    config = get_config()
    generate_markdown(config, output_path="ARDUINO_GUIDE.md")

# Manual terminal execution mode
elif __name__ == "__main__":
    config = get_config()
    generate_markdown(config)
    print("Successfully generated ARDUINO_GUIDE.md for Arduino IDE users!")