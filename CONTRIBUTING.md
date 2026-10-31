# Contributing to openomf-battlegrounds

Bug reports, documentation, tests and code contributions are welcome. Please
follow our [Code of Conduct](CODE_OF_CONDUCT.md). For security issues, use the
private reporting instructions in [SECURITY.md](SECURITY.md).

## Before you start

Check existing [issues](https://github.com/kadima-tech/openomf-battlegrounds/issues)
and [PLACEHOLDERS.md](PLACEHOLDERS.md) for known problems and missing features.
For a substantial change, open an issue first so maintainers can discuss the
scope and approach. Small fixes can go straight to a pull request.

The engine targets the original game's final official patch, **2.123**. Read
[DEVELOPING.md](DEVELOPING.md) for the code layout, reverse-engineering notes
and debugging tools. [README.md](README.md#building) covers building the engine;
[launcher/README.md](launcher/README.md) covers the macOS app and offline setup.

## Reporting bugs and requesting features

For bugs, include the app version or commit, macOS version, CPU architecture,
steps to reproduce, expected and actual results, and relevant logs. A screenshot
or short recording can help with rendering or gameplay problems. Explain whether
you used the launcher or a source build and whether the problem reproduces with
the supported English disc and 2.123 installation.

For feature requests, describe the problem, who it affects and the behavior you
want. Mention the original game's behavior when requesting compatibility work.
Online and LAN play are not implemented yet.

Remove personal paths, credentials and other private information from logs.
Do not upload game files, disc images, extracted scripts or decompiler dumps.

## Sending a pull request

1. Fork the repository and create a branch for your change.
2. Keep the change focused and follow the surrounding code's style. Explain
   compatibility decisions and record new findings in `re/FINDINGS.md` when useful.
3. Add or update meaningful tests for behavior changes and update affected docs.
4. Run the checks relevant to your change and describe the results in the PR.
   If a check needs game data or hardware you do not have, say so explicitly.
5. Open the PR against `main`, describe the problem and resulting behavior, and
   link any related issue. Include before/after screenshots for visual changes.

Engine changes can be checked with:

```sh
cmake -S engine -B engine/build
cmake --build engine/build -j
ctest --test-dir engine/build --output-on-failure
```

CTest needs your own original game data; see [README.md](README.md) for setup.
Launcher checks, including Python installer tests, native readiness tests and
bundle validation, are in [launcher/README.md#validation](launcher/README.md#validation).
Pull requests run the macOS packaging checks for both supported architectures
in CI; data-dependent engine tests are not run there.

## Rights and licensing

Submit only work you have the right to contribute. Contributions to the project's
original code and documentation are provided under the [MIT license](LICENSE).
Keep third-party copyright and license notices intact; identify the source,
version and license of any dependency you add, and update
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the app's packaged notices.

The original game, its data, trademarks and official updates belong to their
respective rights holders and are not covered by the project's MIT license.
Keep your disc files, extracted assets and analysis dumps local. Changes to the
bundled cumulative update must follow its documented reproduction and hash
validation process in [launcher/README.md](launcher/README.md#cumulative-update-provenance);
do not add full game files or original patch executables.
