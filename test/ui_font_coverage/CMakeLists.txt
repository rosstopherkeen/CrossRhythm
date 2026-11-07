add_executable(UiFontCoverageTest
  UiFontCoverageTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(UiFontCoverageTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)
target_compile_definitions(UiFontCoverageTest PRIVATE TRANSLATIONS_DIR="${REPO_ROOT}/lib/I18n/translations"
  SRC_DIR="${REPO_ROOT}/src")
target_link_libraries(UiFontCoverageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(UiFontCoverageTest)
