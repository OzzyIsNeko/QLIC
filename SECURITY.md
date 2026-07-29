# Security

Report security problems privately through the GitHub Security tab.

Include the QLIC version, a small reproducer, and what happened. If private reporting is unavailable, open an issue asking for a private way to continue. Do not publish the details before a fix is available.

QLIC treats every input as untrusted. The native, WIC, and browser decoders check limits before large allocations or decoding work. Applications using the C SDK can change those limits for each call. They should also use a time limit that makes sense for what they are doing.

Only the current release receives security fixes.
