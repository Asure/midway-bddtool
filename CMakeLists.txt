cmake_minimum_required(VERSION 3.16)
project(bddview C)

# SDL2 ---------------------------------------------------------------
# Windows: pass -DSDL2_DIR=<SDL2-devel-VC>/cmake on the command line
#          (build.ps1 does this automatically)
# Linux:   sudo apt install libsdl2-dev
if(WIN32)
    find_package(SDL2 REQUIRED)
else()
    find_package(SDL2 REQUIRED)
    if(NOT SDL2_FOUND)
        # pkg-config fallback
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
endif()

# Target -------------------------------------------------------------
add_executable(bddview bddview.c)

target_include_directories(bddview PRIVATE ${SDL2_INCLUDE_DIRS})
if(TARGET SDL2::SDL2main)
    target_link_libraries(bddview PRIVATE SDL2::SDL2main SDL2::SDL2)
else()
    target_link_libraries(bddview PRIVATE ${SDL2_LIBRARIES})
endif()

# Compiler flags -----------------------------------------------------
if(MSVC)
    target_compile_options(bddview PRIVATE /W3)
    # SDL2main bridges WinMain → main on Windows
    set_target_properties(bddview PROPERTIES WIN32_EXECUTABLE TRUE)
    # comdlg32 for GetOpenFileName (TGA import dialog)
    target_link_libraries(bddview PRIVATE comdlg32)
else()
    target_compile_options(bddview PRIVATE -Wall -Wextra -std=c99)
    target_link_options(bddview PRIVATE -no-pie)
endif()

# On Windows, copy SDL2.dll next to the exe after build
if(WIN32 AND SDL2_RUNTIME_LIBRARY_DIRS)
    add_custom_command(TARGET bddview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_RUNTIME_LIBRARY_DIRS}/SDL2.dll"
            $<TARGET_FILE_DIR:bddview>)
endif()
