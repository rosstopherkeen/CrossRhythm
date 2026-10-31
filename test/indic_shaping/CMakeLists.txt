add_executable(IndicShapeTest
  IndicShapeTest.cpp
  ${REPO_ROOT}/lib/IndicShape/IndicShape.cpp
  ${REPO_ROOT}/lib/IndicShape/IndicTables.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)

target_include_directories(IndicShapeTest PRIVATE
  stubs
  ${REPO_ROOT}/include
  ${REPO_ROOT}/lib/IndicShape
  ${REPO_ROOT}/lib/Utf8)

# The test reads the committed .cpshape files -- the same bytes build-sd-fonts.py
# packs into each font folder -- through the device parser.
target_compile_definitions(IndicShapeTest PRIVATE
  INDIC_SHAPING_DIR="${REPO_ROOT}/lib/EpdFont/scripts/shaping")

target_link_libraries(IndicShapeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main)

gtest_discover_tests(IndicShapeTest)
