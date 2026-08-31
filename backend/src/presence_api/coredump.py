from __future__ import annotations

import os
import re
import subprocess
import tempfile
from pathlib import Path

_SAFE_LINE = re.compile(
    r"^\s*(?:#\d+|Backtrace|Stack frame|Task|Exception|Core dump|"
    r"ELF file SHA256|================)"
)
_LONG_TOKEN = re.compile(r"(?<![0-9A-Fa-fx])[A-Za-z0-9_+/=-]{40,}")
_ARGUMENTS = re.compile(r"\([^()]{1,500}\)")
_SECRET_WORD = re.compile(
    r"password|passwd|token|credential|secret|ssid|authorization",
    re.IGNORECASE,
)


def _sanitize_decoder_output(output: str) -> list[str]:
    result: list[str] = []
    for source_line in output.splitlines():
        line = source_line.strip()
        if not line or not _SAFE_LINE.match(line):
            continue
        if _SECRET_WORD.search(line):
            line = "[redacted sensitive decoder line]"
        else:
            line = _ARGUMENTS.sub("(...)", line)
            line = _LONG_TOKEN.sub("[redacted]", line)
        result.append(line[:240])
        if len(result) >= 80:
            break
    return result


def symbolize_coredump(
    decoder: Path | None,
    raw_dump: bytes,
    elf: bytes,
) -> tuple[str, list[str]]:
    if decoder is None or not decoder.is_file() or not os.access(decoder, os.X_OK):
        return "matched_elf", ["Exact ELF retained; decoder is not configured."]
    with tempfile.TemporaryDirectory(prefix="m5go-coredump-") as directory:
        root = Path(directory)
        dump_path = root / "core.bin"
        elf_path = root / "firmware.elf"
        dump_path.write_bytes(raw_dump)
        elf_path.write_bytes(elf)
        os.chmod(dump_path, 0o600)
        os.chmod(elf_path, 0o600)
        try:
            completed = subprocess.run(
                (
                    os.fspath(decoder),
                    "--chip",
                    "esp32",
                    "info_corefile",
                    "-t",
                    "raw",
                    "-c",
                    os.fspath(dump_path),
                    os.fspath(elf_path),
                ),
                check=False,
                capture_output=True,
                timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired):
            return "failed", ["Core-dump decoder failed or timed out."]
    output = (completed.stdout + b"\n" + completed.stderr).decode(
        "utf-8", errors="replace"
    )
    sanitized = _sanitize_decoder_output(output)
    if completed.returncode != 0:
        return "failed", sanitized or ["Core-dump decoder rejected the dump."]
    return "succeeded", sanitized or ["Core dump decoded without stack frames."]
