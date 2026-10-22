cmake_minimum_required(VERSION 3.21)
project(GpuHitchLab LANGUAGES CXX)

add_executable(GpuHitchLab WIN32 src/main.cpp)
target_compile_features(GpuHitchLab PRIVATE cxx_std_17)
target_compile_definitions(GpuHitchLab PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(GpuHitchLab PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(GpuHitchLab PRIVATE d3d12 dxgi dxguid winmm shell32)
