#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct PathEntry {
  size_t lineno;
  std::string key;
  std::string path;
  bool isProjectFile;
};

struct Aup2Document {
  std::filesystem::path sourcePath;
  std::optional<std::filesystem::file_time_type> loadedWriteTime;
  std::vector<std::string> lines;
  std::vector<PathEntry> entries;
  std::optional<size_t> projectFileLineno;
};

std::optional<Aup2Document> ParseAup2(const std::filesystem::path &path);

bool SaveAup2(Aup2Document &doc, const std::filesystem::path &destPath);

struct CheckResult {
  const PathEntry *entry;
  bool exists;
};

std::vector<CheckResult> CheckPaths(const Aup2Document &doc);

std::filesystem::path PathFromUtf8(const std::string &utf8);