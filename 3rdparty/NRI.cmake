# NRI
add_subdirectory(NRI)
target_compile_options(NRI
        PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
)
target_compile_options(NRI_Shared
        PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
)
target_compile_options(NRI_NONE
        PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
)
target_compile_options(
    NRI_D3D11
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-microsoft-extra-qualification>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-ignored-attributes>"
)

target_compile_options(
    NRI_D3D12
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-nullability-completeness>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-ignored-attributes>"
)

target_compile_options(
    NRI_VK
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-nullability-completeness>"
)

target_compile_options(
    NRI_Validation
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-msse4.1)
endif()
