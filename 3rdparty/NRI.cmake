# NRI
add_subdirectory(NRI)

# 修复NRI使用D3D12AgilitySDK，而不是系统WindowsSDK的问题
target_include_directories(NRI_D3D12 BEFORE PRIVATE 
    "${MOER_AGILITYSDK_DIR}/build/native/include"
)

target_compile_options(NRI
PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"

)
target_compile_options(NRI_Shared
PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
)
target_compile_options(NRI_NONE
PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
)
target_compile_options(
    NRI_D3D11
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-microsoft-extra-qualification>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-ignored-attributes>"
)

target_compile_options(
    NRI_D3D12
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-nullability-completeness>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-ignored-attributes>"
)

target_compile_options(
    NRI_VK
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-nullability-completeness>"
)

target_compile_options(
    NRI_Validation
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-msse4.1)
endif()
