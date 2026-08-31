#!/usr/bin/env python3

"""Emit bounded, privacy-safe diagnostics from a hosted release build log."""

import argparse
import re
import sys
from pathlib import Path


ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
CONTROL_CHARACTER = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
HOME_PATH = re.compile(r"(?<![A-Za-z0-9])/(?:Users|home)/[^\s:'\"]+")
TEMP_PATH = re.compile(r"(?<![A-Za-z0-9])/(?:private/)?(?:tmp|var/folders)/[^\s:'\"]+")
PRIVATE_IPV4 = re.compile(
    r"\b(?:10(?:\.\d{1,3}){3}|192\.168(?:\.\d{1,3}){2}|"
    r"172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2})\b"
)
PRIVATE_IPV6 = re.compile(r"(?i)\b(?:f[cd][0-9a-f]{2}|fe8[0-9a-f]):[0-9a-f:]+\b")
SENSITIVE_ASSIGNMENT = re.compile(
    r"(?i)\b(token|password|secret|authorization|api[_-]?key)"
    r"(\s*[:=]\s*)([^\s]+)"
)
KNOWN_TOKEN = re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{16,}|github_pat_[A-Za-z0-9_]{16,})\b")
URL_USERINFO = re.compile(r"(https?://)[^/@\s:]+:[^/@\s]+@")
KEY_MATERIAL = re.compile(r"-----BEGIN [A-Z0-9 ]*(?:PRIVATE KEY|CERTIFICATE)-----")
ERROR_MARKER = re.compile(r"(?i)\b(?:error|fatal|failed|undefined symbol|not found)\b")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("log_path")
    parser.add_argument("--source-root")
    parser.add_argument("--build-root")
    parser.add_argument("--prefix-root")
    parser.add_argument("--temp-root")
    parser.add_argument("--max-lines", type=int, default=120)
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
        path = str(Path(raw_path).expanduser()).rstrip("/")
        if path:
            replacements.append((path, label))
    return sorted(replacements, key=lambda item: len(item[0]), reverse=True)


def sanitize(line: str, replacements) -> str:
    line = ANSI_ESCAPE.sub("", line)
    line = CONTROL_CHARACTER.sub("", line)
    for path, label in replacements:
        line = line.replace(path, label)
    line = HOME_PATH.sub("<home-path>", line)
    line = TEMP_PATH.sub("<temp-path>", line)
    line = PRIVATE_IPV4.sub("<private-address>", line)
    line = PRIVATE_IPV6.sub("<private-address>", line)
    line = SENSITIVE_ASSIGNMENT.sub(lambda match: f"{match.group(1)}=<redacted>", line)
    line = KNOWN_TOKEN.sub("<redacted-token>", line)
    line = URL_USERINFO.sub(r"\1<redacted>@", line)
    if KEY_MATERIAL.search(line):
        line = "<redacted-key-material>"
    return line.rstrip()


def main() -> int:
    args = parse_args()
    if args.max_lines < 1 or args.max_lines > 500:
        print("release-build-diagnostics: invalid line bound", file=sys.stderr)
        return 2

    try:
        raw_lines = Path(args.log_path).read_text(errors="replace").splitlines()
    except OSError:
        print("release-build-diagnostics: log unavailable", file=sys.stderr)
        return 2

    replacements = root_replacements(args)
    sanitized = [sanitize(line, replacements) for line in raw_lines]
    sanitized = [line for line in sanitized if line]
    emitted = sanitized[-args.max_lines :]
    error_markers = sum(1 for line in sanitized if ERROR_MARKER.search(line))

    print("release-build-diagnostics: sanitized tail")
    print(f"total_lines={len(raw_lines)}")
    print(f"emitted_lines={len(emitted)}")
    print(f"error_markers={error_markers}")
    for line in emitted:
        print(f"| {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
