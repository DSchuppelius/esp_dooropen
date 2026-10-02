# PlatformIO-Vorab-Skript: bettet src/web/index.html gzip-komprimiert als
# C-Array in include/index_html.h ein. Der ESP liefert die Seite mit
# "Content-Encoding: gzip" aus (schneller, weniger Flash).
# Die Datei wird nur neu geschrieben, wenn sich der Inhalt aendert.
import gzip
import os
import re
import shutil
import subprocess
import sys
import tempfile

Import("env")  # noqa: F821 (von PlatformIO bereitgestellt)

root = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(root, "src", "web", "index.html")
dst = os.path.join(root, "include", "index_html.h")

with open(src, "rb") as f:
    html = f.read()

# JavaScript der Seite auf Syntaxfehler pruefen (mit Node, falls installiert):
# ein Fehler legt sonst die ganze Weboberflaeche lahm
node = shutil.which("node")
m = re.search(rb"<script>(.*)</script>", html, re.S)
if node and m:
    fd, js = tempfile.mkstemp(suffix=".js")
    with os.fdopen(fd, "wb") as f:
        f.write(m.group(1))
    try:
        r = subprocess.run([node, "--check", js], capture_output=True, text=True)
    finally:
        os.remove(js)
    if r.returncode != 0:
        sys.stderr.write("FEHLER: JavaScript in src/web/index.html ist ungueltig:\n" + r.stderr)
        env.Exit(1)  # noqa: F821
elif not node:
    print("Hinweis: node nicht gefunden - JavaScript der Weboberflaeche wird nicht geprueft")

data = gzip.compress(html, compresslevel=9, mtime=0)

lines = []
for i in range(0, len(data), 20):
    lines.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 20]) + ",")

out = (
    "// Automatisch erzeugt aus src/web/index.html (tools/embed_html.py) - nicht bearbeiten.\n"
    "#pragma once\n"
    "#include <Arduino.h>\n\n"
    "static const size_t INDEX_HTML_GZ_LEN = %d;\n"
    "static const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n%s\n};\n" % (len(data), "\n".join(lines))
)

os.makedirs(os.path.dirname(dst), exist_ok=True)
old = None
if os.path.exists(dst):
    with open(dst, "r") as f:
        old = f.read()
if old != out:
    with open(dst, "w", newline="\n") as f:
        f.write(out)
    print("index_html.h erzeugt (%d Bytes gzip)" % len(data))
