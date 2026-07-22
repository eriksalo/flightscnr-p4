# PlatformIO pre-build: refresh ICAO airport lookup from mwgg/Airports.
Import("env")

import subprocess
import sys
from pathlib import Path

project_dir = Path(env["PROJECT_DIR"])
tool = project_dir / "tools" / "airports_to_header.py"

print("Refreshing airport lookup...")
result = subprocess.run(
    [sys.executable, str(tool)],
    cwd=str(project_dir),
)

if result.returncode != 0:
    print(
        "WARNING: airport lookup refresh failed; "
        "continuing with existing generated files."
    )
