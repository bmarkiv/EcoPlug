import gzip
import hashlib
from datetime import datetime
from pathlib import Path

def generate_header() -> None:
    project_dir = Path(str(env["PROJECT_DIR"])) if env is not None else Path.cwd()
    html_path = project_dir / "index.html"
    header_path = project_dir / "index_html_gz.h"

    print("Regenerating header...")

    with html_path.open("r", encoding="utf-8") as f:
        html = f.read()

    date_time_macro = datetime.now().strftime("%b %d %Y %H:%M:%S")
    html = html.replace("__DATE__", date_time_macro)

    gz_data = gzip.compress(html.encode("utf-8"))
    byte_list = ",".join(f"0x{b:02x}" for b in gz_data)

    etag = '"' + hashlib.md5(gz_data).hexdigest()[:16] + '"'

    with header_path.open("w", encoding="utf-8") as f:
        f.write("const uint8_t index_html_gz[] PROGMEM = {\n")
        f.write(f"{byte_list}\n}};\n")
        f.write(f"const uint32_t index_html_gz_len = {len(gz_data)};\n")
        f.write(f'const char index_html_gz_etag[] = {etag};\n')

    print(f"Header regenerated: {header_path.name}, date: {date_time_macro}")

try:
    Import("env")  # type: ignore[name-defined]
except Exception:
    env = None

if env is not None:
    # Execute immediately when PlatformIO loads pre-scripts (before compile).
    generate_header()
else:
    # Allows running this file directly for manual regeneration.
    generate_header()
