#!/usr/bin/env python3

"""Fail-closed provenance checks for tag-based release builds.

The workflow uses this helper at three trust transitions:

* ``resolve`` binds an annotated release tag to an immutable tag object and
  peeled commit while the automation checkout is still the exact remote main.
* ``verify-source`` checks the separately checked-out source before and after
  submodule initialization.
* ``initialize-submodules`` validates each nested URL before initializing that
  depth, so an unsafe transport is never contacted first.
* ``recheck`` proves immediately before artifact upload that the remote tag
  still names the originally resolved objects.

Successful stdout is intentionally limited to validated ``KEY=value`` lines.
Failures use category-only stderr messages and never echo supplied values.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


TAG_RE = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
AUTOMATION_TAG_RE = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)-automation\.([1-9][0-9]*)$"
)
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REMOTE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,127}$")
PATH_RE = re.compile(r"^[A-Za-z0-9._/-]+$")
PUBLIC_GITHUB_URL_RE = re.compile(
    r"^https://github\.com/[A-Za-z0-9][A-Za-z0-9-]{0,38}/"
    r"[A-Za-z0-9._-]+(?:\.git)?$"
)
OUTPUT_KEY_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
MAX_VERSION_FILE_BYTES = 128 * 1024
MAX_SUBMODULES = 128
MAX_SUBMODULE_DEPTH = 16


class VerificationError(Exception):
    """A deliberately non-sensitive verification failure."""

    def __init__(self, category: str):
        super().__init__(category)
        self.category = category


class SafeArgumentParser(argparse.ArgumentParser):
    """Avoid reflecting malformed arguments into release logs."""

    def error(self, message: str) -> None:
        del message
        raise VerificationError("invalid-arguments")


@dataclass(frozen=True)
class SubmoduleRecord:
    path: str
    url: str
    sha: str


@dataclass(frozen=True)
class SubmoduleStatus:
    state: str
    sha: str


def run_git(
    repo: Path,
    args: Sequence[str],
    allowed_returncodes: Iterable[int] = (0,),
) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["GIT_TERMINAL_PROMPT"] = "0"
    env["LC_ALL"] = "C"
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=45,
            env=env,
        )
    except (OSError, subprocess.SubprocessError):
        raise VerificationError("git-unavailable")
    if result.returncode not in set(allowed_returncodes):
        raise VerificationError("git-command")
    return result


def decode_ascii(data: bytes, category: str) -> str:
    try:
        return data.decode("ascii")
    except UnicodeDecodeError:
        raise VerificationError(category)


def require_sha(value: str, category: str = "invalid-sha") -> str:
    if not SHA_RE.fullmatch(value):
        raise VerificationError(category)
    return value


def require_sha256(value: str, category: str = "invalid-sha256") -> str:
    if not SHA256_RE.fullmatch(value):
        raise VerificationError(category)
    return value


def parse_release_tag(value: str) -> Tuple[str, str, str, str]:
    if len(value) > 64:
        raise VerificationError("invalid-release-tag")
    match = TAG_RE.fullmatch(value)
    if match is None:
        raise VerificationError("invalid-release-tag")
    major, minor, patch = match.groups()
    return value, major, minor, patch


def parse_automation_tag(value: str) -> Tuple[str, str, str, str, str]:
    if len(value) > 96:
        raise VerificationError("invalid-automation-tag")
    match = AUTOMATION_TAG_RE.fullmatch(value)
    if match is None:
        raise VerificationError("invalid-automation-tag")
    major, minor, patch, sequence = match.groups()
    return value, major, minor, patch, sequence


def require_remote(value: str) -> str:
    if (
        not REMOTE_RE.fullmatch(value)
        or value.startswith("-")
        or ".." in PurePosixPath(value).parts
    ):
        raise VerificationError("invalid-remote")
    return value


def rev_parse(repo: Path, revision: str, category: str) -> str:
    result = run_git(repo, ["rev-parse", "--verify", revision])
    value = decode_ascii(result.stdout, category).strip()
    return require_sha(value, category)


def object_type(repo: Path, revision: str, category: str) -> str:
    result = run_git(repo, ["cat-file", "-t", revision])
    value = decode_ascii(result.stdout, category).strip()
    if value not in {"blob", "commit", "tag", "tree"}:
        raise VerificationError(category)
    return value


def require_worktree(repo: Path, category: str) -> None:
    result = run_git(repo, ["rev-parse", "--is-inside-work-tree"])
    if decode_ascii(result.stdout, category).strip() != "true":
        raise VerificationError(category)


def remote_refs(repo: Path, remote: str, refs: Sequence[str]) -> Dict[str, str]:
    require_remote(remote)
    result = run_git(repo, ["ls-remote", remote, *refs])
    output = decode_ascii(result.stdout, "remote-ref")
    found: Dict[str, str] = {}
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            raise VerificationError("remote-ref")
        sha, ref = fields
        require_sha(sha, "remote-ref")
        if ref not in refs or ref in found:
            raise VerificationError("remote-ref")
        found[ref] = sha
    return found


def resolve_local_annotated_tag(
    repo: Path, tag_name: str, category: str
) -> Tuple[str, str]:
    tag_ref = "refs/tags/" + tag_name
    if object_type(repo, tag_ref, category) != "tag":
        raise VerificationError(category)
    tag_object_sha = rev_parse(repo, tag_ref, category)
    peeled_sha = rev_parse(repo, tag_ref + "^{commit}", category)
    tag_name_result = run_git(
        repo, ["for-each-ref", "--format=%(tag)", "--count=1", tag_ref]
    )
    if decode_ascii(tag_name_result.stdout, category).strip() != tag_name:
        raise VerificationError(category)
    return tag_object_sha, peeled_sha


def resolve_remote_tag(
    repo: Path, remote: str, tag_name: str, category: str = "remote-annotated-tag"
) -> Tuple[str, str]:
    tag_ref = "refs/tags/" + tag_name
    peeled_ref = tag_ref + "^{}"
    refs = remote_refs(repo, remote, [tag_ref, peeled_ref])
    if set(refs) != {tag_ref, peeled_ref}:
        raise VerificationError(category)
    return refs[tag_ref], refs[peeled_ref]


def first_cmake_value(text: str, variable: str, category: str) -> str:
    pattern = re.compile(
        r"^[ \t]*set[ \t]*\([ \t]*"
        + re.escape(variable)
        + r"[ \t]+(?:\"([^\"\r\n]+)\"|([^\s\)]+))[ \t]*\)",
        re.MULTILINE,
    )
    match = pattern.search(text)
    if match is None:
        raise VerificationError(category)
    return match.group(1) if match.group(1) is not None else match.group(2)


def source_version(
    repo: Path, source_sha: str, expected_file_sha256: str
) -> Tuple[str, str, str]:
    expected_digest = require_sha256(
        expected_file_sha256, "source-version-digest"
    )
    result = run_git(repo, ["show", source_sha + ":cmake/Version.cmake"])
    if (
        not result.stdout
        or len(result.stdout) > MAX_VERSION_FILE_BYTES
        or hashlib.sha256(result.stdout).hexdigest() != expected_digest
    ):
        raise VerificationError("source-version-digest")
    try:
        text = result.stdout.decode("utf-8")
    except UnicodeDecodeError:
        raise VerificationError("source-version")
    major = first_cmake_value(text, "BARRIER_VERSION_MAJOR", "source-version")
    minor = first_cmake_value(text, "BARRIER_VERSION_MINOR", "source-version")
    patch = first_cmake_value(text, "BARRIER_VERSION_PATCH", "source-version")
    stage = first_cmake_value(text, "BARRIER_VERSION_STAGE", "source-version")
    for component in (major, minor, patch):
        if not re.fullmatch(r"0|[1-9][0-9]*", component):
            raise VerificationError("source-version")
    if stage != "release":
        raise VerificationError("source-version")
    return major, minor, patch


def require_clean(repo: Path, category: str) -> None:
    result = run_git(
        repo,
        [
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--ignore-submodules=all",
        ],
    )
    if result.stdout:
        raise VerificationError(category)


def validate_submodule_path(path: str) -> str:
    if len(path) > 512 or not PATH_RE.fullmatch(path) or "\\" in path:
        raise VerificationError("submodule-path")
    pure_path = PurePosixPath(path)
    if pure_path.is_absolute() or not pure_path.parts:
        raise VerificationError("submodule-path")
    if any(part in {"", ".", "..", ".git"} for part in pure_path.parts):
        raise VerificationError("submodule-path")
    if str(pure_path) != path:
        raise VerificationError("submodule-path")
    return path


def validate_submodule_url(url: str) -> str:
    if len(url) > 512 or not PUBLIC_GITHUB_URL_RE.fullmatch(url):
        raise VerificationError("submodule-url")
    return url


def gitlinks(repo: Path) -> Dict[str, str]:
    result = run_git(repo, ["ls-tree", "-r", "-z", "HEAD"])
    links: Dict[str, str] = {}
    for raw_entry in result.stdout.split(b"\0"):
        if not raw_entry:
            continue
        header, separator, raw_path = raw_entry.partition(b"\t")
        if not separator:
            raise VerificationError("submodule-metadata")
        fields = header.split(b" ")
        if len(fields) != 3 or fields[0] != b"160000":
            continue
        if fields[1] != b"commit":
            raise VerificationError("submodule-metadata")
        try:
            path = raw_path.decode("utf-8")
            sha = fields[2].decode("ascii")
        except UnicodeDecodeError:
            raise VerificationError("submodule-metadata")
        validate_submodule_path(path)
        require_sha(sha, "submodule-metadata")
        if path in links:
            raise VerificationError("submodule-metadata")
        links[path] = sha
    return links


def has_gitmodules(repo: Path) -> bool:
    result = run_git(repo, ["ls-tree", "-z", "HEAD", "--", ".gitmodules"])
    if not result.stdout:
        return False
    entries = [entry for entry in result.stdout.split(b"\0") if entry]
    if len(entries) != 1 or not entries[0].startswith(b"100644 blob "):
        raise VerificationError("submodule-metadata")
    return True


def gitmodule_fields(repo: Path) -> Dict[str, Dict[str, str]]:
    if not has_gitmodules(repo):
        return {}
    result = run_git(
        repo,
        [
            "config",
            "-z",
            "--blob",
            "HEAD:.gitmodules",
            "--get-regexp",
            r"^submodule\..*\.(path|url)$",
        ],
        allowed_returncodes=(0, 1),
    )
    fields: Dict[str, Dict[str, str]] = {}
    for raw_record in result.stdout.split(b"\0"):
        if not raw_record:
            continue
        raw_key, separator, raw_value = raw_record.partition(b"\n")
        if not separator:
            raise VerificationError("submodule-metadata")
        try:
            key = raw_key.decode("utf-8")
            value = raw_value.decode("utf-8")
        except UnicodeDecodeError:
            raise VerificationError("submodule-metadata")
        if not key.startswith("submodule."):
            raise VerificationError("submodule-metadata")
        if key.endswith(".path"):
            name = key[len("submodule.") : -len(".path")]
            field = "path"
        elif key.endswith(".url"):
            name = key[len("submodule.") : -len(".url")]
            field = "url"
        else:
            raise VerificationError("submodule-metadata")
        if not name or field in fields.setdefault(name, {}):
            raise VerificationError("submodule-metadata")
        fields[name][field] = value
    return fields


def direct_submodules(repo: Path) -> List[SubmoduleRecord]:
    links = gitlinks(repo)
    fields = gitmodule_fields(repo)
    records: List[SubmoduleRecord] = []
    seen_paths = set()
    for values in fields.values():
        if set(values) != {"path", "url"}:
            raise VerificationError("submodule-metadata")
        path = validate_submodule_path(values["path"])
        url = validate_submodule_url(values["url"])
        if path in seen_paths or path not in links:
            raise VerificationError("submodule-metadata")
        seen_paths.add(path)
        records.append(SubmoduleRecord(path=path, url=url, sha=links[path]))
    if seen_paths != set(links):
        raise VerificationError("submodule-metadata")
    return sorted(records, key=lambda record: record.path)


def recursive_status(repo: Path) -> Dict[str, SubmoduleStatus]:
    result = run_git(
        repo,
        ["-c", "core.quotePath=false", "submodule", "status", "--recursive"],
    )
    statuses: Dict[str, SubmoduleStatus] = {}
    text = decode_ascii(result.stdout, "submodule-state")
    for line in text.splitlines():
        if len(line) < 42:
            raise VerificationError("submodule-state")
        state = line[0]
        remainder = line[1:]
        sha, separator, path_and_description = remainder.partition(" ")
        if not separator:
            raise VerificationError("submodule-state")
        path = path_and_description.split(" ", 1)[0]
        require_sha(sha, "submodule-state")
        validate_submodule_path(path)
        if state not in {" ", "-", "+", "U"} or path in statuses:
            raise VerificationError("submodule-state")
        statuses[path] = SubmoduleStatus(state=state, sha=sha)
    return statuses


def safe_child_directory(source_root: Path, relative_path: str) -> Path:
    candidate = source_root.joinpath(*PurePosixPath(relative_path).parts)
    if candidate.is_symlink() or not candidate.is_dir():
        raise VerificationError("submodule-state")
    try:
        candidate.resolve().relative_to(source_root.resolve())
    except (OSError, ValueError):
        raise VerificationError("submodule-path")
    return candidate


def verify_recursive_submodules(
    source_root: Path, top_level: Sequence[SubmoduleRecord]
) -> List[SubmoduleRecord]:
    statuses = recursive_status(source_root)
    visited = set()
    resolved_records: List[SubmoduleRecord] = []

    def visit(parent: Path, prefix: str, records: Sequence[SubmoduleRecord]) -> None:
        for record in records:
            full_path = record.path if not prefix else prefix + "/" + record.path
            validate_submodule_path(full_path)
            status = statuses.get(full_path)
            if status is None or status.state != " " or status.sha != record.sha:
                raise VerificationError("submodule-state")
            child = safe_child_directory(source_root, full_path)
            require_worktree(child, "submodule-state")
            if rev_parse(child, "HEAD^{commit}", "submodule-state") != record.sha:
                raise VerificationError("submodule-state")
            require_clean(child, "submodule-dirty")
            child_records = direct_submodules(child)
            visited.add(full_path)
            resolved_records.append(
                SubmoduleRecord(path=full_path, url=record.url, sha=record.sha)
            )
            visit(child, full_path, child_records)

    visit(source_root, "", top_level)
    if visited != set(statuses):
        raise VerificationError("submodule-state")
    return sorted(resolved_records, key=lambda record: record.path)


def initialize_recursive_submodules(
    source_root: Path, top_level: Sequence[SubmoduleRecord]
) -> List[SubmoduleRecord]:
    """Validate each depth before allowing Git to contact the next one."""

    visited = set()
    resolved_records: List[SubmoduleRecord] = []

    def visit(
        parent: Path,
        prefix: str,
        records: Sequence[SubmoduleRecord],
        depth: int,
    ) -> None:
        if depth > MAX_SUBMODULE_DEPTH:
            raise VerificationError("submodule-limit")
        for record in records:
            if len(visited) >= MAX_SUBMODULES:
                raise VerificationError("submodule-limit")
            full_path = record.path if not prefix else prefix + "/" + record.path
            validate_submodule_path(full_path)
            if full_path in visited:
                raise VerificationError("submodule-state")

            # Recursion is explicitly disabled and ``--recursive`` is omitted:
            # only the URL direct_submodules() already validated is contacted.
            run_git(
                parent,
                [
                    "-c",
                    "submodule.recurse=false",
                    "submodule",
                    "update",
                    "--init",
                    "--checkout",
                    "--",
                    record.path,
                ],
            )
            child = safe_child_directory(source_root, full_path)
            require_worktree(child, "submodule-state")
            if rev_parse(child, "HEAD^{commit}", "submodule-state") != record.sha:
                raise VerificationError("submodule-state")
            require_clean(child, "submodule-dirty")

            child_records = direct_submodules(child)
            visited.add(full_path)
            resolved_records.append(
                SubmoduleRecord(path=full_path, url=record.url, sha=record.sha)
            )
            visit(child, full_path, child_records, depth + 1)

    visit(source_root, "", top_level, 1)
    statuses = recursive_status(source_root)
    if visited != set(statuses):
        raise VerificationError("submodule-state")
    expected = {record.path: record.sha for record in resolved_records}
    for path, status in statuses.items():
        if status.state != " " or status.sha != expected[path]:
            raise VerificationError("submodule-state")
    return sorted(resolved_records, key=lambda record: record.path)


def output_pairs(
    source_sha: str,
    phase: str,
    records: Sequence[SubmoduleRecord],
) -> List[Tuple[str, str]]:
    pairs: List[Tuple[str, str]] = [
        ("SOURCE_SHA", source_sha),
        ("SOURCE_PHASE", phase),
        ("SUBMODULE_COUNT", str(len(records))),
    ]
    for index, record in enumerate(records):
        prefix = "SUBMODULE_{:03d}_".format(index)
        pairs.extend(
            [
                (prefix + "PATH", record.path),
                (prefix + "URL", record.url),
                (prefix + "SHA", record.sha),
            ]
        )
    return pairs


def emit(pairs: Sequence[Tuple[str, str]]) -> None:
    seen = set()
    lines: List[str] = []
    for key, value in pairs:
        if not OUTPUT_KEY_RE.fullmatch(key) or key in seen:
            raise VerificationError("unsafe-output")
        if (
            not value
            or any(ord(character) < 33 or ord(character) > 126 for character in value)
            or "=" in value
        ):
            raise VerificationError("unsafe-output")
        seen.add(key)
        lines.append(key + "=" + value)
    sys.stdout.write("\n".join(lines) + "\n")


def command_resolve(args: argparse.Namespace) -> None:
    release_tag, tag_major, tag_minor, tag_patch = parse_release_tag(
        args.release_tag
    )
    (
        automation_tag,
        automation_major,
        automation_minor,
        automation_patch,
        _,
    ) = parse_automation_tag(args.automation_tag)
    automation_sha = require_sha(args.automation_sha)
    repo = Path(args.repo)
    require_worktree(repo, "automation-checkout")
    if rev_parse(repo, "HEAD^{commit}", "automation-checkout") != automation_sha:
        raise VerificationError("automation-checkout")
    if object_type(repo, automation_sha, "automation-commit") != "commit":
        raise VerificationError("automation-commit")

    remote = require_remote(args.remote)
    main_ref = "refs/heads/main"
    main_refs = remote_refs(repo, remote, [main_ref])
    remote_main_sha = main_refs.get(main_ref)
    if remote_main_sha is None:
        raise VerificationError("automation-main")
    main_ancestry = run_git(
        repo,
        ["merge-base", "--is-ancestor", automation_sha, remote_main_sha],
        allowed_returncodes=(0, 1),
    )
    if main_ancestry.returncode != 0:
        raise VerificationError("automation-main")

    tag_object_sha, source_sha = resolve_local_annotated_tag(
        repo, release_tag, "local-annotated-tag"
    )
    remote_tag_object, remote_source = resolve_remote_tag(repo, remote, release_tag)
    if remote_tag_object != tag_object_sha or remote_source != source_sha:
        raise VerificationError("remote-tag-mismatch")

    if source_version(
        repo, source_sha, args.version_file_sha256
    ) != (tag_major, tag_minor, tag_patch):
        raise VerificationError("source-version-mismatch")
    ancestry = run_git(
        repo,
        ["merge-base", "--is-ancestor", source_sha, automation_sha],
        allowed_returncodes=(0, 1),
    )
    if ancestry.returncode != 0:
        raise VerificationError("source-ancestry")

    if (automation_major, automation_minor, automation_patch) != (
        tag_major,
        tag_minor,
        tag_patch,
    ):
        raise VerificationError("automation-tag-version")
    automation_tag_object_sha, automation_tag_commit = (
        resolve_local_annotated_tag(
            repo, automation_tag, "local-automation-annotated-tag"
        )
    )
    if automation_tag_commit != automation_sha:
        raise VerificationError("local-automation-tag-mismatch")
    remote_automation_object, remote_automation_commit = resolve_remote_tag(
        repo,
        remote,
        automation_tag,
        "remote-automation-annotated-tag",
    )
    if (
        remote_automation_object != automation_tag_object_sha
        or remote_automation_commit != automation_sha
    ):
        raise VerificationError("remote-automation-tag-mismatch")

    emit(
        [
            ("RELEASE_TAG", release_tag),
            ("RELEASE_VERSION", ".".join((tag_major, tag_minor, tag_patch))),
            ("TAG_OBJECT_SHA", tag_object_sha),
            ("SOURCE_SHA", source_sha),
            ("AUTOMATION_SHA", automation_sha),
            ("AUTOMATION_TAG", automation_tag),
            ("AUTOMATION_TAG_OBJECT_SHA", automation_tag_object_sha),
        ]
    )


def command_verify_source(args: argparse.Namespace) -> None:
    source_sha = require_sha(args.source_sha)
    source_dir = Path(args.source_dir)
    require_worktree(source_dir, "source-checkout")
    if rev_parse(source_dir, "HEAD^{commit}", "source-checkout") != source_sha:
        raise VerificationError("source-head")
    require_clean(source_dir, "source-dirty")
    top_level = direct_submodules(source_dir)
    if args.phase == "pre-init":
        records = top_level
    elif args.phase == "post-init":
        records = verify_recursive_submodules(source_dir, top_level)
    else:
        raise VerificationError("invalid-arguments")
    emit(output_pairs(source_sha, args.phase, records))


def command_initialize_submodules(args: argparse.Namespace) -> None:
    source_sha = require_sha(args.source_sha)
    source_dir = Path(args.source_dir)
    require_worktree(source_dir, "source-checkout")
    if rev_parse(source_dir, "HEAD^{commit}", "source-checkout") != source_sha:
        raise VerificationError("source-head")
    require_clean(source_dir, "source-dirty")
    records = initialize_recursive_submodules(
        source_dir, direct_submodules(source_dir)
    )
    require_clean(source_dir, "source-dirty")
    emit(output_pairs(source_sha, "initialized", records))


def command_recheck(args: argparse.Namespace) -> None:
    release_tag, _, _, _ = parse_release_tag(args.release_tag)
    automation_tag, _, _, _, _ = parse_automation_tag(args.automation_tag)
    tag_object_sha = require_sha(args.tag_object_sha)
    source_sha = require_sha(args.source_sha)
    automation_sha = require_sha(args.automation_sha)
    automation_tag_object_sha = require_sha(args.automation_tag_object_sha)
    repo = Path(args.repo)
    require_worktree(repo, "automation-checkout")
    if rev_parse(repo, "HEAD^{commit}", "automation-checkout") != automation_sha:
        raise VerificationError("automation-checkout")
    remote = require_remote(args.remote)
    remote_tag_object, remote_source = resolve_remote_tag(
        repo, remote, release_tag
    )
    if remote_tag_object != tag_object_sha or remote_source != source_sha:
        raise VerificationError("remote-tag-changed")
    remote_automation_object, remote_automation_commit = resolve_remote_tag(
        repo,
        remote,
        automation_tag,
        "remote-automation-annotated-tag",
    )
    if (
        remote_automation_object != automation_tag_object_sha
        or remote_automation_commit != automation_sha
    ):
        raise VerificationError("remote-automation-tag-changed")
    main_ref = "refs/heads/main"
    main_refs = remote_refs(repo, remote, [main_ref])
    remote_main_sha = main_refs.get(main_ref)
    if remote_main_sha is None:
        raise VerificationError("automation-main")
    main_ancestry = run_git(
        repo,
        ["merge-base", "--is-ancestor", automation_sha, remote_main_sha],
        allowed_returncodes=(0, 1),
    )
    if main_ancestry.returncode != 0:
        raise VerificationError("automation-main")
    emit(
        [
            ("RELEASE_TAG", release_tag),
            ("TAG_OBJECT_SHA", tag_object_sha),
            ("SOURCE_SHA", source_sha),
            ("REMOTE_TAG_STABLE", "1"),
            ("AUTOMATION_TAG", automation_tag),
            ("AUTOMATION_TAG_OBJECT_SHA", automation_tag_object_sha),
            ("AUTOMATION_SHA", automation_sha),
            ("REMOTE_AUTOMATION_TAG_STABLE", "1"),
        ]
    )


def build_parser() -> argparse.ArgumentParser:
    parser = SafeArgumentParser(
        description="Verify immutable release-tag and source provenance."
    )
    subparsers = parser.add_subparsers(
        dest="command", required=True, parser_class=SafeArgumentParser
    )

    resolve = subparsers.add_parser(
        "resolve",
        help="bind an annotated remote tag to a source commit",
    )
    resolve.add_argument("--repo", required=True, help="automation checkout")
    resolve.add_argument("--remote", default="origin", help="safe Git remote name")
    resolve.add_argument("--release-tag", required=True, help="vMAJOR.MINOR.PATCH")
    resolve.add_argument(
        "--automation-tag",
        required=True,
        help="vMAJOR.MINOR.PATCH-automation.N",
    )
    resolve.add_argument("--automation-sha", required=True, help="exact main SHA")
    resolve.add_argument(
        "--version-file-sha256",
        required=True,
        help="reviewed cmake/Version.cmake SHA-256",
    )
    resolve.set_defaults(handler=command_resolve)

    verify_source = subparsers.add_parser(
        "verify-source",
        help="verify source checkout and submodule provenance",
    )
    verify_source.add_argument("--source-dir", required=True, help="source checkout")
    verify_source.add_argument("--source-sha", required=True, help="peeled tag SHA")
    verify_source.add_argument(
        "--phase", required=True, choices=("pre-init", "post-init")
    )
    verify_source.set_defaults(handler=command_verify_source)

    initialize_submodules = subparsers.add_parser(
        "initialize-submodules",
        help="validate and initialize public submodules one depth at a time",
    )
    initialize_submodules.add_argument(
        "--source-dir", required=True, help="source checkout"
    )
    initialize_submodules.add_argument(
        "--source-sha", required=True, help="peeled tag SHA"
    )
    initialize_submodules.set_defaults(handler=command_initialize_submodules)

    recheck = subparsers.add_parser(
        "recheck",
        help="recheck remote tag objects immediately before upload",
    )
    recheck.add_argument("--repo", required=True, help="automation checkout")
    recheck.add_argument("--remote", default="origin", help="safe Git remote name")
    recheck.add_argument("--release-tag", required=True, help="vMAJOR.MINOR.PATCH")
    recheck.add_argument("--tag-object-sha", required=True, help="captured tag object")
    recheck.add_argument("--source-sha", required=True, help="captured peeled commit")
    recheck.add_argument(
        "--automation-tag",
        required=True,
        help="captured immutable automation tag",
    )
    recheck.add_argument(
        "--automation-tag-object-sha",
        required=True,
        help="captured automation tag object",
    )
    recheck.add_argument(
        "--automation-sha", required=True, help="captured automation commit"
    )
    recheck.set_defaults(handler=command_recheck)
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        args.handler(args)
        return 0
    except VerificationError as error:
        print("verify-release-tag: " + error.category, file=sys.stderr)
        return 1
    except Exception:
        print("verify-release-tag: internal-error", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
