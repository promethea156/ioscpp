# Formatting targets.
#
# These are only defined when clang-format can be found, so a normal build never
# depends on it. Set -DIOSCPP_CLANG_FORMAT=<path> to use a specific binary.
#
#   cmake --build build --target format        # rewrite the sources in place
#   cmake --build build --target format-check  # fail if anything is unformatted

find_program(IOSCPP_CLANG_FORMAT
    NAMES clang-format-19 clang-format
    HINTS
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/bin"
)

if(IOSCPP_CLANG_FORMAT)
    file(GLOB_RECURSE IOSCPP_FORMAT_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.cpp"
    )

    add_custom_target(format
        COMMAND "${IOSCPP_CLANG_FORMAT}" -i ${IOSCPP_FORMAT_SOURCES}
        COMMENT "Formatting ioscpp sources with clang-format"
        VERBATIM
    )

    add_custom_target(format-check
        COMMAND "${IOSCPP_CLANG_FORMAT}" --dry-run --Werror
            ${IOSCPP_FORMAT_SOURCES}
        COMMENT "Checking ioscpp formatting with clang-format"
        VERBATIM
    )
else()
    message(STATUS
        "clang-format not found; the 'format' and 'format-check' targets are "
        "disabled. Set -DIOSCPP_CLANG_FORMAT=<path> to enable them.")
endif()
