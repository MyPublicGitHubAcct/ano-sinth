# Loads and validates the product identity.
#
# Order: the placeholders in Identity.example.cmake, then the private
# Identity.local.cmake if it exists (gitignored), then any -D cache values
# (CI secrets). See docs/development/product-identity.md.
#
# This file must never contain the real values.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/Identity.example.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Identity.local.cmake" OPTIONAL RESULT_VARIABLE _brutalis_local_identity)

# AU/VST code rules: exactly 4 printable ASCII characters with at least one
# uppercase letter (all-lowercase manufacturer codes are reserved by Apple).
function(_brutalis_validate_four_char_code name value)
    string(LENGTH "${value}" len)
    if(NOT len EQUAL 4)
        message(FATAL_ERROR "${name} must be exactly 4 characters (it has ${len}).")
    endif()
    if(NOT value MATCHES "^[ -~][ -~][ -~][ -~]$")
        message(FATAL_ERROR "${name} must contain only printable ASCII characters.")
    endif()
    if(NOT value MATCHES "[A-Z]")
        message(FATAL_ERROR "${name} must contain at least one uppercase letter.")
    endif()
endfunction()

_brutalis_validate_four_char_code(BRUTALIS_MANUFACTURER_CODE "${BRUTALIS_MANUFACTURER_CODE}")
_brutalis_validate_four_char_code(BRUTALIS_PLUGIN_CODE "${BRUTALIS_PLUGIN_CODE}")

# GarageBand expects a plugin code of one uppercase letter followed by
# lowercase. Other hosts don't care, so this is a warning, not an error.
if(NOT BRUTALIS_PLUGIN_CODE MATCHES "^[A-Z][a-z0-9][a-z0-9][a-z0-9]$")
    message(WARNING "BRUTALIS_PLUGIN_CODE should be one uppercase letter followed by "
                    "lowercase letters or digits, for GarageBand.")
endif()

if(NOT BRUTALIS_BUNDLE_ID MATCHES "^[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)+$")
    message(FATAL_ERROR "BRUTALIS_BUNDLE_ID must be a reverse-DNS identifier, e.g. com.example.product.")
endif()

set(_brutalis_placeholders_in_use "")
if(BRUTALIS_MANUFACTURER_CODE STREQUAL BRUTALIS_PLACEHOLDER_MANUFACTURER_CODE)
    list(APPEND _brutalis_placeholders_in_use BRUTALIS_MANUFACTURER_CODE)
endif()
if(BRUTALIS_PLUGIN_CODE STREQUAL BRUTALIS_PLACEHOLDER_PLUGIN_CODE)
    list(APPEND _brutalis_placeholders_in_use BRUTALIS_PLUGIN_CODE)
endif()
if(BRUTALIS_BUNDLE_ID STREQUAL BRUTALIS_PLACEHOLDER_BUNDLE_ID)
    list(APPEND _brutalis_placeholders_in_use BRUTALIS_BUNDLE_ID)
endif()

if(_brutalis_placeholders_in_use)
    if(BRUTALIS_REQUIRE_REAL_IDENTITY)
        list(JOIN _brutalis_placeholders_in_use ", " _names)
        message(FATAL_ERROR
            "BRUTALIS_REQUIRE_REAL_IDENTITY is ON but these still hold placeholder values: ${_names}. "
            "Provide cmake/Identity.local.cmake or pass the real values with -D.")
    endif()
    set(BRUTALIS_IDENTITY_KIND "development (placeholder)")
else()
    set(BRUTALIS_IDENTITY_KIND "release")
endif()

# Never print the codes themselves: configure logs end up in CI output.
message(STATUS "Brutalis identity: ${BRUTALIS_IDENTITY_KIND}")
