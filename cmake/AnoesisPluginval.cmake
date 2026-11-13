# pluginval (https://github.com/Tracktion/pluginval): Tracktion's
# cross-platform plugin validator. It loads the built plugin in a host and runs
# its test suite (scan, open, audio processing, state, automation, threading,
# editor, buses). It runs as a CTest test, so every route that runs tests runs
# it too: tools/check.sh, the workflow presets, and Version CI.
#
# The binary is downloaded once at configure time from a pinned release,
# verified by SHA-256, and cached in .cache/pluginval/ (gitignored). Set
# BRUTALIS_PLUGINVAL_EXECUTABLE to use your own copy, for example when offline.
# pluginval is GPLv3. That doesn't affect us: we only run it, and never link or
# ship it.

include_guard(GLOBAL)

set(ANOESIS_PLUGINVAL_VERSION "1.0.4")

# SHA-256 of each release zip for ANOESIS_PLUGINVAL_VERSION. When you bump the
# version, update these from the new release assets.
set(_anoesis_pluginval_sha256_macOS   "3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f")
set(_anoesis_pluginval_sha256_Linux   "c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352")
set(_anoesis_pluginval_sha256_Windows "c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab")

# Sets <out_var> to the path of a pluginval executable, downloading it if needed.
function(anoesis_get_pluginval out_var)
    if(BRUTALIS_PLUGINVAL_EXECUTABLE)
        if(NOT EXISTS "${BRUTALIS_PLUGINVAL_EXECUTABLE}")
            message(FATAL_ERROR "BRUTALIS_PLUGINVAL_EXECUTABLE does not exist: ${BRUTALIS_PLUGINVAL_EXECUTABLE}")
        endif()
        set(${out_var} "${BRUTALIS_PLUGINVAL_EXECUTABLE}" PARENT_SCOPE)
        return()
    endif()

    if(APPLE)
        set(platform macOS)
        set(relative_exe "pluginval.app/Contents/MacOS/pluginval")
    elseif(WIN32)
        set(platform Windows)
        set(relative_exe "pluginval.exe")
    else()
        set(platform Linux)
        set(relative_exe "pluginval")
    endif()

    set(cache_dir "${PROJECT_SOURCE_DIR}/.cache/pluginval/${ANOESIS_PLUGINVAL_VERSION}/${platform}")
    set(exe "${cache_dir}/${relative_exe}")

    if(NOT EXISTS "${exe}")
        set(zip "${cache_dir}/pluginval_${platform}.zip")
        set(url "https://github.com/Tracktion/pluginval/releases/download/v${ANOESIS_PLUGINVAL_VERSION}/pluginval_${platform}.zip")
        message(STATUS "Downloading pluginval ${ANOESIS_PLUGINVAL_VERSION} (${platform})")
        file(DOWNLOAD "${url}" "${zip}"
            EXPECTED_HASH SHA256=${_anoesis_pluginval_sha256_${platform}}
            STATUS download_status
            TLS_VERIFY ON)
        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            file(REMOVE "${zip}")
            message(FATAL_ERROR
                "Could not download pluginval: ${download_status}. "
                "Set BRUTALIS_PLUGINVAL_EXECUTABLE to a local copy, or BRUTALIS_PLUGINVAL=OFF.")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${cache_dir}")
        file(REMOVE "${zip}")
        if(NOT WIN32)
            file(CHMOD "${exe}" PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif()
    endif()

    set(${out_var} "${exe}" PARENT_SCOPE)
endfunction()
