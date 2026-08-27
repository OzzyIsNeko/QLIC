# Production release gate

This checklist is normative for a QLIC production release. A build is not a
production release merely because it compiles, passes unit tests, or is placed
in the `Release 1.0` directory. Every applicable automated and manual gate below
must have a retained result tied to the same source revision and artifact
hashes.

## Automated gates

1. Release and sanitizer test suites pass on Windows x64 and Linux x86-64.
2. Windows ARM64 and macOS arm64/x86-64 compile and run the portable codec
   suite; platform-specific packages are claimed only where their integration
   suite also passes.
3. The C, Rust, and Web decoders pass all positive, negative, and prior-version
   conformance vectors and agree on decoded samples and metadata. Retained PQ
   and HLG vectors must preserve exact samples and CICP transfers 16 and 18.
4. The Web archive contains no native executable or DLL. Its self-contained
   `index.html` works when double-clicked offline, and its separate ESM/Wasm API
   works from a generic static HTTP host. In current Chrome, Edge, and Firefox,
   `tests/browser_smoke.html?package=dist` passes module loading, Wasm fetching,
   worker encode, exact RGBA decode, URL/Blob decode, canvas drawing, and HDR
   decode intact.
5. Coverage-guided fuzzing covers the C container/entropy decoders, metadata
   parsers, import adapters, and WIC `IStream` boundary. The release corpus must
   run clean under ASan/UBSan and Windows Application Verifier/PageHeap.
6. Thread, cancellation, allocation-limit, work-budget, malformed-stream,
   hostile-seek, and COM STA/MTA stress tests pass.
7. Package contents, installed SDK consumers, SBOMs, checksums, Authenticode
   signatures, archive provenance, and clean-source reproducibility pass.
8. Upgrade, uninstall, repair, machine-wide WIC registration/discovery, and
   clean virtual-machine tests pass without leaving registrations or files
   behind. A non-elevated launcher must request UAC before changing system
   state, and canceling that request must leave the installation unchanged.

## Photographic workflow gates

1. Exact samples, ICC/CICP authority, HDR mastering/content-light data, alpha
   association, EXIF, XMP, IPTC, JUMBF/C2PA, orientation, DPI, and thumbnails
   survive the documented supported round trips. Any destination format that
   cannot carry a record must cause an explicit warning or error selected by
   policy; silent loss is forbidden.
2. Straight and premultiplied alpha have golden tests for zero-alpha hidden
   color, partial alpha, 8/16-bit data, WIC, PNG, and associated-alpha TIFF.
3. HDR10 is tested on a calibrated HDR display and exact 16-bit integer data is
   tested in color-managed applications. No float/half or RAW-development claim
   is made unless a future profile implements it.
4. Explorer, Windows Photos, Photoshop, Lightroom Classic, and Affinity Photo
   qualification records identify exact application/OS versions, operations,
   source fixtures, output hashes, and screenshots or logs.

## Release authority

The release owner signs the binaries and installer, verifies the timestamped
signatures on an offline copy, publishes the provenance/SBOM/checksum bundle,
and approves the retained qualification record. Missing certificates, hardware,
or proprietary applications are release blockers—not reasons to mark a gate as
passed. An unsigned community release may be public when every archive states
that limitation and its checksums and provenance are published. Development
packages are not public releases.

Generate the qualification record's `source_revision` with
`scripts/get-source-revision.ps1 -SourceDir <reviewed-source>`. Production
packaging rebuilds from that exact tree and rejects a different digest. The
package-only `UNSIGNED-DEVELOPMENT-BUILD.txt` and
`UNSIGNED-COMMUNITY-RELEASE.txt` markers are deliberately excluded from this
digest so an unsigned source archive can still prove that its source content
matches the provenance build input.
