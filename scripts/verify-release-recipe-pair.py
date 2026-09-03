#!/usr/bin/env python3

"""Verify an audited release workflow pair for an immutable product tag.

Each supported product tag owns its driver: the exact (source, automation)
workflow-file fingerprint pair that built it. v3.4.0 predates this table and
keeps its original pair. Adding a release means adding one audited row; any
unknown tag, unknown step, field, or command changes a fingerprint and fails
closed.
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

# Audited (source workflow, automation workflow) fingerprint pairs by product
# tag. The source workflow is the file committed at the product tag; the
# automation workflow is the file in the automation-tag tree that ran the
# build. Both trees are immutable and reviewed before tagging.
RECIPES = {
    "v3.4.0": {
        "source_workflow_sha256":
            "c0f707e5b4d51c4d1a36545c2f681a9a79cdb567dac1a97f4d8dad9dad639601",
        "automation_workflow_sha256":
            "4cbff406f667d099e3608c88c05262ae6b7dec144570f92a992630bafe36c74b",
    },
    "v3.4.6": {
        "source_workflow_sha256":
            "4cbff406f667d099e3608c88c05262ae6b7dec144570f92a992630bafe36c74b",
        "automation_workflow_sha256":
            "1cac12cb71accb97018d896cf260d9df5df61cecc99f6b50c676b6c05541adc2",
    },
}
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
        recipe = RECIPES.get(args.release_tag)
        if recipe is None:
            raise VerificationError
        for key in ("source_workflow_sha256",
                    "automation_workflow_sha256"):
            if not SHA256_RE.fullmatch(recipe[key]):
                raise VerificationError
        if fingerprint(args.source_workflow) != recipe["source_workflow_sha256"]:
            raise VerificationError
        if fingerprint(args.automation_workflow) != recipe["automation_workflow_sha256"]:
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
