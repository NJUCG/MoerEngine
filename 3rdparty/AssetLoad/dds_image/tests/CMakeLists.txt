include(FetchContent)
FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.0.1)
FetchContent_MakeAvailable(Catch2)

# We want these tests to be a optional executable.
add_executable(tests EXCLUDE_FROM_ALL)
target_compile_features(tests PRIVATE cxx_std_20)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain dds_image)
target_sources(tests PRIVATE "test.cpp")
