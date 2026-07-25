#!/usr/bin/env python3
"""Change settings on a running device without clobbering the rest.

POST /save applies *every* field of the settings form at once, so a hand-rolled
partial POST silently clears whatever it omits. This reads the live form, keeps
all current values, and overrides only the fields given on the command line.

    python tools/device_settings.py 10.0.0.133 --dry-run
    python tools/device_settings.py 10.0.0.133 range_mi=32km dist_unit=mi
    python tools/device_settings.py 10.0.0.133 weather_units=imperial

range_mi accepts a unit suffix (30mi / 48km / 26nm) and snaps to the nearest
allowed ring. API keys render as empty password inputs and the firmware treats
empty as "keep existing" (saveIfNonEmpty), so they are never sent.

Unknown field names abort rather than post, so a typo cannot blank a setting.
Standard library only.
"""
from __future__ import annotations

import re
import sys
import urllib.parse
import urllib.request

# Belong to the Wi-Fi slot sub-forms, which POST to /wifi/* rather than /save.
STRAY_FIELDS = ("i", "s")


def read_form(host: str) -> tuple[dict[str, str], list[str]]:
    page = urllib.request.urlopen(f"http://{host}/", timeout=20).read().decode("utf-8", "replace")
    fields: dict[str, str] = {}
    passwords: list[str] = []

    for tag in re.findall(r"<input\b[^>]*>", page, re.I):
        name_m = re.search(r'name="([^"]+)"', tag)
        if not name_m:
            continue
        name = name_m.group(1)
        itype = (re.search(r'type="([^"]+)"', tag) or [None, "text"])[1].lower()
        value_m = re.search(r'value="([^"]*)"', tag)
        value = value_m.group(1) if value_m else ""
        if itype == "password":
            passwords.append(name)          # omitted: empty means keep
        elif itype in ("checkbox", "radio"):
            if re.search(r"\bchecked\b", tag, re.I):
                fields[name] = value or "on"  # unchecked boxes are simply absent
        else:
            fields[name] = value

    for m in re.finditer(r'<select\b[^>]*name="([^"]+)"[^>]*>(.*?)</select>', page, re.I | re.S):
        name, body = m.group(1), m.group(2)
        chosen = (re.search(r'<option\b[^>]*value="([^"]*)"[^>]*\bselected\b', body, re.I)
                  or re.search(r'<option\b[^>]*value="([^"]*)"', body, re.I))
        if chosen:
            fields[name] = chosen.group(1)

    for m in re.finditer(r'<textarea\b[^>]*name="([^"]+)"[^>]*>(.*?)</textarea>', page, re.I | re.S):
        fields[m.group(1)] = m.group(2).strip()

    for stray in STRAY_FIELDS:
        fields.pop(stray, None)
    return fields, passwords


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    host = args[0]
    dry_run = "--dry-run" in sys.argv

    fields, passwords = read_form(host)
    print(f"read {len(fields)} fields from {host}; keeping password fields: {passwords}")

    for override in args[1:]:
        if "=" not in override:
            print(f"skipping {override!r}: expected name=value")
            continue
        key, value = override.split("=", 1)
        if key not in fields:
            print(f"field {key!r} is not in the live form — aborting without posting")
            return 1
        print(f"  {key}: {fields[key]!r} -> {value!r}")
        fields[key] = value

    if dry_run:
        for k in sorted(fields):
            print(f"  {k} = {fields[k]!r}")
        print("dry run: nothing posted")
        return 0

    req = urllib.request.Request(
        f"http://{host}/save",
        data=urllib.parse.urlencode(fields).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=25) as resp:
        print("POST /save ->", resp.status)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
