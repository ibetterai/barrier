#!/usr/bin/env python3

"""Emit bounded, privacy-safe structured diagnostics from a release build log."""

import argparse
import re
import sys
from pathlib import Path


ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
CONTROL_CHARACTER = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
COMPILER_DIAGNOSTIC = re.compile(
    r"^(?P<path>.+?):(?P<line>[0-9]+)(?::(?P<column>[0-9]+))?:\s*"
    r"(?P<severity>fatal error|error):\s*(?P<message>.*)$",
    re.IGNORECASE,
)
CMAKE_DIAGNOSTIC = re.compile(
    r"^CMake Error at (?P<path>.+?):(?P<line>[0-9]+)(?:\s+\([^)]*\))?:?",
    re.IGNORECASE,
)
COMPILER_DRIVER_DIAGNOSTIC = re.compile(
    r"^(?:apple )?(?P<tool>clang(?:\+\+)?|cc|c\+\+|ld):\s*"
    r"(?:fatal\s+)?error:\s*(?P<message>.*)$",
    re.IGNORECASE,
)
UNDEFINED_SYMBOLS = re.compile(
    r"^Undefined symbols for architecture (?P<arch>[A-Za-z0-9_-]{1,32}):?$",
    re.IGNORECASE,
)
LINKER_NOT_FOUND = re.compile(
    r"^(?:ld|clang(?:\+\+)?):.*(?:symbol\(s\) not found|linker command failed)",
    re.IGNORECASE,
)
BUILD_TOOL_FAILURE = re.compile(
    r"^(?:ninja: build stopped: subcommand failed\.?|"
    r"(?:g?make|make)(?:\[[0-9]+\])?: \*\*\* .*Error [0-9]+)$",
    re.IGNORECASE,
)
AUTOGEN_FAILURE = re.compile(
    r"^(?:AutoMoc|AutoUic|AutoRcc)(?: subprocess)? error", re.IGNORECASE
)
PEM_BEGIN = re.compile(r"-----BEGIN [^-]+-----", re.IGNORECASE)
PEM_END = re.compile(r"-----END [^-]+-----", re.IGNORECASE)
URL = re.compile(r"\b[a-z][a-z0-9+.-]*://[^\s'\"<>]+", re.IGNORECASE)
WINDOWS_PATH = re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/][^\s'\"<>|]+")
UNC_PATH = re.compile(r"\\\\[^\s\\/]+[\\/][^\s'\"<>|]+")
POSIX_ABSOLUTE_PATH = re.compile(r"(?<![A-Za-z0-9_.-])/(?:[^/\s'\"<>|][^\s'\"<>|]*)")
TILDE_PATH = re.compile(r"(?<![A-Za-z0-9])~[/\\][^\s'\"<>|]+")
IPV4_ADDRESS = re.compile(
    r"(?<![0-9.])(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})"
    r"(?:\.(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})){3}(?![0-9.])"
)
IPV6_ADDRESS = re.compile(
    r"(?i)(?<![0-9a-f:])(?:(?:[0-9a-f]{1,4}:){7}[0-9a-f]{1,4}|"
    r"[0-9a-f:]*::[0-9a-f:]*)(?![0-9a-f:])"
)
MAC_ADDRESS = re.compile(r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f])")
SENSITIVE_TERM = re.compile(
    r"(?i)(?:^|[^A-Za-z0-9])(?:[A-Za-z0-9_-]*"
    r"(?:token|secret|password|passwd|authorization|credential|cookie|"
    r"private[_-]?key|access[_-]?key|api[_-]?key|bearer)"
    r"[A-Za-z0-9_-]*)(?:[^A-Za-z0-9]|$)"
)
OPAQUE_VALUE = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z0-9_+/=-]{32,}(?![A-Za-z0-9_])")
SAFE_ARCH = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
SAFE_RELATIVE_PATH = re.compile(r"^[A-Za-z0-9._+/@~-]+$")
SAFE_LOCATION_OUTPUT = (
    r"(?:barrier-(?:source|build|prefix)|runner-temp)"
    r"(?:/[A-Za-z0-9._+/@~-]+)?|<(?:external|relative)-path>"
)
ALLOWED_DETAIL = re.compile(
    rf"^\| (?:compiler (?:fatal )?error at (?:{SAFE_LOCATION_OUTPUT}):"
    r"[0-9]+(?::[0-9]+)?|"
    rf"cmake error at (?:{SAFE_LOCATION_OUTPUT}):[0-9]+|"
    r"compiler driver error: command failed|"
    r"linker error: (?:command failed|undefined symbols for architecture [A-Za-z0-9_-]{1,32})|"
    r"build tool error: command failed|autogen error: subprocess failed)$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("log_path")
    parser.add_argument("--source-root")
    parser.add_argument("--build-root")
    parser.add_argument("--prefix-root")
    parser.add_argument("--temp-root")
    parser.add_argument("--max-lines", type=int, default=80)
    parser.add_argument("--max-line-bytes", type=int, default=768)
    parser.add_argument("--max-output-bytes", type=int, default=49152)
    parser.add_argument("--max-input-bytes", type=int, default=8 * 1024 * 1024)
    return parser.parse_args()


def root_replacements(args: argparse.Namespace):
    candidates = (
        (args.source_root, "barrier-source"),
        (args.build_root, "barrier-build"),
        (args.prefix_root, "barrier-prefix"),
        (args.temp_root, "runner-temp"),
    )
    replacements = []
    for raw_path, label in candidates:
        if not raw_path:
            continue
        path = str(Path(raw_path).expanduser()).rstrip("/\\")
        if path:
            replacements.append((path, label))
    return sorted(replacements, key=lambda item: len(item[0]), reverse=True)


def read_bounded_tail(path: Path, max_bytes: int):
    size = path.stat().st_size
    offset = max(0, size - max_bytes)
    with path.open("rb") as handle:
        handle.seek(offset)
        raw = handle.read(max_bytes)
    if offset:
        newline = raw.find(b"\n")
        raw = b"" if newline < 0 else raw[newline + 1 :]
    return raw.decode("utf-8", errors="replace").splitlines(), bool(offset)


def strip_controls(value: str) -> str:
    value = ANSI_ESCAPE.sub("", value)
    value = CONTROL_CHARACTER.sub("", value)
    return "".join(character if ord(character) < 128 else "?" for character in value)


def safe_path_fragment(value: str) -> str:
    value = value.replace("\\", "/").lstrip("/")
    pieces = []
    for piece in value.split("/"):
        if not piece or piece in (".", ".."):
            continue
        if len(piece) > 64 or SENSITIVE_TERM.search(piece) or OPAQUE_VALUE.search(piece):
            pieces.append("_redacted_")
            continue
        safe_piece = re.sub(r"[^A-Za-z0-9._+@~-]", "_", piece)
        pieces.append(safe_piece or "_")
    result = "/".join(pieces)[:240]
    return result or "_"


def normalize_location(raw_path: str, replacements) -> str:
    path = strip_controls(raw_path.strip())
    normalized = path.replace("\\", "/")
    for root, label in replacements:
        normalized_root = root.replace("\\", "/")
        if normalized == normalized_root:
            return label
        prefix = normalized_root + "/"
        if normalized.startswith(prefix):
            return f"{label}/{safe_path_fragment(normalized[len(prefix):])}"

    if normalized.startswith("/") or WINDOWS_PATH.search(path) or normalized.startswith("//"):
        return "<external-path>"
    if SAFE_RELATIVE_PATH.fullmatch(normalized):
        return f"barrier-source/{safe_path_fragment(normalized)}"
    return "<relative-path>"


def structured_diagnostic(line: str, replacements):
    match = COMPILER_DIAGNOSTIC.match(line)
    if match:
        location = normalize_location(match.group("path"), replacements)
        line_number = match.group("line")
        column = match.group("column")
        if column:
            line_number = f"{line_number}:{column}"
        severity = match.group("severity").lower()
        return f"compiler {severity} at {location}:{line_number}"

    match = CMAKE_DIAGNOSTIC.match(line)
    if match:
        location = normalize_location(match.group("path"), replacements)
        return f"cmake error at {location}:{match.group('line')}"

    match = COMPILER_DRIVER_DIAGNOSTIC.match(line)
    if match:
        return "compiler driver error: command failed"

    match = UNDEFINED_SYMBOLS.match(line)
    if match and SAFE_ARCH.fullmatch(match.group("arch")):
        return f"linker error: undefined symbols for architecture {match.group('arch')}"

    if LINKER_NOT_FOUND.match(line):
        return "linker error: command failed"
    if BUILD_TOOL_FAILURE.match(line):
        return "build tool error: command failed"
    if AUTOGEN_FAILURE.match(line):
        return "autogen error: subprocess failed"
    return None


def bound_line(line: str, max_bytes: int) -> str:
    encoded = line.encode("ascii", errors="replace")
    if len(encoded) <= max_bytes:
        return encoded.decode("ascii")
    suffix = b"...<truncated>"
    return (encoded[: max_bytes - len(suffix)] + suffix).decode("ascii")


def select_first_and_last(values, limit: int):
    if len(values) <= limit:
        return list(values)
    first_count = (limit + 1) // 2
    return list(values[:first_count]) + list(values[-(limit - first_count) :])


def candidate_is_safe(candidate: str, max_line_bytes: int, max_output_bytes: int) -> bool:
    encoded = candidate.encode("ascii", errors="strict")
    if len(encoded) > max_output_bytes or CONTROL_CHARACTER.search(candidate):
        return False
    lines = candidate.splitlines()
    if len(lines) < 6 or lines[0] != (
        "release-build-diagnostics: privacy-safe structured diagnostics"
    ):
        return False
    header_patterns = (
        re.compile(r"^scanned_lines=[0-9]+$"),
        re.compile(r"^matched_diagnostics=[0-9]+$"),
        re.compile(r"^emitted_diagnostics=[0-9]+$"),
        re.compile(r"^input_truncated=(?:yes|no)$"),
    )
    if not all(pattern.fullmatch(line) for pattern, line in zip(header_patterns, lines[1:5])):
        return False
    emitted_count = int(lines[3].split("=", 1)[1])
    matched_count = int(lines[2].split("=", 1)[1])
    if emitted_count != len(lines[5:]) or matched_count < emitted_count:
        return False
    for line in lines:
        if len(line.encode("ascii")) > max_line_bytes:
            return False
    if not all(ALLOWED_DETAIL.fullmatch(line) for line in lines[5:]):
        return False
    forbidden = (
        PEM_BEGIN,
        PEM_END,
        URL,
        WINDOWS_PATH,
        UNC_PATH,
        TILDE_PATH,
        POSIX_ABSOLUTE_PATH,
        IPV4_ADDRESS,
        IPV6_ADDRESS,
        MAC_ADDRESS,
        SENSITIVE_TERM,
        OPAQUE_VALUE,
    )
    return not any(pattern.search(candidate) for pattern in forbidden)


def main() -> int:
    args = parse_args()
    if (
        args.max_lines < 1
        or args.max_lines > 200
        or args.max_line_bytes < 128
        or args.max_line_bytes > 2048
        or args.max_output_bytes < 1024
        or args.max_output_bytes > 131072
        or args.max_input_bytes < 4096
        or args.max_input_bytes > 64 * 1024 * 1024
    ):
        print("release-build-diagnostics: invalid bound", file=sys.stderr)
        return 2

    try:
        raw_lines, input_truncated = read_bounded_tail(
            Path(args.log_path), args.max_input_bytes
        )
    except OSError:
        print("release-build-diagnostics: log unavailable", file=sys.stderr)
        return 2

    replacements = root_replacements(args)
    diagnostics = []
    inside_pem = False
    for raw_line in raw_lines:
        line = strip_controls(raw_line).strip()
        if PEM_BEGIN.search(line):
            inside_pem = True
            continue
        if inside_pem:
            if PEM_END.search(line):
                inside_pem = False
            continue
        diagnostic = structured_diagnostic(line, replacements)
        if diagnostic:
            diagnostics.append(diagnostic)

    if not diagnostics:
        print("release-build-diagnostics: no structured diagnostics", file=sys.stderr)
        return 3

    selected = select_first_and_last(diagnostics, args.max_lines)
    details = [bound_line(f"| {line}", args.max_line_bytes) for line in selected]
    while True:
        header = [
            "release-build-diagnostics: privacy-safe structured diagnostics",
            f"scanned_lines={len(raw_lines)}",
            f"matched_diagnostics={len(diagnostics)}",
            f"emitted_diagnostics={len(details)}",
            f"input_truncated={'yes' if input_truncated else 'no'}",
        ]
        candidate = "\n".join(header + details) + "\n"
        if len(candidate.encode("ascii")) <= args.max_output_bytes:
            break
        if len(details) <= 1:
            print("release-build-diagnostics: output bound unavailable", file=sys.stderr)
            return 3
        details.pop(len(details) // 2)

    if not candidate_is_safe(candidate, args.max_line_bytes, args.max_output_bytes):
        print("release-build-diagnostics: candidate rejected", file=sys.stderr)
        return 3

    sys.stdout.write(candidate)
    return 0


if __name__ == "__main__":
    sys.exit(main())
