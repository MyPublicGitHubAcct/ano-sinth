# Product identity

## What it is

Plugin hosts identify a plugin by:

- a 4-character **manufacturer code**;
- a 4-character **plugin code**;
- on macOS, a **bundle ID**.

The VST3 class ID and the AU component description are derived from these. If any of them changes after release, hosts treat the plugin as a different product, and saved sessions lose their instances. So the values are chosen once and never changed.

Brutalis's real values are **private**: they are kept out of the repository and its history. (They are still visible inside shipped binaries. This is repo hygiene, not secrecy from users.)

## Where the values come from

[cmake/BrutalisIdentity.cmake](../../cmake/BrutalisIdentity.cmake) loads them in this order. Later sources override earlier ones.

1. [cmake/Identity.example.cmake](../../cmake/Identity.example.cmake), tracked: public **placeholder** values for development builds. They deliberately differ from the real ones, so a dev build and a release build can be installed side by side.
2. `cmake/Identity.local.cmake`, **gitignored**: the real values on a developer machine. Copy the example file and fill it in. Keep a backup outside the repo.
3. `-D` cache variables: CI passes the real values from GitHub Actions secrets (`BRUTALIS_MANUFACTURER_CODE`, `BRUTALIS_PLUGIN_CODE`, `BRUTALIS_BUNDLE_ID`).

Code and CMake refer to these values only through the `BRUTALIS_*` variables. Configure prints only whether the identity is `development (placeholder)` or `release`, never the values, because configure logs end up in CI output.

## Validation (at configure time)

- Both codes must be exactly 4 printable ASCII characters with at least one uppercase letter. All-lowercase manufacturer codes are reserved by Apple.
- The plugin code should be one uppercase letter followed by lowercase, which GarageBand expects. A mismatch is only a warning.
- The bundle ID must be reverse-DNS.
- With `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`, configure **fails** if any placeholder is still in use. The `release` preset and the release workflow turn this on, so a placeholder build cannot ship.

All of this was checked on 2026-11-13 with a scratch project:

- placeholders with the guard off configure as development;
- placeholders with the guard on fail;
- real-looking `-D` values with the guard on configure as release;
- an all-lowercase code fails;
- a 5-character code fails;
- `ABcd` gives the GarageBand warning.

## Leak prevention

[tools/check-identity-leak.sh](../../tools/check-identity-leak.sh) fails if a real value appears in tracked files or in any commit message. It reads the values from environment variables (CI secrets) or from `cmake/Identity.local.cmake`.

- The 4-character codes are matched as **whole words**. The product and company names start with the same letters, and a substring match would flag every mention of them.
- In CI it prints only the names of the files that leak, never the matching lines.
- It runs in CI (`identity-leak` job), in the release workflow before any build, and locally through the pre-commit and commit-msg hooks.

Both directions were tested: a staged file containing the real plugin code is caught, and the product and company names are not flagged.
