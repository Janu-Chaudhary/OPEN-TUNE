# The engine: pure DSP. Depends on the C++ standard library and vendored DSP
# code only (constitution IV). No platform, file-format, UI or threading headers.
add_library(opentune STATIC
    src/Version.cpp
    src/ScaleQuantizer.cpp
    src/AutocorrelationDetector.cpp
    src/ResampleCorrector.cpp
    src/SignalsmithCorrector.cpp
    src/Engine.cpp
)
target_include_directories(opentune PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# signalsmith-stretch and signalsmith-linear are vendored, unmodified
# third-party DSP code (see their VENDORED.md and
# docs/decisions/0005-signalsmith-pulled-into-stage-0.md). Unlike dr_wav,
# these ARE linked into engine/ -- constitution IV permits vendored DSP
# libraries here. SYSTEM suppresses warnings originating inside their
# headers, so -Werror et al. apply to our own code without requiring us to
# edit vendored files.
target_include_directories(opentune SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(opentune PRIVATE opentune_warnings)
add_library(opentune::engine ALIAS opentune)
target_compile_definitions(opentune PRIVATE OPENTUNE_VERSION="${PROJECT_VERSION}")
