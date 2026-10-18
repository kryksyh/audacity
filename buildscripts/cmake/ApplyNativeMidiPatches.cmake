# Keep the native MIDI integration reproducible from the pinned upstream
# submodules. All changes live in this Audacity repository; no forked framework
# or dependency commits are needed when checking out or building this branch.
include_guard(GLOBAL)
find_package(Git REQUIRED)
get_filename_component(_native_midi_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(_apply_native_midi_patch submodule patch_name)
    set(_directory "${_native_midi_root}/${submodule}")
    set(_patch "${_native_midi_root}/buildscripts/patches/${patch_name}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_directory}" apply --reverse --check "${_patch}"
        RESULT_VARIABLE _already_applied
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_already_applied EQUAL 0)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_directory}" apply --check "${_patch}"
        RESULT_VARIABLE _can_apply
        ERROR_VARIABLE _error
    )
    if(NOT _can_apply EQUAL 0)
        message(FATAL_ERROR "Cannot apply native MIDI changes to ${submodule}. Use the pinned submodule revision or resolve local edits.\n${_error}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_directory}" apply "${_patch}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    message(STATUS "Applied native MIDI integration changes to ${submodule}")
endfunction()

_apply_native_midi_patch(muse muse-native-midi.patch)
_apply_native_midi_patch(muse_deps muse-deps-native-midi.patch)
