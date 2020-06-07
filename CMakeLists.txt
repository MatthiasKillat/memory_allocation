cmake_minimum_required(VERSION 2.8.3)
project(memory_allocator )

include_directories( include )

FILE(GLOB SOURCES "*.cpp")

add_executable(allocator
  ${SOURCES}
)

target_link_libraries(allocator )


