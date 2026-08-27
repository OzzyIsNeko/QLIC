# Security

Report security problems privately through the GitHub Security tab. Include the
QLIC version or source revision, a small reproducer, the decoder used, and what
happened. If private reporting is unavailable, open an issue asking for a
private contact. Do not post the reproducer publicly before a fix is available.

QLIC 1.0 has no paid security response-time promise. Security fixes target the
active release tree.

Treat every image as untrusted. Native, WIC, Rust, and browser decoders enforce
input and output limits before large allocations where their format permits.
Applications must choose limits for their workload and enforce a decode-time
budget outside the library. Run network-facing decoding in a process with only
the filesystem, memory, and CPU access it needs.

Local packages include SHA-256 manifests and SPDX SBOMs. Verify both before
deployment and retain the exact decoder used for stored QLIC data.
