#pragma once

// In-memory host filesystem with directory support, enough for BookCacheUtils
// and FsHelpers. Paths are stored without a trailing slash.

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace host_fs {

inline std::string normalize(const std::string& path) {
  std::string result = path;
  while (result.size() > 1 && result.back() == '/') result.pop_back();
  return result;
}

// Empty for the root and for relative single-segment paths: nothing to create.
inline std::string parentOf(const std::string& path) {
  const std::string normalized = normalize(path);
  if (normalized == "/") return "";
  const size_t slash = normalized.find_last_of('/');
  if (slash == std::string::npos) return "";
  return slash == 0 ? "/" : normalized.substr(0, slash);
}

inline std::string baseNameOf(const std::string& path) {
  const std::string normalized = normalize(path);
  const size_t slash = normalized.find_last_of('/');
  return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

struct Node {
  bool isDirectory = false;
  std::vector<uint8_t> bytes;
};

// Shared so an open handle keeps working after its map entry is replaced.
using NodePtr = std::shared_ptr<Node>;

}  // namespace host_fs

class HalStorage;

class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(std::string path, host_fs::NodePtr node, std::vector<std::string> children)
      : path_(std::move(path)), node_(std::move(node)), children_(std::move(children)) {}

  int read(void* output, const size_t length) {
    if (!node_) return 0;
    const size_t available = node_->bytes.size() - std::min(cursor_, node_->bytes.size());
    const size_t readable = std::min(length, available);
    if (readable > 0) std::copy_n(node_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(output));
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* input, const size_t length) override {
    if (!node_) return 0;
    if (cursor_ + length > node_->bytes.size()) node_->bytes.resize(cursor_ + length);
    std::copy_n(input, length, node_->bytes.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += length;
    return length;
  }
  size_t write(const void* input, const size_t length) { return write(static_cast<const uint8_t*>(input), length); }

  void flush() {}
  bool sync() const { return static_cast<bool>(node_); }
  bool close() {
    node_.reset();
    children_.clear();
    nextChild_ = 0;
    cursor_ = 0;
    return true;
  }

  bool isDirectory() const { return node_ && node_->isDirectory; }
  size_t fileSize() const { return node_ ? node_->bytes.size() : 0; }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return false; }
  explicit operator bool() const { return static_cast<bool>(node_); }

  size_t getName(char* name, const size_t len) {
    if (!node_ || !name || len == 0) return 0;
    const std::string base = host_fs::baseNameOf(path_);
    if (base.size() + 1 > len) return 0;
    std::memcpy(name, base.c_str(), base.size() + 1);
    return base.size();
  }

  HalFile openNextFile();

 private:
  std::string path_;
  host_fs::NodePtr node_;
  std::vector<std::string> children_;
  size_t nextChild_ = 0;
  size_t cursor_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  void reset() {
    nodes_.clear();
    auto root = std::make_shared<host_fs::Node>();
    root->isDirectory = true;
    nodes_["/"] = std::move(root);
  }

  bool exists(const char* path) const { return path && nodes_.count(host_fs::normalize(path)) > 0; }

  bool mkdir(const char* path, const bool pFlag = true) {
    if (!path || path[0] == '\0') return false;
    const std::string target = host_fs::normalize(path);
    if (const auto found = nodes_.find(target); found != nodes_.end()) return found->second->isDirectory;
    const std::string parent = host_fs::parentOf(target);
    if (!parent.empty() && !exists(parent.c_str())) {
      if (!pFlag || !mkdir(parent.c_str(), true)) return false;
    }
    auto node = std::make_shared<host_fs::Node>();
    node->isDirectory = true;
    nodes_[target] = std::move(node);
    return true;
  }

  bool remove(const char* path) {
    if (!path) return false;
    const auto found = nodes_.find(host_fs::normalize(path));
    if (found == nodes_.end() || found->second->isDirectory) return false;
    nodes_.erase(found);
    return true;
  }

  bool removeDir(const char* path) {
    if (!path || path[0] == '\0') return false;
    const std::string target = host_fs::normalize(path);
    const auto found = nodes_.find(target);
    if (found == nodes_.end() || !found->second->isDirectory) return false;
    for (const std::string& child : childrenOf(target)) {
      const auto childNode = nodes_.find(child);
      if (childNode == nodes_.end()) continue;
      if (childNode->second->isDirectory) {
        if (!removeDir(child.c_str())) return false;
      } else if (!remove(child.c_str())) {
        return false;
      }
    }
    nodes_.erase(target);
    return true;
  }

  bool rename(const char* from, const char* to) {
    if (!from || !to) return false;
    const std::string source = host_fs::normalize(from);
    const std::string destination = host_fs::normalize(to);
    const auto found = nodes_.find(source);
    if (found == nodes_.end() || nodes_.count(destination) > 0) return false;
    const std::string parent = host_fs::parentOf(destination);
    if (!parent.empty() && !exists(parent.c_str())) return false;
    nodes_[destination] = found->second;
    nodes_.erase(source);
    return true;
  }

  HalFile open(const char* path) {
    if (!path) return {};
    const std::string target = host_fs::normalize(path);
    const auto found = nodes_.find(target);
    if (found == nodes_.end()) return {};
    return HalFile(target, found->second, found->second->isDirectory ? childrenOf(target) : std::vector<std::string>{});
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    const std::string target = host_fs::normalize(path);
    const std::string parent = host_fs::parentOf(target);
    if (!parent.empty() && !exists(parent.c_str())) return false;
    if (const auto found = nodes_.find(target); found != nodes_.end() && found->second->isDirectory) return false;
    auto node = std::make_shared<host_fs::Node>();
    nodes_[target] = node;
    file = HalFile(target, std::move(node), {});
    return true;
  }

  // Test helpers.
  void putFile(const std::string& path, const std::vector<uint8_t>& data) {
    mkdir(host_fs::parentOf(path).c_str(), true);
    auto node = std::make_shared<host_fs::Node>();
    node->bytes = data;
    nodes_[host_fs::normalize(path)] = std::move(node);
  }
  std::vector<uint8_t> bytes(const std::string& path) const {
    const auto found = nodes_.find(host_fs::normalize(path));
    return found == nodes_.end() ? std::vector<uint8_t>{} : found->second->bytes;
  }
  std::vector<std::string> childrenOf(const std::string& directory) const {
    std::vector<std::string> children;
    const std::string prefix = host_fs::normalize(directory) == "/" ? "/" : host_fs::normalize(directory) + "/";
    for (const auto& [path, node] : nodes_) {
      if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) continue;
      if (path.find('/', prefix.size()) != std::string::npos) continue;
      children.push_back(path);
    }
    return children;
  }

 private:
  std::map<std::string, host_fs::NodePtr> nodes_;
};

inline HalStorage Storage;

inline HalFile HalFile::openNextFile() {
  while (nextChild_ < children_.size()) {
    const std::string child = children_[nextChild_++];
    HalFile entry = Storage.open(child.c_str());
    if (entry) return entry;
  }
  return {};
}

inline void yield() {}
