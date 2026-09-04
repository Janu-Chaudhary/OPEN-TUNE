# opentune-cli: WAV in -> WAV out test harness (T0.11). Host code: depends on
# the engine plus dr_wav (vendored, third_party/dr_wav) for WAV I/O.
add_executable(opentune-cli main.cpp)
target_include_directories(opentune-cli PRIVATE ${CMAKE_SOURCE_DIR})
# dr_wav is vendored, unmodified third-party code (see third_party/dr_wav/VENDORED.md).
# SYSTEM suppresses warnings originating inside its headers, so -Werror et al.
# apply to our own code without requiring us to edit dr_wav.h.
target_include_directories(opentune-cli SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/dr_wav)
target_link_libraries(opentune-cli PRIVATE opentune::engine opentune_warnings)
