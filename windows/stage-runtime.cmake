cmake_minimum_required(VERSION 3.20)

if(PSXRC_OBJDUMP)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${PSXRC_OBJDUMP}")
endif()

# Scan apart from any DLLs staged by an earlier build.
get_filename_component(executable_name "${PSXRC_EXECUTABLE}" NAME)
file(COPY "${PSXRC_EXECUTABLE}" DESTINATION "${PSXRC_SCAN_DIR}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${PSXRC_SCAN_DIR}/${executable_name}"
    DIRECTORIES ${PSXRC_SEARCH_DIRS}
    RESOLVED_DEPENDENCIES_VAR dependencies
    PRE_EXCLUDE_REGEXES "^api-ms-win-" "^ext-ms-win-"
    POST_EXCLUDE_REGEXES "[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\]" "[/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\]")

file(COPY "${PSXRC_EXECUTABLE}" ${dependencies} DESTINATION "${PSXRC_DESTINATION}")
