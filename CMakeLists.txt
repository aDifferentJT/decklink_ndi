cmake_minimum_required(VERSION 3.10)

project(decklink_ndi)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(out_ptr/include)
include_directories(decklink/${PLATFORM}/include)
include_directories(ndi/include)

add_executable(decklink_ndi main.cpp)

if (${PLATFORM} MATCHES Mac)
find_library(FOUNDATION Foundation)
if (NOT FOUNDATION)
  message(FATAL_ERROR "Foundation not found")
endif()

target_link_libraries(decklink_ndi ${FOUNDATION})
endif()

