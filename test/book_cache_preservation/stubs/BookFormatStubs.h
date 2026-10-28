#pragma once

// Minimal stand-ins for the format classes BookCacheUtils uses to locate and
// clear a book's cache directory. The real hash is irrelevant here; the test
// only needs a stable cache path per book path.

#include <HalStorage.h>

#include <string>

namespace book_format_stub {

inline std::string cachePathFor(const std::string& prefix, const std::string& filepath,
                                const std::string& cacheBasePath) {
  const size_t slash = filepath.find_last_of('/');
  const std::string name = slash == std::string::npos ? filepath : filepath.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  return cacheBasePath + "/" + prefix + (dot == std::string::npos ? name : name.substr(0, dot));
}

class Book {
 public:
  Book(const std::string& prefix, std::string filepath, const std::string& cacheBasePath)
      : cachePath(cachePathFor(prefix, filepath, cacheBasePath)) {}

  const std::string& getCachePath() const { return cachePath; }
  bool clearCache() const { return !Storage.exists(cachePath.c_str()) || Storage.removeDir(cachePath.c_str()); }

 private:
  std::string cachePath;
};

}  // namespace book_format_stub
