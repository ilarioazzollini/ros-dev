cmake_minimum_required(VERSION 3.8)
project(issue_2981_demo)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)

add_executable(load_demo src/load_demo.cpp)
target_link_libraries(load_demo rclcpp::rclcpp)

add_executable(param_holder_node src/param_holder_node.cpp)
target_link_libraries(param_holder_node rclcpp::rclcpp)

add_executable(switch_config_demo src/switch_config_demo.cpp)
target_link_libraries(switch_config_demo rclcpp::rclcpp)

add_executable(self_reload_demo src/self_reload_demo.cpp)
target_link_libraries(self_reload_demo rclcpp::rclcpp)

add_executable(param_holder_node_save src/param_holder_node_save.cpp)
target_link_libraries(param_holder_node_save rclcpp::rclcpp)

install(TARGETS
  load_demo
  param_holder_node
  switch_config_demo
  self_reload_demo
  param_holder_node_save
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY params
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
