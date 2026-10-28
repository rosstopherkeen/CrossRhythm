#pragma once

#include <BookFormatStubs.h>

#include <string>

class Txt : public book_format_stub::Book {
 public:
  explicit Txt(std::string filepath, std::string cacheBasePath) : Book("txt_", std::move(filepath), cacheBasePath) {}
};
