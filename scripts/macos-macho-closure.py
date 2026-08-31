#!/usr/bin/env python3
"""Fail-closed Mach-O dependency-closure audit for a macOS app bundle.

The verifier deliberately parses thin arm64 Mach-O load commands itself.  Its
output is a small, deterministic TSV fragment for the release manifest; it is
silent on success and never includes host-absolute paths in diagnostics.
"""

import argparse
import os
import posixpath
import stat
import struct
import sys
import tempfile
import unicodedata
from dataclasses import dataclass
from typing import Dict, FrozenSet, Iterable, List, Optional, Set, Tuple


MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C

MH_EXECUTE = 0x2
MH_DYLIB = 0x6
MH_BUNDLE = 0x8

LC_LOAD_DYLIB = 0xC
LC_ID_DYLIB = 0xD
LC_LOADFVMLIB = 0x6
LC_IDFVMLIB = 0x7
LC_LOAD_DYLINKER = 0xE
LC_PREBOUND_DYLIB = 0x10
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_LAZY_LOAD_DYLIB = 0x20
LC_RPATH = 0x8000001C
LC_REEXPORT_DYLIB = 0x8000001F
LC_DYLD_INFO_ONLY = 0x80000022
LC_LOAD_UPWARD_DYLIB = 0x80000023
LC_DYLD_ENVIRONMENT = 0x27
LC_MAIN = 0x80000028
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_REQ_DYLD = 0x80000000

DYLIB_USE_MARKER = 0x1A741800
DYLIB_USE_WEAK_LINK = 0x01
DYLIB_USE_REEXPORT = 0x02
DYLIB_USE_UPWARD = 0x04
DYLIB_USE_DELAYED_INIT = 0x08
DYLIB_USE_KNOWN_FLAGS = (
    DYLIB_USE_WEAK_LINK
    | DYLIB_USE_REEXPORT
    | DYLIB_USE_UPWARD
    | DYLIB_USE_DELAYED_INIT
)

THIN_64_LE_MAGIC = b"\xcf\xfa\xed\xfe"
OTHER_MACHO_MAGICS = {
    b"\xfe\xed\xfa\xcf",  # 64-bit big endian
    b"\xce\xfa\xed\xfe",  # 32-bit little endian
    b"\xfe\xed\xfa\xce",  # 32-bit big endian
    b"\xca\xfe\xba\xbe",  # fat
    b"\xbe\xba\xfe\xca",  # fat, byte swapped
    b"\xca\xfe\xba\xbf",  # fat64
    b"\xbf\xba\xfe\xca",  # fat64, byte swapped
}

MAX_CONTEXT_STATES = 250000


class AuditError(Exception):
    """An expected failure whose message is safe for a public build log."""


class ContextResolutionError(AuditError):
    """A dependency failure that may resolve in another dyld caller context."""


@dataclass(frozen=True)
class Dependency:
    command_ordinal: int
    command_kind: str
    path: str
    weak: bool
    reexport: bool
    upward: bool
    delayed_init: bool


@dataclass(frozen=True)
class Image:
    real_path: str
    relative_path: str
    file_type: int
    runpaths: Tuple[str, ...]
    dependencies: Tuple[Dependency, ...]
    install_id: Optional[str]


def safe_field(value: str) -> bool:
    try:
        value.encode("utf-8", "strict")
    except UnicodeError:
        return False
    return bool(value) and all(
        unicodedata.category(character) not in ("Cc", "Cf", "Cs", "Zl", "Zp")
        for character in value
    )


def checked_display(relative_path: str) -> str:
    if not safe_field(relative_path) or relative_path.startswith("/"):
        raise AuditError("unsafe bundle path")
    return relative_path


def malformed_load_commands(relative_path: str) -> AuditError:
    return AuditError("malformed Mach-O load commands: {}".format(
        checked_display(relative_path)))


def read_command_string(command: bytes, string_offset: int, minimum_offset: int,
                        relative_path: str) -> str:
    if string_offset < minimum_offset or string_offset >= len(command):
        raise malformed_load_commands(relative_path)
    terminator = command.find(b"\0", string_offset)
    if terminator < 0 or terminator == string_offset:
        raise malformed_load_commands(relative_path)
    try:
        value = command[string_offset:terminator].decode("utf-8", "strict")
    except UnicodeDecodeError:
        raise malformed_load_commands(relative_path)
    if not safe_field(value):
        raise malformed_load_commands(relative_path)
    return value


def validate_component_path(value: str, relative_path: str,
                            description: str, allow_parent: bool = False) -> None:
    if not safe_field(value) or "\\" in value or value.startswith("/"):
        raise AuditError("malformed {}: {}".format(
            description, checked_display(relative_path)))
    components = value.split("/")
    for component in components:
        if not component or component == "." or (component == ".." and not allow_parent):
            raise AuditError("malformed {}: {}".format(
                description, checked_display(relative_path)))


def validate_install_id(install_id: str, relative_path: str) -> None:
    display = checked_display(relative_path)
    if install_id.startswith("/"):
        raise AuditError("unsafe install identity: {}".format(display))
    for prefix in ("@rpath/", "@loader_path/", "@executable_path/"):
        if install_id.startswith(prefix):
            validate_component_path(install_id[len(prefix):], relative_path,
                                    "install identity", allow_parent=True)
            return
    if install_id.startswith("@") or "/" in install_id:
        raise AuditError("malformed install identity: {}".format(display))
    validate_component_path(install_id, relative_path, "install identity")
    if not install_id.endswith(".dylib"):
        raise AuditError("malformed install identity: {}".format(display))


def validate_data_range(command: bytes, offset: int, file_size: int,
                        relative_path: str) -> None:
    data_offset, data_size = struct.unpack_from("<II", command, offset)
    if data_offset > file_size or data_size > file_size - data_offset:
        raise malformed_load_commands(relative_path)


def parse_target_major(target: str) -> int:
    parts = target.split(".")
    if not parts or any(not part or not part.isdigit() for part in parts):
        raise AuditError("invalid macOS closure target")
    return int(parts[0])


def parse_macho(path: str, relative_path: str, target_major: int) -> Image:
    display = checked_display(relative_path)
    try:
        with open(path, "rb") as macho_file:
            header = macho_file.read(32)
            if len(header) != 32:
                raise malformed_load_commands(relative_path)
            magic, cpu_type, _cpu_subtype, file_type, command_count, command_bytes, \
                _flags, _reserved = struct.unpack("<IIIIIIII", header)
            if magic != MH_MAGIC_64:
                raise AuditError("unsupported Mach-O format: {}".format(display))
            if cpu_type != CPU_TYPE_ARM64:
                raise AuditError("unexpected Mach-O architecture: {}".format(display))
            if file_type not in (MH_EXECUTE, MH_DYLIB, MH_BUNDLE):
                raise AuditError("unsupported Mach-O image type: {}".format(display))
            file_size = os.fstat(macho_file.fileno()).st_size
            if command_bytes > file_size - 32 or command_count > command_bytes // 8:
                raise malformed_load_commands(relative_path)
            commands = macho_file.read(command_bytes)
            if len(commands) != command_bytes:
                raise malformed_load_commands(relative_path)
    except AuditError:
        raise
    except OSError:
        raise AuditError("unable to inspect Mach-O image: {}".format(display))

    runpaths: List[str] = []
    dependencies: List[Dependency] = []
    install_ids: List[str] = []
    dynamic_linkers: List[str] = []
    main_command_count = 0
    singleton_required_commands: Set[int] = set()
    offset = 0

    for _command_index in range(command_count):
        if offset + 8 > len(commands):
            raise malformed_load_commands(relative_path)
        command_type, command_size = struct.unpack_from("<II", commands, offset)
        if command_size < 8 or command_size % 8 != 0 or \
                offset + command_size > len(commands):
            raise malformed_load_commands(relative_path)
        command = commands[offset:offset + command_size]

        if command_type in (LC_LOADFVMLIB, LC_IDFVMLIB,
                            LC_DYLD_ENVIRONMENT):
            raise AuditError("unsupported Mach-O loader command: {}".format(display))
        if command_type == LC_LOAD_DYLINKER:
            if command_size < 12:
                raise malformed_load_commands(relative_path)
            string_offset = struct.unpack_from("<I", command, 8)[0]
            dynamic_linkers.append(read_command_string(
                command, string_offset, 12, relative_path))
        elif command_type == LC_RPATH:
            if command_size < 12:
                raise malformed_load_commands(relative_path)
            string_offset = struct.unpack_from("<I", command, 8)[0]
            runpaths.append(read_command_string(command, string_offset, 12,
                                                relative_path))
        elif command_type == LC_ID_DYLIB:
            if command_size < 24:
                raise malformed_load_commands(relative_path)
            string_offset = struct.unpack_from("<I", command, 8)[0]
            install_ids.append(read_command_string(command, string_offset, 24,
                                                   relative_path))
        elif command_type in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB,
                              LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB):
            weak = command_type == LC_LOAD_WEAK_DYLIB
            reexport = command_type == LC_REEXPORT_DYLIB
            upward = command_type == LC_LOAD_UPWARD_DYLIB
            delayed_init = False
            alternate = False
            if command_type in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB) and \
                    command_size >= 16 and \
                    struct.unpack_from("<I", command, 12)[0] == DYLIB_USE_MARKER:
                alternate = True

            if alternate:
                if target_major < 15:
                    raise AuditError(
                        "unsupported Mach-O dependency command: {}".format(display))
                if command_size < 28:
                    raise malformed_load_commands(relative_path)
                string_offset = struct.unpack_from("<I", command, 8)[0]
                flags = struct.unpack_from("<I", command, 24)[0]
                if string_offset != 28 or flags & ~DYLIB_USE_KNOWN_FLAGS:
                    raise malformed_load_commands(relative_path)
                weak = weak or bool(flags & DYLIB_USE_WEAK_LINK)
                reexport = bool(flags & DYLIB_USE_REEXPORT)
                upward = bool(flags & DYLIB_USE_UPWARD)
                delayed_init = bool(flags & DYLIB_USE_DELAYED_INIT)
                minimum_offset = 28
            else:
                if command_size < 24:
                    raise malformed_load_commands(relative_path)
                string_offset = struct.unpack_from("<I", command, 8)[0]
                minimum_offset = 24

            dependency_path = read_command_string(
                command, string_offset, minimum_offset, relative_path)
            command_names = {
                LC_LOAD_DYLIB: "load-dylib",
                LC_LOAD_WEAK_DYLIB: "load-weak-dylib",
                LC_REEXPORT_DYLIB: "reexport-dylib",
                LC_LOAD_UPWARD_DYLIB: "load-upward-dylib",
            }
            command_kind = command_names[command_type]
            if alternate:
                modifiers = ["alternate"]
                if flags & DYLIB_USE_WEAK_LINK:
                    modifiers.append("weak")
                if flags & DYLIB_USE_REEXPORT:
                    modifiers.append("reexport")
                if flags & DYLIB_USE_UPWARD:
                    modifiers.append("upward")
                if flags & DYLIB_USE_DELAYED_INIT:
                    modifiers.append("delayed-init")
                command_kind += "+" + "+".join(modifiers)
            dependencies.append(Dependency(
                _command_index + 1,
                command_kind,
                dependency_path,
                weak,
                reexport,
                upward,
                delayed_init,
            ))
        elif command_type in (LC_LAZY_LOAD_DYLIB, LC_PREBOUND_DYLIB):
            raise AuditError("unsupported Mach-O dependency command: {}".format(display))
        elif command_type == LC_DYLD_INFO_ONLY:
            if command_size != 48 or command_type in singleton_required_commands:
                raise malformed_load_commands(relative_path)
            singleton_required_commands.add(command_type)
            for range_offset in (8, 16, 24, 32, 40):
                validate_data_range(command, range_offset, file_size, relative_path)
        elif command_type == LC_MAIN:
            if command_size != 24 or file_type != MH_EXECUTE:
                raise malformed_load_commands(relative_path)
            main_command_count += 1
            if main_command_count > 1:
                raise malformed_load_commands(relative_path)
        elif command_type in (LC_DYLD_EXPORTS_TRIE,
                              LC_DYLD_CHAINED_FIXUPS):
            if command_size != 16 or command_type in singleton_required_commands:
                raise malformed_load_commands(relative_path)
            singleton_required_commands.add(command_type)
            validate_data_range(command, 8, file_size, relative_path)
        elif command_type & LC_REQ_DYLD:
            raise AuditError("unsupported required Mach-O command: {}".format(display))

        offset += command_size

    if offset != len(commands):
        raise malformed_load_commands(relative_path)
    if file_type == MH_EXECUTE:
        if dynamic_linkers != ["/usr/lib/dyld"] or main_command_count != 1:
            raise AuditError("invalid executable loader metadata: {}".format(display))
    elif dynamic_linkers or main_command_count:
        raise AuditError("invalid executable loader metadata: {}".format(display))
    if file_type == MH_DYLIB:
        if len(install_ids) != 1:
            raise AuditError("malformed install identity: {}".format(display))
        validate_install_id(install_ids[0], relative_path)
        install_id: Optional[str] = install_ids[0]
    else:
        if install_ids:
            raise AuditError("malformed install identity: {}".format(display))
        install_id = None

    return Image(
        real_path=os.path.realpath(path),
        relative_path=relative_path,
        file_type=file_type,
        runpaths=tuple(runpaths),
        dependencies=tuple(dependencies),
        install_id=install_id,
    )


def normalize_absolute(path: str) -> Optional[str]:
    if not path.startswith("/") or not safe_field(path) or "\\" in path:
        return None
    components = path.split("/")
    if components[0] != "" or any(
            component in ("", ".", "..") for component in components[1:]):
        return None
    return path


def normalize_bundle_relative(path: str) -> Optional[str]:
    if path.startswith("/") or "\\" in path:
        return None
    components: List[str] = []
    for component in path.split("/"):
        if not component or component == ".":
            continue
        if component == "..":
            if not components:
                return None
            components.pop()
        else:
            components.append(component)
    return "/".join(components)


def symlink_target_is_lexically_contained(relative_path: str,
                                           target: str) -> bool:
    if not safe_field(target) or target.startswith("/"):
        return False
    parent = posixpath.dirname(relative_path)
    depth = 0 if not parent else len(parent.split("/"))
    for component in target.split("/"):
        if component in ("", "."):
            continue
        if component == "..":
            if depth == 0:
                return False
            depth -= 1
        else:
            depth += 1
    return True


class ClosureAudit:
    def __init__(self, root: str, main_relative: str, architecture: str,
                 target: str) -> None:
        if architecture != "arm64":
            raise AuditError("unsupported Mach-O closure architecture")
        self.target_major = parse_target_major(target)
        self.root = os.path.realpath(root)
        if not os.path.isdir(self.root):
            raise AuditError("Mach-O closure root is unavailable")
        normalized_main = normalize_bundle_relative(main_relative)
        if normalized_main != main_relative or not safe_field(main_relative):
            raise AuditError("invalid bundle main executable path")
        self.main_relative = main_relative
        self.images_by_real: Dict[str, Image] = {}
        self.manifest_rows: Set[
            Tuple[str, str, str, str, str, str, str]
        ] = set()
        self.expanded_runpath_cache: Dict[Tuple[str, str], Tuple[str, ...]] = {}
        self.context_state_count = 0

    def inside_root(self, path: str) -> bool:
        try:
            return os.path.commonpath((self.root, path)) == self.root
        except ValueError:
            return False

    def absolute_for_relative(self, relative_path: str) -> str:
        if relative_path:
            return os.path.join(self.root, *relative_path.split("/"))
        return self.root

    def enumerate_images(self) -> None:
        regular_files: List[Tuple[str, str]] = []
        symlinks: List[Tuple[str, str]] = []
        try:
            for directory, directory_names, file_names in os.walk(
                    self.root, topdown=True, followlinks=False):
                kept_directories: List[str] = []
                for name in sorted(directory_names):
                    path = os.path.join(directory, name)
                    relative = os.path.relpath(path, self.root).replace(os.sep, "/")
                    checked_display(relative)
                    if os.path.islink(path):
                        symlinks.append((path, relative))
                    else:
                        kept_directories.append(name)
                directory_names[:] = kept_directories
                for name in sorted(file_names):
                    path = os.path.join(directory, name)
                    relative = os.path.relpath(path, self.root).replace(os.sep, "/")
                    checked_display(relative)
                    mode = os.lstat(path).st_mode
                    if stat.S_ISLNK(mode):
                        symlinks.append((path, relative))
                    elif stat.S_ISREG(mode):
                        regular_files.append((path, relative))
        except AuditError:
            raise
        except OSError:
            raise AuditError("unable to enumerate Mach-O closure root")

        for path, relative in sorted(regular_files, key=lambda item: item[1]):
            try:
                with open(path, "rb") as candidate:
                    magic = candidate.read(4)
            except OSError:
                raise AuditError("unable to inspect bundle file: {}".format(
                    checked_display(relative)))
            if magic == THIN_64_LE_MAGIC:
                image = parse_macho(path, relative, self.target_major)
                if image.real_path in self.images_by_real:
                    raise AuditError("ambiguous Mach-O image: {}".format(
                        checked_display(relative)))
                self.images_by_real[image.real_path] = image
            elif magic in OTHER_MACHO_MAGICS:
                raise AuditError("unsupported Mach-O format: {}".format(
                    checked_display(relative)))

        for path, relative in sorted(symlinks, key=lambda item: item[1]):
            try:
                target = os.readlink(path)
            except OSError:
                raise AuditError("unsafe bundle symlink: {}".format(
                    checked_display(relative)))
            if not symlink_target_is_lexically_contained(relative, target):
                raise AuditError("unsafe bundle symlink: {}".format(
                    checked_display(relative)))
            resolved = os.path.realpath(path)
            if not os.path.exists(path) or not self.inside_root(resolved):
                raise AuditError("unsafe bundle symlink: {}".format(
                    checked_display(relative)))

        if not self.images_by_real:
            raise AuditError("no Mach-O images found for dependency closure")

    def resolve_main(self) -> Tuple[Image, str]:
        main_path = self.absolute_for_relative(self.main_relative)
        if not os.path.exists(main_path):
            raise AuditError("bundle main executable is missing")
        resolved = os.path.realpath(main_path)
        if not self.inside_root(resolved):
            raise AuditError("bundle main executable escapes the bundle")
        image = self.images_by_real.get(resolved)
        if image is None or image.file_type != MH_EXECUTE:
            raise AuditError("bundle main executable is not Mach-O")
        return image, image.relative_path

    def expand_runpaths(self, image: Image, logical_path: str,
                        executable_path: str) -> Tuple[str, ...]:
        if logical_path != image.relative_path:
            raise AuditError("noncanonical Mach-O loader context")
        cache_key = (image.real_path, executable_path)
        cached = self.expanded_runpath_cache.get(cache_key)
        if cached is not None:
            return cached
        loader_directory = posixpath.dirname(logical_path)
        executable_directory = posixpath.dirname(executable_path)
        expanded: List[str] = []
        for runpath in image.runpaths:
            display = checked_display(image.relative_path)
            if runpath.startswith("/"):
                raise AuditError("unsafe runpath: {}".format(display))
            if runpath == "@loader_path":
                base, suffix = loader_directory, ""
            elif runpath.startswith("@loader_path/"):
                base, suffix = loader_directory, runpath[len("@loader_path/"):]
            elif runpath == "@executable_path":
                base, suffix = executable_directory, ""
            elif runpath.startswith("@executable_path/"):
                base, suffix = executable_directory, runpath[len("@executable_path/"):]
            else:
                raise AuditError("malformed runpath: {}".format(display))
            if "\\" in suffix or not safe_field(runpath):
                raise AuditError("malformed runpath: {}".format(display))
            if not suffix and runpath.endswith("/"):
                raise AuditError("malformed runpath: {}".format(display))
            if suffix and any(not component for component in suffix.split("/")):
                raise AuditError("malformed runpath: {}".format(display))
            combined = base if not suffix else posixpath.join(base, suffix)
            normalized = normalize_bundle_relative(combined)
            if normalized is None:
                raise ContextResolutionError(
                    "unsafe runpath: {}".format(display))
            expanded.append(normalized)
        result = tuple(expanded)
        self.expanded_runpath_cache[cache_key] = result
        return result

    def candidate_image(self, relative_path: str) -> Tuple[str, Optional[Image]]:
        candidate = self.absolute_for_relative(relative_path)
        resolved = os.path.realpath(candidate)
        if not self.inside_root(resolved):
            return "invalid", None
        if not os.path.lexists(candidate):
            return "absent", None
        if not os.path.exists(candidate):
            return "invalid", None
        if not os.path.isfile(candidate):
            return "non-loadable", None
        image = self.images_by_real.get(resolved)
        if image is None:
            return "non-loadable", None
        if image.file_type != MH_DYLIB:
            return "non-loadable", None
        return "dylib", image

    def internal_suffix(self, dependency_path: str, prefix: str,
                        image: Image) -> str:
        suffix = dependency_path[len(prefix):]
        validate_component_path(
            suffix, image.relative_path, "dependency", allow_parent=True)
        return suffix

    def resolve_dependency(
            self, image: Image, logical_path: str, executable_path: str,
            runpath_stack: Tuple[str, ...], dependency: Dependency,
            manifest_rows: Set[Tuple[str, str, str, str, str, str, str]]
    ) -> Optional[Tuple[Image, str]]:
        dependency_path = dependency.path
        display = checked_display(image.relative_path)

        if dependency_path.startswith("/"):
            normalized = normalize_absolute(dependency_path)
            if normalized is None or not (
                    normalized.startswith("/System/Library/") or
                    normalized.startswith("/usr/lib/")):
                raise AuditError("forbidden external dependency: {}".format(display))
            manifest_rows.add((
                "dependency",
                logical_path,
                str(dependency.command_ordinal),
                dependency.command_kind,
                dependency_path,
                "system",
                normalized,
            ))
            return None

        candidates: List[str] = []
        if dependency_path.startswith("@loader_path/"):
            suffix = self.internal_suffix(
                dependency_path, "@loader_path/", image)
            combined = posixpath.join(posixpath.dirname(logical_path), suffix)
            normalized = normalize_bundle_relative(combined)
            if normalized is None:
                raise ContextResolutionError(
                    "unresolved bundled dependency: {}".format(display))
            candidates.append(normalized)
        elif dependency_path.startswith("@executable_path/"):
            suffix = self.internal_suffix(
                dependency_path, "@executable_path/", image)
            combined = posixpath.join(posixpath.dirname(executable_path), suffix)
            normalized = normalize_bundle_relative(combined)
            if normalized is None:
                raise ContextResolutionError(
                    "unresolved bundled dependency: {}".format(display))
            candidates.append(normalized)
        elif dependency_path.startswith("@rpath/"):
            suffix = self.internal_suffix(dependency_path, "@rpath/", image)
            for runpath in runpath_stack:
                combined = posixpath.join(runpath, suffix) if runpath else suffix
                normalized = normalize_bundle_relative(combined)
                if normalized is None:
                    raise ContextResolutionError(
                        "unresolved bundled dependency: {}".format(display))
                candidates.append(normalized)
        else:
            raise AuditError("forbidden external dependency: {}".format(display))

        for candidate in candidates:
            status, target = self.candidate_image(candidate)
            if status in ("absent", "non-loadable"):
                continue
            if status != "dylib" or target is None:
                raise AuditError("unresolved bundled dependency: {}".format(display))
            manifest_rows.add((
                "dependency",
                logical_path,
                str(dependency.command_ordinal),
                dependency.command_kind,
                dependency_path,
                "bundle",
                candidate,
            ))
            return target, candidate

        if dependency.weak:
            manifest_rows.add((
                "dependency",
                logical_path,
                str(dependency.command_ordinal),
                dependency.command_kind,
                dependency_path,
                "weak-missing",
                "-",
            ))
            return None
        raise ContextResolutionError(
            "unresolved bundled dependency: {}".format(display))

    def visit(self, image: Image, logical_path: str, executable_path: str,
              inherited_runpaths: Tuple[str, ...],
              active_images: FrozenSet[str],
              manifest_rows: Set[
                  Tuple[str, str, str, str, str, str, str]
              ]) -> FrozenSet[str]:
        if image.real_path in active_images:
            return frozenset((image.real_path,))
        own_runpaths = self.expand_runpaths(image, logical_path, executable_path)
        effective_runpaths = own_runpaths + inherited_runpaths
        self.context_state_count += 1
        if self.context_state_count > MAX_CONTEXT_STATES:
            raise AuditError("Mach-O dependency context limit exceeded")

        reached: Set[str] = {image.real_path}
        next_active = frozenset(set(active_images) | {image.real_path})
        for dependency in image.dependencies:
            resolved = self.resolve_dependency(
                image, logical_path, executable_path, effective_runpaths,
                dependency, manifest_rows)
            if resolved is None:
                continue
            target, _target_candidate_path = resolved
            if target.real_path in next_active:
                reached.add(target.real_path)
                continue
            reached.update(self.visit(
                target,
                target.relative_path,
                executable_path,
                effective_runpaths,
                next_active,
                manifest_rows,
            ))
        result = frozenset(reached)
        return result

    def all_images(self) -> Iterable[Image]:
        return sorted(self.images_by_real.values(),
                      key=lambda item: item.relative_path)

    def validate_dependency_policies(self) -> None:
        for image in self.all_images():
            display = checked_display(image.relative_path)
            for dependency in image.dependencies:
                dependency_path = dependency.path
                if dependency_path.startswith("/"):
                    normalized = normalize_absolute(dependency_path)
                    if normalized is None or not (
                            normalized.startswith("/System/Library/") or
                            normalized.startswith("/usr/lib/")):
                        raise AuditError(
                            "forbidden external dependency: {}".format(display))
                    continue
                for prefix in ("@loader_path/", "@executable_path/", "@rpath/"):
                    if dependency_path.startswith(prefix):
                        self.internal_suffix(dependency_path, prefix, image)
                        break
                else:
                    raise AuditError(
                        "forbidden external dependency: {}".format(display))

    @staticmethod
    def plugin_path(relative_path: str) -> bool:
        return "PlugIns" in relative_path.split("/")

    def audit(self) -> None:
        self.enumerate_images()
        main_image, _main_logical = self.resolve_main()
        self.validate_dependency_policies()

        executable_roots = [
            image for image in self.all_images()
            if image.file_type == MH_EXECUTE
        ]
        if main_image not in executable_roots:
            executable_roots.append(main_image)
            executable_roots.sort(key=lambda item: item.relative_path)

        statically_reached_all: Set[str] = set()
        caller_contexts: Set[Tuple[str, Tuple[str, ...]]] = set()

        for executable in executable_roots:
            executable_logical = executable.relative_path
            statically_reached = self.visit(
                executable, executable_logical, executable_logical,
                tuple(), frozenset(), self.manifest_rows)
            statically_reached_all.update(statically_reached)

            main_runpaths = self.expand_runpaths(
                executable, executable_logical, executable_logical)
            caller_contexts.add((executable_logical, main_runpaths))
            # For a direct dlopen, dyld combines the caller image's own
            # LC_RPATH values with those of the process main executable.  A
            # statically reached image is a proven caller candidate; unrelated
            # dynamic roots must not validate one another by accident.
            for caller in self.all_images():
                if caller.real_path not in statically_reached or \
                        caller.file_type == MH_EXECUTE:
                    continue
                caller_runpaths = self.expand_runpaths(
                    caller, caller.relative_path, executable_logical)
                caller_contexts.add((
                    executable_logical,
                    caller_runpaths + main_runpaths,
                ))

        dynamic_roots = [
            candidate for candidate in self.all_images()
            if candidate.file_type != MH_EXECUTE and (
                candidate.file_type == MH_BUNDLE or
                self.plugin_path(candidate.relative_path) or
                candidate.real_path not in statically_reached_all
            )
        ]

        for dynamic_image in dynamic_roots:
            successful_rows: Set[
                Tuple[str, str, str, str, str, str, str]
            ] = set()
            first_context_error: Optional[ContextResolutionError] = None
            context_succeeded = False
            for executable_logical, inherited in sorted(caller_contexts):
                trial_rows: Set[
                    Tuple[str, str, str, str, str, str, str]
                ] = set()
                try:
                    self.visit(
                        dynamic_image,
                        dynamic_image.relative_path,
                        executable_logical,
                        inherited,
                        frozenset(),
                        trial_rows,
                    )
                except ContextResolutionError as error:
                    if first_context_error is None:
                        first_context_error = error
                    continue
                context_succeeded = True
                successful_rows.update(trial_rows)
            if not context_succeeded:
                if first_context_error is not None:
                    raise first_context_error
                raise AuditError("no plausible Mach-O caller context")
            self.manifest_rows.update(successful_rows)

    def write_manifest_fragment(self, output: str) -> None:
        directory = os.path.dirname(os.path.abspath(output))
        temporary_path = ""
        try:
            descriptor, temporary_path = tempfile.mkstemp(
                prefix=".macho-closure.", dir=directory)
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
                for row in sorted(self.manifest_rows):
                    if not all(safe_field(field) for field in row):
                        raise AuditError("unsafe Mach-O manifest row")
                    stream.write("\t".join(row) + "\n")
            os.replace(temporary_path, output)
            temporary_path = ""
        except AuditError:
            raise
        except OSError:
            raise AuditError("unable to write Mach-O dependency manifest")
        finally:
            if temporary_path:
                try:
                    os.unlink(temporary_path)
                except OSError:
                    pass


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="audit a thin arm64 Mach-O dependency closure")
    parser.add_argument("--root", required=True)
    parser.add_argument("--main", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--output", required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        audit = ClosureAudit(
            arguments.root,
            arguments.main,
            arguments.arch,
            arguments.target,
        )
        audit.audit()
        audit.write_manifest_fragment(arguments.output)
    except AuditError as error:
        sys.stderr.write("{}\n".format(error))
        return 1
    except Exception:
        # Unexpected failures remain generic so public CI logs cannot disclose
        # an absolute build path, a raw load-command string, or an identifier.
        sys.stderr.write("Mach-O dependency closure audit failed\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
