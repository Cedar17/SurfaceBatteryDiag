cmake_minimum_required(VERSION 3.20)
project(SurfaceBatteryDiag LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(SurfaceBatteryDiag src/main.cpp)
target_compile_features(SurfaceBatteryDiag PRIVATE cxx_std_17)
target_compile_definitions(SurfaceBatteryDiag PRIVATE UNICODE _UNICODE)
target_link_libraries(SurfaceBatteryDiag PRIVATE setupapi)

if(MSVC)
  target_compile_options(SurfaceBatteryDiag PRIVATE /W4 /EHsc /utf-8)
endif()
