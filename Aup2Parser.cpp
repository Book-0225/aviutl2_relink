#include "Aup2Parser.h"
#include <fstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

std::filesystem::path PathFromUtf8(const std::string &utf8) {
  return std::filesystem::path(reinterpret_cast<const char8_t *>(utf8.c_str()));
}

static bool IsAbsolutePath(const std::string &value) {
  try {
    return PathFromUtf8(value).is_absolute();
  } catch (...) {
    return false;
  }
}

static std::string TrimRight(const std::string &s) {
  size_t end = s.find_last_not_of(" \t\r\n");
  return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

std::optional<Aup2Document> ParseAup2(const std::filesystem::path &path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs)
    return std::nullopt;

  {
    unsigned char bom[3]{};
    ifs.read(reinterpret_cast<char *>(bom), 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
      ifs.seekg(0);
    }
  }

  Aup2Document doc;
  doc.sourcePath = path;

  std::string line;
  bool inProjectSection = false;

  while (std::getline(ifs, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    doc.lines.push_back(line);

    size_t lineno = doc.lines.size();

    if (!line.empty() && line.front() == '[') {
      inProjectSection = (line == "[project]");
      continue;
    }

    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;

    std::string key = TrimRight(line.substr(0, eq));
    std::string value = TrimRight(line.substr(eq + 1));

    if (!IsAbsolutePath(value))
      continue;

    if (key == "output.file")
      continue;

    PathEntry entry;
    entry.lineno = lineno;
    entry.key = key;
    entry.path = value;
    entry.isProjectFile = inProjectSection && (key == "file" || key == "File");

    if (entry.isProjectFile) {
      doc.projectFileLineno = lineno;
    }

    doc.entries.push_back(std::move(entry));
  }

  return doc;
}

bool SaveAup2(Aup2Document &doc, const std::filesystem::path &destPath) {
  const std::filesystem::path bakPath = doc.sourcePath.native() + L".bak";
  try {
    std::filesystem::copy_file(
        doc.sourcePath, bakPath,
                  std::filesystem::copy_options::overwrite_existing);
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }

  for (const auto &entry : doc.entries) {
    size_t idx = entry.lineno - 1;
    if (idx >= doc.lines.size())
      continue;

    doc.lines[idx] = entry.key + "=" + entry.path;
  }

  std::ofstream ofs(destPath, std::ios::binary);
  if (!ofs)
    return false;

  for (size_t i = 0; i < doc.lines.size(); ++i) {
    ofs << doc.lines[i];
    if (i + 1 < doc.lines.size())
      ofs << "\r\n";
  }

  return ofs.good();
}

std::vector<CheckResult> CheckPaths(const Aup2Document &doc) {
  std::vector<CheckResult> results;
  results.reserve(doc.entries.size());
  for (const auto &entry : doc.entries) {
    CheckResult r;
    r.entry = &entry;
    try {
      r.exists = std::filesystem::exists(PathFromUtf8(entry.path));
    } catch (...) {
      r.exists = false;
    }
    results.push_back(r);
  }
  return results;
}