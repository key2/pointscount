cmake_minimum_required(VERSION 3.16)
project(pointscount LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The TikTok LIVE client library.
add_subdirectory(ttlive-cpp)

add_executable(pointscount main.cpp)
target_link_libraries(pointscount PRIVATE ttlive)
