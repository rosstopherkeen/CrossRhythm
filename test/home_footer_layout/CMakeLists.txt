add_executable(HomeFooterLayoutTest HomeFooterLayoutTest.cpp)

target_include_directories(HomeFooterLayoutTest PRIVATE ${REPO_ROOT}/src)

target_link_libraries(HomeFooterLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeFooterLayoutTest)
