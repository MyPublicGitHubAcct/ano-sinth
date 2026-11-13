# Product identity: PLACEHOLDER values for development builds.
#
# The real release identifiers are private and never committed. To build with
# them, do one of the following:
#   - Local: copy this file to cmake/Identity.local.cmake (gitignored) and fill
#     in the real values. It is loaded after this file and overrides it.
#   - CI: pass them as cache variables from secrets, for example
#       cmake --preset release -DBRUTALIS_MANUFACTURER_CODE=$SECRET ...
#
# Release builds set BRUTALIS_REQUIRE_REAL_IDENTITY=ON, which fails configure
# if any placeholder is still in use.
#
# AU rules: both codes are exactly 4 characters with at least one uppercase
# letter. For plugin codes, GarageBand wants an uppercase letter followed by
# lowercase.
#
# The placeholders deliberately differ from the release values, so a
# development build and a release build can be installed side by side.

set(BRUTALIS_PRODUCT_NAME      "Brutalis"                 CACHE STRING "Product name shown to users")
set(BRUTALIS_COMPANY_NAME      "Anoesis Audio"            CACHE STRING "Company name shown to users")
set(BRUTALIS_MANUFACTURER_CODE "Devm"                     CACHE STRING "4-char AU/VST manufacturer code (private)")
set(BRUTALIS_PLUGIN_CODE       "Devp"                     CACHE STRING "4-char AU/VST plugin code (private)")
set(BRUTALIS_BUNDLE_ID         "com.example.brutalis-dev" CACHE STRING "macOS bundle identifier (private)")

# Values that mark a build as a development build. Used by the release guard.
set(BRUTALIS_PLACEHOLDER_MANUFACTURER_CODE "Devm")
set(BRUTALIS_PLACEHOLDER_PLUGIN_CODE       "Devp")
set(BRUTALIS_PLACEHOLDER_BUNDLE_ID         "com.example.brutalis-dev")
