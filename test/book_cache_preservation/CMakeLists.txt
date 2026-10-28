add_executable(BookCachePreservationTest
  BookCachePreservationTest.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# Stubs first so HalStorage/Epub/Txt/Xtc resolve to host fakes.
target_include_directories(BookCachePreservationTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
target_include_directories(BookCachePreservationTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BookCachePreservationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCachePreservationTest)
