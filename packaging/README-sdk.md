# QLIC C SDK

This package contains the shared and static libraries, headers, CMake and
pkg-config files, examples, and public contracts.

```cmake
find_package(qlic 1.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE qlic::qlic_static)
```

Use `qlic::qlic` for the shared library. Start with `docs/sdk.md`. Wire and
compatibility details are in `docs/format.md` and `docs/profiles.md`.

The API version is 8. Set decode limits for the product. Free returned memory
with the matching QLIC function.
