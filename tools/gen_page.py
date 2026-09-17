"""Pre-build step: gzip web/dashboard.html into include/dashboard_html.h.

The board serves the compressed bytes with Content-Encoding: gzip. Over a WiFi
link that is time-sharing the radio with BLE, the page transfer is the slowest
thing the dashboard does, and gzip cuts it by roughly four times.
"""
import gzip
import pathlib
import re
import shutil
import subprocess
import tempfile

Import("env")  # noqa: F821  (injected by PlatformIO's SCons environment)

ROOT = pathlib.Path(env.subst("$PROJECT_DIR"))  # noqa: F821
SRC = ROOT / "web" / "dashboard.html"
DST = ROOT / "include" / "dashboard_html.h"

raw = SRC.read_bytes()

# A broken page is invisible until it's flashed and opened in a browser, so it
# gets checked here. `node --check` catches syntax; smoke_page.js evaluates the
# script against a DOM stub, which is what catches a declaration-order
# ReferenceError - the kind that throws at load and blanks the whole dashboard.
node = shutil.which("node")
if node:
    script = re.search(rb"<script>(.*?)</script>", raw, re.S)
    if script:
        with tempfile.NamedTemporaryFile("wb", suffix=".js", delete=False) as fh:
            fh.write(script.group(1))
            tmp = fh.name
        check = subprocess.run([node, "--check", tmp], capture_output=True, text=True)
        pathlib.Path(tmp).unlink(missing_ok=True)
        if check.returncode != 0:
            raise SystemExit("dashboard.html has a JavaScript syntax error:\n" + check.stderr)

    smoke = subprocess.run([node, str(ROOT / "tools" / "smoke_page.js"), str(SRC)],
                           capture_output=True, text=True)
    if smoke.returncode != 0:
        raise SystemExit(smoke.stderr or smoke.stdout)

# mtime=0 keeps the output byte-identical between builds, so an unchanged page
# doesn't force a rebuild of everything that includes the header.
blob = gzip.compress(raw, compresslevel=9, mtime=0)

rows = [
    "    " + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + ","
    for i in range(0, len(blob), 16)
]
out = (
    "#pragma once\n"
    "#include <Arduino.h>\n\n"
    "// Generated from web/dashboard.html by tools/gen_page.py - do not edit.\n"
    "static const size_t DASHBOARD_HTML_GZ_LEN = %d;\n"
    "static const uint8_t DASHBOARD_HTML_GZ[] PROGMEM = {\n%s\n};\n"
) % (len(blob), "\n".join(rows))

if not DST.exists() or DST.read_text() != out:
    DST.write_text(out)
    print("dashboard: %d bytes -> %d gzipped (%.0f%%)"
          % (len(raw), len(blob), 100.0 * len(blob) / len(raw)))
