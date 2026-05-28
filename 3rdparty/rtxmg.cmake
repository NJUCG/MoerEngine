## RTXMG是CLAS拓展的Sample，而不是SDK，所以不应该直接接入。此处RTXMG相关代码暂时废弃

## 当前模块只从主 CMake 配置流程调用，直接复用顶层已经解析好的路径变量
set(moer_rtxmg_checkout_dir "${moer_third_party_dir}/rtxmg")
set(moer_rtxmg_python_executable "${moer_third_party_dir}/python312/x64/python.exe")
set(moer_rtxmg_python_script "${moer_root_dir}/tools/3rdparty/rtxmg.py")

foreach(path
    ${moer_rtxmg_python_executable}
    ${moer_rtxmg_python_script}
)
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "Required RTXMG bootstrap path does not exist: ${path}")
    endif()
endforeach()

## 如果 RTXMG 已经是一个完整 git checkout，则直接跳过，避免每次 configure 都重新执行 clone
if (EXISTS "${moer_rtxmg_checkout_dir}/.git")
    message(STATUS "RTXMG checkout already exists: ${moer_rtxmg_checkout_dir}")
    return()
endif()

## 如果目标目录已经存在但不是 git checkout，说明当前状态不明确，先拒绝继续覆盖
if (EXISTS "${moer_rtxmg_checkout_dir}")
    file(GLOB moer_rtxmg_checkout_children LIST_DIRECTORIES TRUE "${moer_rtxmg_checkout_dir}/*")
    if (moer_rtxmg_checkout_children)
        message(FATAL_ERROR "RTXMG checkout directory exists but is not a git repository: ${moer_rtxmg_checkout_dir}")
    endif()
endif()

## 调用 Python 脚本执行最小 RTXMG clone 流程
message(STATUS "Running RTXMG bootstrap script: ${moer_rtxmg_python_script}")
execute_process(
    COMMAND "${moer_rtxmg_python_executable}" -u "${moer_rtxmg_python_script}"
    WORKING_DIRECTORY "${moer_root_dir}"
    RESULT_VARIABLE moer_rtxmg_bootstrap_result
    ECHO_OUTPUT_VARIABLE
    ECHO_ERROR_VARIABLE
    COMMAND_ECHO STDOUT
)

if (NOT moer_rtxmg_bootstrap_result EQUAL 0)
    message(FATAL_ERROR "RTXMG bootstrap failed with exit code: ${moer_rtxmg_bootstrap_result}")
endif()

if (NOT EXISTS "${moer_rtxmg_checkout_dir}/.git")
    message(FATAL_ERROR "RTXMG bootstrap completed but no git checkout was found: ${moer_rtxmg_checkout_dir}")
endif()

message(STATUS "RTXMG checkout is ready: ${moer_rtxmg_checkout_dir}")