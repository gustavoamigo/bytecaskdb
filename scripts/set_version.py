import sys
import re
import os

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <version>")
        sys.exit(1)

    version = sys.argv[1]
    if version.startswith('v'):
        version = version[1:]

    # Node.js
    pkg_json_path = "bytecaskdb-node/package.json"
    if os.path.exists(pkg_json_path):
        with open(pkg_json_path, "r") as f:
            content = f.read()
        content = re.sub(r'"version": "[^"]+"', f'"version": "{version}"', content, count=1)
        with open(pkg_json_path, "w") as f:
            f.write(content)

    # Python
    py_init_path = "bytecaskdb-python/bytecaskdb/__init__.py"
    if os.path.exists(py_init_path):
        with open(py_init_path, "r") as f:
            content = f.read()
        content = re.sub(r'__version__ = "[^"]+"', f'__version__ = "{version}"', content)
        with open(py_init_path, "w") as f:
            f.write(content)

    # MariaDB Plugin
    mariadb_cc_path = "bytecaskdb-mariadb-plugin/bytecaskdb_plugin.cc"
    if os.path.exists(mariadb_cc_path):
        parts = version.split('.')
        major = int(parts[0]) if len(parts) > 0 else 0
        minor = int(parts[1]) if len(parts) > 1 else 0
        hex_version = f"0x{major:02x}{minor:02x}"
        str_version = f"{major}.{minor}"

        with open(mariadb_cc_path, "r") as f:
            content = f.read()
        content = re.sub(r'0x[0-9a-fA-F]+,(\s*//\s*version.*)', f'{hex_version},\\1', content)
        content = re.sub(r'"[0-9]+\.[0-9]+",(\s*//\s*version string)', f'"{str_version}",\\1', content)
        with open(mariadb_cc_path, "w") as f:
            f.write(content)

    # C++
    xmake_path = "xmake.lua"
    if os.path.exists(xmake_path):
        with open(xmake_path, "r") as f:
            content = f.read()
        if "set_version(" in content:
            content = re.sub(r'set_version\("[^"]+"\)', f'set_version("{version}")', content)
        else:
            lines = content.splitlines()
            lines.insert(1, f'set_version("{version}")')
            content = "\n".join(lines) + "\n"
        with open(xmake_path, "w") as f:
            f.write(content)

    print(f"Version set to {version}")

if __name__ == "__main__":
    main()
