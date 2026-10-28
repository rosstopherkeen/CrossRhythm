#pragma once

#include <BookFormatStubs.h>

#include <string>

class Xtc : public book_format_stub::Book {
 public:
  explicit Xtc(std::string filepath, const std::string& cacheDir) : Book("xtc_", std::move(filepath), cacheDir) {}
};
