#!/usr/bin/env python3

"""Parse validated release provenance into canonical GitHub command files."""

import argparse
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


MAX_INPUT_BYTES = 8192
REQUIRED_KEYS = (
    "RELEASE_TAG",
    "RELEASE_VERSION",
    "TAG_OBJECT_SHA",
    "SOURCE_SHA",
    "AUTOMATION_SHA",
    "AUTOMATION_TAG",
    "AUTOMATION_TAG_OBJECT_SHA",
)
SEMVER_COMPONENT = r"(0|[1-9][0-9]*)"
RELEASE_TAG_RE = re.compile(
    rf"^v{SEMVER_COMPONENT}\.{SEMVER_COMPONENT}\.{SEMVER_COMPONENT}$"
)
RELEASE_VERSION_RE = re.compile(
    rf"^{SEMVER_COMPONENT}\.{SEMVER_COMPONENT}\.{SEMVER_COMPONENT}$"
)
AUTOMATION_TAG_RE = re.compile(
    rf"^v{SEMVER_COMPONENT}\.{SEMVER_COMPONENT}\.{SEMVER_COMPONENT}"
    r"-automation\.([1-9][0-9]*)$"
)
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
TEMP_PREFIX = ".release-provenance."


class ProvenanceError(Exception):
    """A category-only failure safe to expose in release logs."""

    def __init__(self, category: str):
        super().__init__(category)
        self.category = category


class SafeArgumentParser(argparse.ArgumentParser):
    """Do not reflect malformed path or value arguments into logs."""

    def error(self, message: str) -> None:
        del message
        raise ProvenanceError("invalid-arguments")


@dataclass(frozen=True)
class OutputPlan:
    target: Path
    canonical_target: str
    payload: bytes


@dataclass(frozen=True)
class StagedOutput:
    plan: OutputPlan
    temporary: Path
    device: int
    inode: int


def read_bounded_ascii_file(path_value: str) -> str:
    path = Path(path_value)
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError:
        raise ProvenanceError("input-unavailable")

    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise ProvenanceError("input-unavailable")
        with os.fdopen(descriptor, "rb", closefd=True) as handle:
            descriptor = -1
            data = handle.read(MAX_INPUT_BYTES + 1)
    except ProvenanceError:
        raise
    except OSError:
        raise ProvenanceError("input-unavailable")
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass

    if not data or len(data) > MAX_INPUT_BYTES:
        raise ProvenanceError("invalid-input-size")
    if any(byte > 0x7E or (byte < 0x20 and byte != 0x0A) for byte in data):
        raise ProvenanceError("invalid-input-character")
    try:
        return data.decode("ascii")
    except UnicodeDecodeError:
        raise ProvenanceError("invalid-input-character")


def parse_records(text: str) -> Dict[str, str]:
    if not text.endswith("\n"):
        raise ProvenanceError("invalid-input-record")
    lines = text[:-1].split("\n")
    if not lines or any(not line for line in lines):
        raise ProvenanceError("invalid-input-record")

    records: Dict[str, str] = {}
    required = set(REQUIRED_KEYS)
    for line in lines:
        key, separator, value = line.partition("=")
        if not separator or not key or not value or "=" in value:
            raise ProvenanceError("invalid-input-record")
        if key not in required:
            raise ProvenanceError("unexpected-key")
        if key in records:
            raise ProvenanceError("duplicate-key")
        records[key] = value

    if set(records) != required:
        raise ProvenanceError("missing-key")
    return records


def semver(groups: Sequence[str]) -> str:
    return ".".join(groups)


def validate_records(records: Dict[str, str]) -> None:
    release_tag = RELEASE_TAG_RE.fullmatch(records["RELEASE_TAG"])
    release_version = RELEASE_VERSION_RE.fullmatch(records["RELEASE_VERSION"])
    automation_tag = AUTOMATION_TAG_RE.fullmatch(records["AUTOMATION_TAG"])
    if release_tag is None or release_version is None or automation_tag is None:
        raise ProvenanceError("invalid-value")

    tagged_version = semver(release_tag.groups())
    if records["RELEASE_VERSION"] != tagged_version:
        raise ProvenanceError("version-mismatch")
    if semver(automation_tag.groups()[:3]) != tagged_version:
        raise ProvenanceError("automation-version-mismatch")

    for key in (
        "TAG_OBJECT_SHA",
        "SOURCE_SHA",
        "AUTOMATION_SHA",
        "AUTOMATION_TAG_OBJECT_SHA",
    ):
        if SHA_RE.fullmatch(records[key]) is None:
            raise ProvenanceError("invalid-value")


def canonical_payloads(records: Dict[str, str]) -> Tuple[bytes, bytes]:
    environment_lines = [f"{key}={records[key]}" for key in REQUIRED_KEYS]
    step_lines = [f"{key.lower()}={records[key]}" for key in REQUIRED_KEYS]
    environment = ("\n".join(environment_lines) + "\n").encode("ascii")
    step_output = ("\n".join(step_lines) + "\n").encode("ascii")
    return environment, step_output


def output_plan(path_value: str, payload: bytes) -> OutputPlan:
    if not path_value or any(
        ord(character) < 0x20 or ord(character) > 0x7E
        for character in path_value
    ):
        raise ProvenanceError("unsafe-output-path")
    path = Path(path_value)
    if path.name in {"", ".", ".."}:
        raise ProvenanceError("unsafe-output-path")
    parent = path.parent
    try:
        parent_metadata = parent.stat()
    except OSError:
        raise ProvenanceError("unsafe-output-path")
    if not stat.S_ISDIR(parent_metadata.st_mode):
        raise ProvenanceError("unsafe-output-path")
    try:
        os.lstat(path)
    except FileNotFoundError:
        pass
    except OSError:
        raise ProvenanceError("unsafe-output-path")
    else:
        raise ProvenanceError("output-exists")

    canonical_target = os.path.join(os.path.realpath(parent), path.name)
    return OutputPlan(
        target=path,
        canonical_target=canonical_target,
        payload=payload,
    )


def unlink_if_present(path: Path) -> bool:
    try:
        os.unlink(path)
    except FileNotFoundError:
        return True
    except OSError:
        return False
    return True


def stage_output(plan: OutputPlan) -> StagedOutput:
    descriptor = -1
    temporary_name = ""
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=TEMP_PREFIX,
            dir=str(plan.target.parent),
        )
        with os.fdopen(descriptor, "wb", closefd=True) as handle:
            descriptor = -1
            handle.write(plan.payload)
            handle.flush()
            os.fsync(handle.fileno())
        metadata = os.stat(temporary_name, follow_symlinks=False)
        if not stat.S_ISREG(metadata.st_mode):
            raise ProvenanceError("output-write")
        return StagedOutput(
            plan=plan,
            temporary=Path(temporary_name),
            device=metadata.st_dev,
            inode=metadata.st_ino,
        )
    except (OSError, ProvenanceError) as error:
        cleanup_succeeded = True
        if temporary_name:
            cleanup_succeeded = unlink_if_present(Path(temporary_name))
        if not cleanup_succeeded:
            raise ProvenanceError("output-rollback")
        if isinstance(error, ProvenanceError):
            raise
        raise ProvenanceError("output-write")
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass


def cleanup_staged(staged: Sequence[StagedOutput]) -> bool:
    succeeded = True
    for item in staged:
        if not unlink_if_present(item.temporary):
            succeeded = False
    return succeeded


def rollback_published(published: Sequence[StagedOutput]) -> bool:
    succeeded = True
    for item in reversed(published):
        try:
            metadata = os.lstat(item.plan.target)
        except FileNotFoundError:
            continue
        except OSError:
            succeeded = False
            continue
        if metadata.st_dev != item.device or metadata.st_ino != item.inode:
            succeeded = False
            continue
        if not unlink_if_present(item.plan.target):
            succeeded = False
    return succeeded


def publish_outputs(plans: Sequence[OutputPlan]) -> None:
    staged: List[StagedOutput] = []
    published: List[StagedOutput] = []
    try:
        for plan in plans:
            staged.append(stage_output(plan))
        for item in staged:
            try:
                os.link(item.temporary, item.plan.target, follow_symlinks=False)
            except FileExistsError:
                raise ProvenanceError("output-exists")
            except OSError:
                raise ProvenanceError("output-write")
            published.append(item)
    except ProvenanceError:
        rollback_succeeded = rollback_published(published)
        cleanup_succeeded = cleanup_staged(staged)
        if not rollback_succeeded or not cleanup_succeeded:
            raise ProvenanceError("output-rollback")
        raise

    if not cleanup_staged(staged):
        if not rollback_published(published):
            raise ProvenanceError("output-rollback")
        raise ProvenanceError("output-write")


def build_parser() -> argparse.ArgumentParser:
    parser = SafeArgumentParser(
        description="Create canonical GitHub release-provenance command files."
    )
    parser.add_argument("--input", required=True, help="validated provenance input")
    parser.add_argument("--github-env", required=True, help="new GitHub environment file")
    parser.add_argument("--github-output", required=True, help="new step output file")
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        records = parse_records(read_bounded_ascii_file(args.input))
        validate_records(records)
        environment, step_output = canonical_payloads(records)
        environment_plan = output_plan(args.github_env, environment)
        step_plan = output_plan(args.github_output, step_output)
        if environment_plan.canonical_target == step_plan.canonical_target:
            raise ProvenanceError("unsafe-output-path")
        publish_outputs((environment_plan, step_plan))
        return 0
    except ProvenanceError as error:
        print("parse-release-provenance: " + error.category, file=sys.stderr)
        return 1
    except Exception:
        print("parse-release-provenance: internal-error", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
