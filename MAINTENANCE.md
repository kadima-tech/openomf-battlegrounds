# Maintenance

## Maintenance Log

- **2026-11-11:** CI broke on Windows (MinGW) and Fedora: `engine/editor/app/assets.cpp` used `std::clamp` without `<algorithm>`. Apple's libc++ includes it indirectly, GCC's libstdc++ does not.
- **2026-11-11:** made the asset tests' temporary folders unique.
  - `editor-har` and `editor-tournament` run the same `editor-assets-tests` binary.
  - Under `ctest -j`, both could create the same temp folder in the same second (an unseeded `rand()` plus the time), and one deleted it under the other.
  - The name now includes the test mode and a `random_device` value.
- **2026-11-11:** new third-party pins for the glTF pipeline: cgltf v1.15 and stb_image at a fixed commit, both via FetchContent in `engine/CMakeLists.txt`. They are listed in the third-party notices.
- **2026-11-10:** removed a duplicate `target_link_libraries(enet ...)`. It broke the Windows CI configure because the plain and keyword signatures were mixed on one target.

## Recurring Patterns

- **GCC-only build breaks from missing standard headers.** Code built only on macOS can use `std::` functions whose header libc++ pulls in indirectly. The GCC builds (Windows MinGW, Linux, Fedora) then fail in CI.
  - Fix: add the header.
  - Prevention: before merging, check new files for `std::` algorithms without `<algorithm>` (see the 2026-11-11 entry).
