add_executable(
    HavkTests

    Main.cpp
    
    YsonTests.cpp
  #  DataIOTests.cpp
)
target_link_libraries(HavkTests PRIVATE doctest havk::havk havk::extensions)

target_compile_options(HavkTests PRIVATE "$<$<CONFIG:RelWithDebInfo>:-O0>")