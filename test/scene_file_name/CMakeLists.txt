add_executable(SceneFileNameTest
  SceneFileNameTest.cpp
)

target_include_directories(SceneFileNameTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SceneFileNameTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SceneFileNameTest)
