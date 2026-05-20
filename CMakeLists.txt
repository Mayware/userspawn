cmake_minimum_required(VERSION 4.1.2)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(userspawn LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

add_executable(userspawn main.cpp)

target_link_libraries(userspawn PRIVATE PkgConfig::DBUS)
