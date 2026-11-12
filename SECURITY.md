# Security policy

## Supported versions

This project is alpha software. Security fixes target the current `main` branch
and the latest published build. Older builds do not have a separate maintenance
or backport commitment. Please retest on the latest build when possible.

## Reporting a vulnerability

Email Kadima Tech at [jacobus@kadima-tech.com](mailto:jacobus@kadima-tech.com)
to report a vulnerability privately. You can also use GitHub's private
[Report a vulnerability](https://github.com/kadima-tech/openomf-battlegrounds/security/advisories/new)
form if it is available. Do not publish exploit details, credentials or private
game data in a public issue or pull request.

Include the affected version or commit, operating system (macOS, Windows or Linux distribution) version and architecture, impact,
reproduction steps and any suggested fix. Prefer a minimal synthetic input over
copyrighted game files. Let maintainers know if you plan to disclose the issue
publicly so a fix and disclosure can be coordinated. Response times depend on
maintainer availability; there is no guaranteed response time or bounty program.

Relevant areas include malformed game-data parsing, archive extraction, patch
application, network play (the encrypted game traffic and its handshake, and the
lobby server in `server/`; see docs/NETWORK_PLAN.md 7.8-7.9), bundled dependencies
and release packaging. Ordinary crashes and
gameplay bugs without a security impact can use the public bug report template.
