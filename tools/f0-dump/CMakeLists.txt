# opentune-f0dump: dumps a detector's per-block pitch estimate as CSV (T1.0).
# Host code: engine + dr_wav for WAV I/O, reusing autotune-cli's WavFile.h.
add_executable(opentune-f0dump main.cpp)
target_include_directories(opentune-f0dump PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(opentune-f0dump SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/dr_wav)
target_link_libraries(opentune-f0dump PRIVATE opentune::engine opentune_warnings)
