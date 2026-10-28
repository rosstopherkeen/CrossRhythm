#pragma once

#include <string>

class String {
 public:
  String() = default;
  // Explicit so std::string arguments do not make the String and
  // string_view FsHelpers overloads ambiguous.
  explicit String(const char* s) : value_(s ? s : "") {}
  explicit String(const std::string& s) : value_(s) {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};
