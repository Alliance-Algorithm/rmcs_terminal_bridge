cmake_minimum_required(VERSION 3.22)
project(rmcs_terminal_bridge)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-O2 -Wall -Wextra -Wpedantic)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rmcs_executor REQUIRED)

# vendored 快照：桥仓自包含，不访问 Alliance-Terminal、不联网
add_subdirectory(third_party/teleop_ipc)

# 生产 Rx/Tx 与测试组件分文件；Link/payload_codec 是 header-only。
add_library(
    rmcs_terminal_bridge_component SHARED
    src/component.cpp
    src/test_components.cpp)
target_link_libraries(
    rmcs_terminal_bridge_component
    teleop::ipc
    rclcpp::rclcpp
    ${rmcs_executor_LIBRARIES}
    pluginlib::pluginlib)
target_include_directories(
    rmcs_terminal_bridge_component
    PRIVATE ${rmcs_executor_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/include)

pluginlib_export_plugin_description_file(rmcs_executor plugins.xml)

install(TARGETS rmcs_terminal_bridge_component LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES config/terminal_bridge.yaml DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
    find_package(ament_cmake_gtest REQUIRED)

    ament_add_gtest(test_payload_codec test/test_payload_codec.cpp)
    target_link_libraries(test_payload_codec rmcs_terminal_bridge_component teleop::ipc)
    target_include_directories(
        test_payload_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    ament_add_gtest(test_link test/test_link.cpp)
    target_link_libraries(test_link rmcs_terminal_bridge_component teleop::ipc)
    target_include_directories(test_link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # 无 ROS 的 Link 服务端：给客户端 ipc-probe 当对端，不链接 rclcpp。
    add_executable(bridge_link_test test/bridge_link_test.cpp)
    target_include_directories(bridge_link_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(bridge_link_test PRIVATE teleop::ipc)
endif()

ament_export_include_directories(include)
ament_export_dependencies(rclcpp pluginlib rmcs_executor)
ament_package()
