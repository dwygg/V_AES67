cmake_minimum_required(VERSION 3.16)
project(AES67Panel VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(aes67_panel WIN32
    main.cpp
    mainwindow.cpp
    pipe_client.cpp
)

target_link_libraries(aes67_panel PRIVATE Qt6::Widgets)

# Win32 named pipe needs no extra libs (kernel32)
