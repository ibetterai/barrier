#!/usr/bin/env python3

"""Verify the audited one-release workflow pair for immutable v3.4.0.

Version 3.4.0 predates a tag-owned product driver. Its protected product tag
supplies the original recipe while a separate protected automation tag supplies
the repaired orchestration. The complete workflow files were compared during
review and are accepted only as this exact hash pair. Any unknown step, field,
or command changes a full-file fingerprint and fails closed. Future release
tags must use a tag-owned driver.
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path


SUPPORTED_TAG = "v3.4.0"
SOURCE_WORKFLOW_SHA256 = "c0f707e5b4d51c4d1a36545c2f681a9a79cdb567dac1a97f4d8dad9dad639601"
AUTOMATION_WORKFLOW_SHA256 = "4cbff406f667d099e3608c88c05262ae6b7dec144570f92a992630bafe36c74b"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MAX_WORKFLOW_BYTES = 128 * 1024


class VerificationError(Exception):
    pass


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        del message
        raise VerificationError


def fingerprint(path_text: str) -> str:
    try:
        path = Path(path_text)
        if not path.is_file() or path.is_symlink():
            raise VerificationError
        size = path.stat().st_size
        if size < 1 or size > MAX_WORKFLOW_BYTES:
            raise VerificationError
        contents = path.read_bytes()
    except OSError as error:
        raise VerificationError from error
    return hashlib.sha256(contents).hexdigest()


def parse_args() -> argparse.Namespace:
    parser = SafeArgumentParser(add_help=False)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--source-workflow", required=True)
    parser.add_argument("--automation-workflow", required=True)
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        if args.release_tag != SUPPORTED_TAG:
            raise VerificationError
        if not SHA256_RE.fullmatch(AUTOMATION_WORKFLOW_SHA256):
            raise VerificationError
        if fingerprint(args.source_workflow) != SOURCE_WORKFLOW_SHA256:
            raise VerificationError
        if fingerprint(args.automation_workflow) != AUTOMATION_WORKFLOW_SHA256:
            raise VerificationError
    except VerificationError:
        print("release-recipe verification failed", file=sys.stderr)
        return 1
    except Exception:
        print("release-recipe verification failed", file=sys.stderr)
        return 1
    print("release recipe pair verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
