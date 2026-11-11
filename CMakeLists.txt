cmake_minimum_required(VERSION 3.15)
project(ExplorerGitStatus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Portable core: status parsing and the non-blocking cache.
add_library(gitstatus_core STATIC
    src/GitStatus.cpp
    src/StatusCache.cpp
)
target_include_directories(gitstatus_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(gitstatus_core PUBLIC Threads::Threads)
set_target_properties(gitstatus_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    enable_language(RC)

    add_library(ExplorerGitStatus SHARED
        src/Dll.cpp
        src/GitQuery.cpp
        src/OverlayHandler.cpp
        src/ExplorerGitStatus.def
        src/ExplorerGitStatus.rc
    )
    target_include_directories(ExplorerGitStatus PRIVATE icons)
    target_compile_definitions(ExplorerGitStatus PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                                         _WIN32_WINNT=0x0601)
    target_link_libraries(ExplorerGitStatus PRIVATE gitstatus_core shell32 ole32 advapi32 user32)
    set_target_properties(ExplorerGitStatus PROPERTIES PREFIX "")

    if(MSVC)
        # Static CRT so the DLL has no runtime redistributable dependency.
        set_property(TARGET ExplorerGitStatus gitstatus_core PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(ExplorerGitStatus PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(gitstatus_tests tests/tests.cpp)
    target_link_libraries(gitstatus_tests PRIVATE gitstatus_core)
    if(MSVC)
        set_property(TARGET gitstatus_tests PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME gitstatus_tests COMMAND gitstatus_tests)
endif()
