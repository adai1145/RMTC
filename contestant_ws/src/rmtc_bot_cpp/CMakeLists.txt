cmake_minimum_required(VERSION 3.8)
project(rmtc_bot_cpp)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -O2)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# 纯逻辑核心库：不依赖 rclpy/rclcpp，与 Python 版分层一致
add_library(rmtc_bot_cpp_core
  src/protocol.cpp
  src/world.cpp
  src/planner.cpp
  src/llm_client.cpp
  src/strategy.cpp)
target_include_directories(rmtc_bot_cpp_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(rmtc_bot_cpp_core PUBLIC CURL::libcurl Threads::Threads)

add_executable(rmtc_bot_cpp src/main.cpp)
target_link_libraries(rmtc_bot_cpp rmtc_bot_cpp_core)
ament_target_dependencies(rmtc_bot_cpp rclcpp std_msgs ament_index_cpp)

install(TARGETS rmtc_bot_cpp rmtc_bot_cpp_core
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
install(PROGRAMS run_contestant_cpp.sh DESTINATION lib/${PROJECT_NAME})

ament_package()
