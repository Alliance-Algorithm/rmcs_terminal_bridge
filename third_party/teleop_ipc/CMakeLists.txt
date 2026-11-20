# header-only：只有头文件，没有编译单元；可被别的项目 add_subdirectory/FetchContent 消费
add_library(teleop_ipc INTERFACE)
add_library(teleop::ipc ALIAS teleop_ipc)

target_compile_features(teleop_ipc INTERFACE cxx_std_20)

# ucred / accept4 / SOCK_CLOEXEC 需要 GNU 扩展
target_compile_definitions(teleop_ipc INTERFACE _GNU_SOURCE)

target_include_directories(
    teleop_ipc
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# vendored tl::expected（CC0-1.0），头文件警告不参与本仓库的告警策略
target_include_directories(
    teleop_ipc
    SYSTEM INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
