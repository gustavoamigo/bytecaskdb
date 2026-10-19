// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// test_util.cppm — a scratch directory and a bytes() literal for the tests.

module;
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <string_view>

export module bytecask.sql.tests.util;

export import bytecask.sql.key_codec;

export namespace sql_test {

// A fresh directory under the system temp dir, removed on destruction.
class TempDir {
public:
  explicit TempDir(std::string_view tag)
      : path_{std::filesystem::temp_directory_path() /
              (std::string{"sql_layer_"} + std::string{tag} + "_" + std::to_string(std::random_device{}()))} {
    std::filesystem::remove_all(path_);
  }
  ~TempDir() { std::filesystem::remove_all(path_); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

private:
  std::filesystem::path path_;
};

inline auto bytes(std::string_view s) -> bytecask::sql::Bytes {
  auto v = std::as_bytes(std::span{s.data(), s.size()});
  return {v.begin(), v.end()};
}

}  // namespace sql_test
