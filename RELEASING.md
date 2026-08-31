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

## 2. Build the audited app

Run the `release-macos-arm64` workflow from the final, green `main` commit. The
workflow builds checksum-pinned Qt and OpenSSL sources for the declared minimum
macOS version, runs the test suites, audits every bundled Mach-O file, and
uploads an unsigned app archive.

The workflow output is an input to signing, not a public release asset. Do not
publish the unsigned archive.

## 3. Sign, notarize, and package

On a trusted signing host:

1. Sign the app from the inside out with a Developer ID Application identity.
2. Submit the signed app to Apple's notary service and wait for acceptance.
3. Staple the accepted ticket to the app.
4. Create a disk image containing `Barrier.app` and an `Applications` link.
5. Sign, notarize, and staple the disk image.

Keep certificate names, keychain profiles, credentials, and local filesystem
paths out of release notes and public logs.

Verify the final app and disk image before uploading them:

```sh
codesign --verify --deep --strict --verbose=2 Barrier.app
spctl --assess --type execute --verbose=2 Barrier.app
xcrun stapler validate Barrier.app
hdiutil verify Barrier-X.Y.Z-release-arm64.dmg
codesign --verify --verbose=2 Barrier-X.Y.Z-release-arm64.dmg
xcrun stapler validate Barrier-X.Y.Z-release-arm64.dmg
shasum -a 256 Barrier-X.Y.Z-release-arm64.dmg
```

Mount the disk image and run `scripts/verify-macos-deployment-target.sh` against
the mounted app. Confirm that the mounted app's version and embedded commit
identify the final `main` commit. Install and launch that exact app on Apple
Silicon running the declared minimum macOS version before publication.

## 4. Create the signed tag

Create a signed, annotated tag at the exact release commit and push only that
tag:

```sh
git tag -s vX.Y.Z -m vX.Y.Z RELEASE_COMMIT
git push origin vX.Y.Z
```

## 5. Publish the GitHub release

Create a draft GitHub release for `vX.Y.Z`. Use the public release notes as its
description and upload exactly one public package:

```text
Barrier-X.Y.Z-release-arm64.dmg
```

Include the SHA-256 digest and state that the package is Developer ID signed,
Apple notarized, and stapled. Privately audit the issue, pull request, tag, and
draft-release text before publishing. Do not include internal hostnames,
network addresses, local paths, signing identities, or build-system inventory,
and never print a matched protected value into a public log.

Download the draft asset to a fresh location and repeat the digest, signature,
Gatekeeper, notarization-ticket, disk-image, mounted-app, privacy, and minimum-OS
launch checks. Publish the release only after the downloaded asset passes every
check.
