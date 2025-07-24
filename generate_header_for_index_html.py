import gzip
import os
from datetime import datetime

HTML_PATH = "index.html"
HEADER_PATH = "index_html_gz.h"

def file_modified_after(src, target):
    return not os.path.exists(target) or os.path.getmtime(src) > os.path.getmtime(target)

if file_modified_after(HTML_PATH, HEADER_PATH):
    print("🔄 Regenerating header...")

    # Load and process HTML
    with open(HTML_PATH, "r", encoding="utf-8") as f:
        html = f.read()

    # Inject build timestamp
    date_time_macro = datetime.now().strftime("%b %d %Y %H:%M:%S")
    html = html.replace("__DATE__", date_time_macro)

    # Compress and generate header file
    gz_data = gzip.compress(html.encode("utf-8"))
    byte_list = ','.join(f"0x{b:02x}" for b in gz_data)

    with open(HEADER_PATH, "w") as f:
        f.write("const uint8_t index_html_gz[] PROGMEM = {\n")
        f.write(f"{byte_list}\n}};\n")
        f.write(f"const uint32_t index_html_gz_len = {len(gz_data)};\n")

    print(f"✅ Header regenerated: {HEADER_PATH}")
else:
    print("⏸️ No changes. Header file is up to date.")
