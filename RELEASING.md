# Creating a release

This document is for maintainers of this Barrier fork. It describes the
Apple Silicon macOS release process. Release credentials and signing identities
must never be committed to this repository or copied into public build logs.

## 1. Prepare the release change

Work from a release branch and update the version consistently in:

- `Build.properties`
- `cmake/Version.cmake`
- `doc/barrierc.1`
- `doc/barriers.1`
- `.github/ISSUE_TEMPLATE/bug_report.yml`

Update `doc/release_notes/index.md` and `README.md` as needed. Open a pull
request, run all required checks, review the exact head commit, and merge it
before building the public artifact.

## 2. Create immutable release tags

After the release change reaches `main`, wait for successful `ci.yml` and
`public-audit.yml` push runs. Create signed product and automation tags at that
exact commit:

```sh
git tag -s vX.Y.Z -m vX.Y.Z RELEASE_COMMIT
git tag -s vX.Y.Z-automation.1 -m vX.Y.Z-automation.1 RELEASE_COMMIT
git push origin vX.Y.Z vX.Y.Z-automation.1
```

The release workflow and recipe verifier must already pin that automation tag,
the reviewed version-file hash, and the exact source/automation workflow
fingerprints.

## 3. Build the audited app

Dispatch `release-macos-arm64` from the protected automation tag with the
product tag as its input. The workflow builds checksum-pinned Qt and OpenSSL
sources for the declared minimum macOS version, runs the test suites, audits
every bundled Mach-O file, and uploads an unsigned app archive.

The workflow output is an input to signing, not a public release asset. Download
it to a trusted signing host and record its SHA-256 digest.

## 4. Sign, notarize, and package without intervention

The trusted signing host requires:

- exactly one valid Developer ID Application identity;
- a validated `notarytool` Data Protection Keychain profile named
  `notarytool`, accessible to commands run by Terminal.app; and
- macOS Automation permission for the invoking agent application to control
  Terminal.app.

Run one command:

```sh
scripts/notarize-macos-release.sh \
  --archive /absolute/path/Barrier-vX.Y.Z-macos-arm64-unsigned-SHA.zip \
  --archive-sha256 UNSIGNED_ARCHIVE_SHA256 \
  --version X.Y.Z \
  --revision RELEASE_COMMIT_FIRST_8_HEX \
  --output-root /absolute/path/to/new-release-output
```

The launcher validates every argument and the unsigned archive digest, copies
that archive into a unique mode-0700 bridge, and uses an argument-safe Apple
Event to run its temporary worker inside the already-authorized Terminal.app
context. The worker copies and rehashes that private input before extraction,
closing the external-path replacement window. The bridge never contains a
certificate, API key, password, or exported Keychain value. The launcher waits
for completion and propagates worker failure, permission denial, or timeout.

Inside the authorized Terminal context, the worker:

1. discovers exactly one valid Developer ID Application identity;
2. signs nested code from the inside out;
3. notarizes and staples the application;
4. creates and signs the disk image;
5. notarizes and staples the disk image;
6. mounts the disk image and verifies version, revision, architecture,
   deployment target, signatures, tickets, Gatekeeper, distribution policy,
   and protected-metadata rules; and
7. writes the DMG checksum and returns it to the launcher.

Existing output paths, missing credentials, ambiguous identities, invalid
signatures, rejected notarization submissions, malformed status, and timeouts
all fail closed. Keep certificate names, Keychain data, credentials, and local
filesystem paths out of release notes and public logs.

## 5. Publish the GitHub release

Create a draft GitHub release for `vX.Y.Z`. Use the public release notes as its
description and upload exactly one public package:

```text
Barrier-X.Y.Z-release-arm64.dmg
```

Include the SHA-256 digest and state that the package is Developer ID signed,
Apple notarized, and stapled. Privately audit the issue, pull request, tags,
and draft-release text before publishing. Do not include internal hostnames,
network addresses, local paths, signing identities, or build-system inventory,
and never print a matched protected value into a public log.

Download the draft asset to a fresh location and repeat the digest, signature,
Gatekeeper, notarization-ticket, disk-image, mounted-app, privacy, and
minimum-OS checks. Publish the release only after the downloaded asset passes
every check.
