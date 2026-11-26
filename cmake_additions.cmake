# ---- Append to your existing CMakeLists.txt (UNTESTED here: no CMake/JUCE in my sandbox) ----
# Do not upgrade JUCE. Plugin target name below must match your existing juce_add_plugin() target.

option(MOBIUS_SPECTRAL_PATH "Build the legacy STFT core instead of the limiter (A/B)" OFF)
if(MOBIUS_SPECTRAL_PATH)
    target_compile_definitions(MobiusCanceller PUBLIC MOBIUS_SPECTRAL_PATH=1)
endif()

target_sources(MobiusCanceller PRIVATE Source/dsp/LimiterCore.cpp)
target_include_directories(MobiusCanceller PRIVATE Source)

# JUCE-free DSP tests + sweep CLI (CTest preserved)
enable_testing()
add_library(limiter_core STATIC Source/dsp/LimiterCore.cpp)
target_include_directories(limiter_core PUBLIC Source)
target_compile_features(limiter_core PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(limiter_core PRIVATE /W4 /WX)
else()
    target_compile_options(limiter_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()

add_executable(limiter_tests Tests/limiter_tests.cpp)
target_link_libraries(limiter_tests PRIVATE limiter_core)
add_test(NAME limiter_core_tests COMMAND limiter_tests)

add_executable(limiter_cli Tools/limiter_cli.cpp)
target_link_libraries(limiter_cli PRIVATE limiter_core)
add_test(NAME sweep_smoke
         COMMAND python3 ${CMAKE_SOURCE_DIR}/Tools/sweep.py --cli $<TARGET_FILE:limiter_cli> --quick --dur 3 --seed 1 --out ${CMAKE_BINARY_DIR}/sweep_smoke.csv)
