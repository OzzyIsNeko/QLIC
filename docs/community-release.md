# Unsigned public release

The community tier packages an unsigned public release. It runs the compiler,
codec, conformance, WebAssembly, package, checksum, SBOM, and provenance checks.

Every archive contains `UNSIGNED-COMMUNITY-RELEASE.txt`, provenance records the
`community` channel, and the release notes explain Windows SmartScreen behavior.

Stage a release candidate without publishing it:

```powershell
.\scripts\stage-community-release.ps1
```

The command rebuilds and verifies the packages, then writes the exact upload
set under `release/qlic-<version>-community/upload`. It does not use Git, GitHub,
or the network and does not sign, commit, tag, or publish anything.

Use `-SkipPackage` only when `dist` was produced by the same source tree with
`scripts/package.ps1 -CommunityRelease`. The staging command rejects a
development or production provenance record.

Before publishing manually:

1. Review `RELEASE-NOTES.md` and `RELEASE-ASSETS.json` in the staged directory.
2. Run `VERIFY-RELEASE.ps1` there once more.
3. Commit and tag the exact source revision recorded in provenance.
4. Paste the release notes and upload every file from `upload`.
5. Download the published files on a clean machine and compare their hashes.
