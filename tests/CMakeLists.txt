# One test executable for the whole engine; doctest discovers cases at link time.
add_executable(opentune_tests
    main.cpp
    test_skeleton.cpp
    test_signals.cpp
    test_pitch_detector_interface.cpp
    test_scale_quantizer.cpp
    test_pitch_corrector_interface.cpp
    test_params.cpp
)
target_include_directories(opentune_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/doctest
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(opentune_tests PRIVATE opentune::engine opentune_warnings)
add_test(NAME opentune_tests COMMAND opentune_tests)
