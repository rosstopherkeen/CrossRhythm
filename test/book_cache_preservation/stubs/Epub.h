#pragma once

#include <BookFormatStubs.h>

#include <string>

class Epub : public book_format_stub::Book {
 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : Book("epub_", std::move(filepath), cacheDir) {}
};
