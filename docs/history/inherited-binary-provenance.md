# Inherited binary provenance

The reconstructed source snapshots retain two upstream binary baselines. They
are not used in the Apple Silicon macOS release, but remain in source history
for upstream compatibility and reproducibility.

## Windows OpenSSL vendor baseline

`ext/openssl/windows/` is byte-for-byte the same Git tree published in the
[upstream Barrier v2.4.0 source tag](https://github.com/debauchee/barrier/tree/v2.4.0).
It contains upstream-generated Windows libraries and their inherited build
metadata. The public audit accepts protected-metadata patterns inside this
subtree only while its complete Git tree matches the fixed reviewed tree; any
added, removed, or changed byte fails the audit. Its license is retained in
[`ext/openssl/LICENSE`](../../ext/openssl/LICENSE).

## IS Download DLL

`dist/inno/scripts/isxdl/isxdl.dll` is byte-for-byte the file distributed in
both the upstream Barrier v2.4.0 source tag and the official
[IS Download DLL v5.1.0 release](https://github.com/KrinkelsTeam/isxdl/releases/tag/isxdl-5.1.0).
The component is redistributed under the BSD 3-Clause License retained beside
it in [`dist/inno/scripts/isxdl/LICENSE`](../../dist/inno/scripts/isxdl/LICENSE).

No historical binary from these baselines is repackaged into this fork's
Apple Silicon macOS release artifact.
