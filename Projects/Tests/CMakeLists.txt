file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    *.cpp
)

add_executable(fbzz_tests ${TEST_SOURCES})

target_link_libraries(fbzz_tests
    PRIVATE
        fbzz_physics
)

add_test(NAME fbzz_tests COMMAND fbzz_tests)
