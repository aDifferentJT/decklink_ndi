cmake_minimum_required(VERSION 3.10)

project(decklink_ndi)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if (CMAKE_SYSTEM_NAME MATCHES Darwin)
  set(PLATFORM Mac)
elseif (CMAKE_SYSTEM_NAME MATCHES Linux)
  set(PLATFORM Linux)
elseif (CMAKE_SYSTEM_NAME MATCHES Windows)
  set(PLATFORM Win)
endif()

if (CMAKE_SYSTEM_NAME MATCHES Windows)
  # Use FindIDL
  list(APPEND CMAKE_MODULE_PATH "FindIDL/cmake")
  find_package(IDL REQUIRED)
endif()

include_directories(out_ptr/include)
include_directories(decklink/${PLATFORM}/include)
include_directories(ndi/include)

add_executable(decklink_ndi main.cpp)

if (APPLE)
  find_library(FOUNDATION Foundation)
  if (NOT FOUNDATION)
    message(FATAL_ERROR "Foundation not found")
  endif()
  
  target_link_libraries(decklink_ndi PRIVATE ${FOUNDATION})
endif()

if (UNIX)
  find_library(DL dl)
  if (NOT DL)
    message(FATAL_ERROR "dl not found")
  endif()
  
  target_link_libraries(decklink_ndi PRIVATE ${DL})

  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(decklink_ndi PRIVATE Threads::Threads)
endif()

if (CMAKE_SYSTEM_NAME MATCHES Windows)
  add_idl(DeckLinkAPI decklink/Win/include/DeckLinkAPI.idl)
  target_link_libraries(decklink_ndi PRIVATE DeckLinkAPI)
endif()

