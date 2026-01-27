cmake_minimum_required(VERSION 3.28)

project(Havk VERSION 1.0.0 LANGUAGES CXX)

set(HAVK_ENABLE_GFX_EXTENSIONS ${PROJECT_IS_TOP_LEVEL}          CACHE STRING "Enable graphics extension libraries")
set(HAVK_ENABLE_GLFW           ${HAVK_ENABLE_GFX_EXTENSIONS}    CACHE STRING "Import GLFW for `MainWindow.h` and ImGui backend.")

if (NOT CPM_INITIALIZED)
    # Set local cache to speed up configures, because for some reason that's not the default.
    # https://github.com/cpm-cmake/CPM.cmake/pull/669
    if (NOT DEFINED ENV{CPM_SOURCE_CACHE} AND NOT CPM_SOURCE_CACHE)
        set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/deps/" CACHE PATH "Dependency cache directory" FORCE)
    endif()

    include("CPM.cmake")
endif()

# Dependencies
find_package(Vulkan REQUIRED)

CPMAddPackage("gh:GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator#v3.3.0")
CPMAddPackage("gh:g-truc/glm#1.0.1")

## Slang
add_library(slang::slang SHARED IMPORTED)
cmake_path(GET Vulkan_INCLUDE_DIR PARENT_PATH vulkan_sdk_dir)

if (EXISTS "${vulkan_sdk_dir}/include/slang/slang.h")
    set_target_properties(slang::slang PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${vulkan_sdk_dir}/include/slang")
    set(slang_SOURCE_DIR ${vulkan_sdk_dir})
else()
    set(slang_version "2025.24.3")

    if (CMAKE_HOST_WIN32)
        set(slang_package_platform_name "windows-x86_64")
    elseif (CMAKE_HOST_LINUX)
        set(slang_package_platform_name "linux-x86_64-glibc-2.27")
    else()
        message(ERROR_FATAL "Don't know how to setup slang on this OS")
    endif()

    CPMAddPackage(
        NAME slang
        URL "https://github.com/shader-slang/slang/releases/download/v${slang_version}/slang-${slang_version}-${slang_package_platform_name}.zip"
        VERSION ${slang_version}
    )
    set_target_properties(slang::slang PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${slang_SOURCE_DIR}/include")
endif()

if (CMAKE_HOST_WIN32)
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_IMPLIB     "${slang_SOURCE_DIR}/lib/slang-compiler.lib"
        IMPORTED_LOCATION   "${slang_SOURCE_DIR}/bin/slang-compiler.dll"
    )
    install(IMPORTED_RUNTIME_ARTIFACTS slang::slang RUNTIME)
elseif (CMAKE_HOST_LINUX)
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION   "${slang_SOURCE_DIR}/lib/libslang-compiler.so"
    )
endif()

if (HAVK_ENABLE_GFX_EXTENSIONS)
    CPMAddPackage("gh:ocornut/imgui#v1.92.5-docking")
    CPMAddPackage("gh:epezent/implot#v0.17")

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${implot_SOURCE_DIR}/implot.cpp
        ${implot_SOURCE_DIR}/implot_items.cpp
        ${implot_SOURCE_DIR}/implot_demo.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${implot_SOURCE_DIR})
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMPLOT_NO_FORCE_INLINE)

    if (HAVK_ENABLE_GLFW)
        find_package(glfw3 QUIET)
        if (NOT glfw3_FOUND)
            CPMAddPackage("gh:glfw/glfw#3.4")
        endif()

        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
        target_link_libraries(imgui PUBLIC glfw)

        # Needed since v1.92.4 for ImGui_ImplGlfw_SetWindowFloating()
        if (CMAKE_HOST_LINUX)
            target_link_libraries(imgui PRIVATE X11)
        endif()
    endif()
endif()

add_subdirectory("src/")

if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory("samples/")

    CPMAddPackage("gh:doctest/doctest#v2.4.12")
    add_subdirectory("tests/")
endif()