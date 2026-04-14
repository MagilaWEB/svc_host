cmake_minimum_required(VERSION 3.20)
project(SvcHost VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SvcHost is only supported on Windows")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Исходные файлы
set(SOURCES
    src/logger.cpp
    src/service_host.cpp
    src/main.cpp
)

set(HEADERS
    src/logger.hpp
    src/service_host.hpp
)

add_executable(SvcHost WIN32 ${SOURCES} ${HEADERS})
target_include_directories(SvcHost PRIVATE src)

target_link_options(SvcHost PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:
        /ENTRY:mainCRTStartup
    >
    $<$<CXX_COMPILER_ID:Clang>:
        -Wl,/ENTRY:mainCRTStartup
    >
)

target_link_libraries(SvcHost PRIVATE advapi32)
target_compile_definitions(SvcHost PRIVATE _UNICODE UNICODE)

if(MSVC)
    target_compile_options(SvcHost PRIVATE /W4 /utf-8)
else()
    target_compile_options(SvcHost PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Установка
install(TARGETS SvcHost RUNTIME DESTINATION bin)

# Копирование PDB в релизе для отладки (опционально)
if(MSVC)
    install(FILES $<TARGET_PDB_FILE:SvcHost> DESTINATION bin OPTIONAL)
endif()