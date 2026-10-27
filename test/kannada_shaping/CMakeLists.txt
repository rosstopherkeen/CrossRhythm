add_executable(KannadaShapeTest
  KannadaShapeTest.cpp
  ${REPO_ROOT}/lib/KannadaShape/KannadaShape.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(KannadaShapeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/KannadaShape
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(KannadaShapeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KannadaShapeTest)
