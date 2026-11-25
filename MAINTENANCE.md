# Maintenance

## Maintenance Log

- **2026-11-25:** weekly dependency bump.
  - uv 0.13.7 → 0.13.9. bsdiff4 1.4.2 → 1.4.3. Applying an existing BSDIFF40 patch is unchanged.
  - python-build-standalone `20261110` → `20261124` (still CPython 3.15.2). The release is newer than the 7-day cutoff; it is the rebuild that carries Expat 2.11.0 (CVE-2026-134703), zstd 1.6.0 (CVE-2026-124851) and libffi 3.8.1 (GHSA-fq46-69xf-x5gf, GHSA-5pc3-hr6f-q3mm, GHSA-h52h-3759-xqm4, GHSA-vg77-j273-647r, GHSA-23hx-9rxv-9qfp). uv 0.13.9 does not fetch it, so the bundle scripts unpack it directly.
  - stb_image `52a3128f` → `98bc6401`. Lucide lucide-static 1.72.0 → 1.75.0 (the icons in use keep their codepoints). openindeo `5452df0c` → `cd14ed5b` (its workflow file only).
  - `actions/setup-go` v7.0.0 → v7.1.0. Go stays 1.27.4.
  - Held: SDL 3.4.20 (the client is SDL2), Ubuntu 22.04 download runners (glibc 2.35), and other releases published after 2026-11-18.
- **2026-11-18:** weekly dependency bump onto current main (Go was already 1.26.11).
  - Go lobby toolchain 1.26.11 → 1.27.4 (`server/go.mod` and `golang:1.27.4-alpine`). Standard library only.
  - Bundled CPython 3.12.12 / python-build-standalone `20251028` → 3.15.2 / `20261110`, unpacked directly because uv 0.13.7 still fetches the `20261026` rebuild. uv 0.9.7 → 0.13.7. bsdiff4 1.2.6 → 1.4.2.
  - unshield 1.5.1 → 1.6.2 (needs CMake 3.21), Dear ImGui `v1.92.5-docking` → `v1.93.0-docking`, Monocypher 4.0.2 → 4.0.3, stb_image to commit `52a3128f`, Lucide 0.469.0 → lucide-static 1.72.0.
  - macOS CI runners `macos-15` → `macos-26`. App deployment target stays 13.0. Linux player builds stay on `ubuntu-22.04` (glibc 2.35). The zlib notice now matches the existing 1.4 pin.
- **2026-11-12:** new dependencies for internet play.
  - Monocypher 4.0.2 (with its optional Ed25519), pinned by commit and SHA-256 via FetchContent in `engine/CMakeLists.txt`; its licence is bundled by the macOS, Windows and Linux app builds and listed in the third-party notices. Windows links `bcrypt` for `BCryptGenRandom`.
  - The lobby server `server/` needs a Go toolchain (`go.mod`: 1.22; standard library only, so no module pins). Its CI (`.github/workflows/server.yml`) pins `actions/setup-go` v7.0.0 by commit and the Dockerfile's build image `golang:1.22-alpine`.
  - The Linux app build now also copies ENet's licence, which it had left out.
- **2026-11-11:** the Linux CI jobs timed out during `apt-get` while Ubuntu's mirrors were failing: 43 minutes against the usual 30 seconds, and DNS failures for ports.ubuntu.com on arm64. Added apt retries and timeouts, and a 15-minute limit on the install step.
- **2026-11-11:** CI broke on Windows (MinGW) and Fedora: `engine/editor/app/assets.cpp` used `std::clamp` without `<algorithm>`. Apple's libc++ includes it indirectly, GCC's libstdc++ does not.
- **2026-11-11:** made the asset tests' temporary folders unique.
  - `editor-har` and `editor-tournament` run the same `editor-assets-tests` binary.
  - Under `ctest -j`, both could create the same temp folder in the same second (an unseeded `rand()` plus the time), and one deleted it under the other.
  - The name now includes the test mode and a `random_device` value.
- **2026-11-11:** new third-party pins for the glTF pipeline: cgltf v1.15 and stb_image at a fixed commit, both via FetchContent in `engine/CMakeLists.txt`. They are listed in the third-party notices.
- **2026-11-10:** removed a duplicate `target_link_libraries(enet ...)`. It broke the Windows CI configure because the plain and keyword signatures were mixed on one target.

## Recurring Patterns

- **Pinned dependency bumps.** Third-party code is pinned by commit and SHA-256 (FetchContent in `engine/CMakeLists.txt`) or by action commit (workflows), and each one is also named in `THIRD_PARTY_NOTICES.md` and copied into the app bundles' `licenses/` by `launcher/build-app.sh`, `launcher/build-linux.sh` and `launcher/windows/build-windows.sh`.
  - Fix: when adding or bumping one, change the pin, the notices row and all three bundle scripts together; for the Go lobby, `server/go.mod`'s Go version and the Dockerfile's `golang` image together.
  - Seen: ENet (2026-11-10), cgltf / stb (2026-11-11), Monocypher and the Go toolchain (2026-11-12).
- **GCC-only build breaks from missing standard headers.** Code built only on macOS can use `std::` functions whose header libc++ pulls in indirectly. The GCC builds (Windows MinGW, Linux, Fedora) then fail in CI.
  - Fix: add the header.
  - Prevention: before merging, check new files for `std::` algorithms without `<algorithm>` (see the 2026-11-11 entry).
