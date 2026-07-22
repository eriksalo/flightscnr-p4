# PlatformIO pre-build: refresh ICAO aircraft type lookup from tar1090-db.
Import("env")

import subprocess
import sys
from pathlib import Path

project_dir = Path(env["PROJECT_DIR"])
tool = project_dir / "tools" / "icao_types_to_header.py"

print("Refreshing ICAO aircraft type lookup...")
result = subprocess.run(
    [sys.executable, str(tool)],
    cwd=str(project_dir),
)

if result.returncode != 0:
    print(
        "WARNING: ICAO type lookup refresh failed; "
        "continuing with existing generated files."
    )
