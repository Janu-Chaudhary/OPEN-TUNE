# One test executable for the whole engine; doctest discovers cases at link time.
add_executable(opentune_tests
    main.cpp
    test_skeleton.cpp
    test_signals.cpp
    test_pitch_detector_interface.cpp
    test_scale_quantizer.cpp
    test_pitch_corrector_interface.cpp
    test_params.cpp
    test_autocorrelation_detector.cpp
    test_resample_corrector.cpp
    test_engine.cpp
    test_wav_file.cpp
)
target_include_directories(opentune_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/doctest
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# dr_wav is vendored, unmodified third-party code (see third_party/dr_wav/VENDORED.md).
# SYSTEM tells the compiler to suppress warnings originating inside its headers,
# so -Werror et al. apply to our own code without requiring us to edit dr_wav.h.
target_include_directories(opentune_tests SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/dr_wav
)
target_link_libraries(opentune_tests PRIVATE opentune::engine opentune_warnings)
add_test(NAME opentune_tests COMMAND opentune_tests)
