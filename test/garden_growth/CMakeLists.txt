add_executable(GardenGrowthTest
  GardenGrowthTest.cpp
  ${REPO_ROOT}/src/components/themes/garden/GardenGrowth.cpp
)

target_include_directories(GardenGrowthTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(GardenGrowthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GardenGrowthTest)
