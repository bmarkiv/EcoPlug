import gzip
import hashlib
import re
from datetime import datetime
from pathlib import Path

def combine_html_chunks(html_path: Path) -> str:
    html = html_path.read_text(encoding="utf-8")
    base_dir = html_path.parent

    def replace_chunk(match: re.Match[str]) -> str:
        chunk_name = match.group(1)
        chunk_path = base_dir / chunk_name
        return chunk_path.read_text(encoding="utf-8")

    html = re.sub(r'<!--\s*#include\s+"([^"]+)"\s*-->', replace_chunk, html)
    html = html.replace("__DATE__", datetime.now().strftime("%b %d %Y %H:%M:%S"))
    html = re.sub(r"\n[ \t]+", "\n", html)
    # html = re.sub(r"\n", "", html)
    return html


def compress_html(html_path: Path, header_path: Path, array_name: str, len_name: str, etag_name: str) -> None:
    html = combine_html_chunks(html_path)

    date_time_macro = datetime.now().strftime("%b %d %Y %H:%M:%S")

    gz_data = gzip.compress(html.encode("utf-8"))
    byte_list = ",".join(f"0x{b:02x}" for b in gz_data)

    etag = '"' + hashlib.md5(gz_data).hexdigest()[:16] + '"'

    with header_path.open("w", encoding="utf-8") as f:
        f.write(f"const uint8_t {array_name}[] PROGMEM = {{\n")
        f.write(f"{byte_list}\n}};\n")
        f.write(f"const uint32_t {len_name} = {len(gz_data)};\n")
        f.write(f'const char {etag_name}[] = {etag};\n')
        f.write(f"// Generated: {date_time_macro}")

    print(f"Header regenerated: {header_path.name}, date: {date_time_macro}")


def generate_header() -> None:
    project_dir = Path(str(env["PROJECT_DIR"])) if env is not None else Path.cwd()
    html_path = project_dir / "index.html"

    print("Regenerating header...")

    compress_html(
        html_path,
        project_dir / "index_html_gz.h",
        "index_html_gz", "index_html_gz_len", "index_html_gz_etag",
    )

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
