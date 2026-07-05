#define WIN32_LEAN_AND_MEAN
#include "Aup2Parser.h"

#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <windows.h>

struct ObjectFrameInfo {
    std::optional<int64_t> layer;
    std::optional<int64_t> frameStart;
    std::optional<int64_t> frameEnd;
};

static std::string TrimBoth(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

static bool TryParseInt(const std::string& s, int64_t& out) {
    std::string trimmed = TrimBoth(s);
    if (trimmed.empty())
        return false;
    try {
        size_t idx = 0;
        int64_t v = std::stoll(trimmed, &idx);
        if (idx != trimmed.size())
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static std::string TopLevelIdOf(const std::string& section) {
    size_t dot = section.find('.');
    return dot == std::string::npos ? section : section.substr(0, dot);
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
}

static bool IsAbsolutePath(const std::string& value) {
    try {
        return PathFromUtf8(value).is_absolute();
    } catch (...) {
        return false;
    }
}

static std::string TrimRight(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

std::optional<Aup2Document> ParseAup2(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return std::nullopt;

    {
        unsigned char bom[3]{};
        ifs.read(reinterpret_cast<char*>(bom), 3);
        if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
            ifs.seekg(0);
        }
    }

    Aup2Document doc;
    doc.sourcePath = path;
    {
        std::error_code ec;
        auto writeTime = std::filesystem::last_write_time(path, ec);
        if (!ec)
            doc.loadedWriteTime = writeTime;
    }

    std::string line;
    bool inProjectSection = false;
    std::string currentSection;
    std::string currentEffectName;
    std::unordered_map<std::string, ObjectFrameInfo> objectInfoByIndex;

    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        doc.lines.push_back(line);

        size_t lineno = doc.lines.size();

        if (!line.empty() && line.front() == '[') {
            std::string header = line;
            header.erase(0, 1);
            if (!header.empty() && header.back() == ']')
                header.pop_back();

            currentSection = header;
            currentEffectName.clear();
            inProjectSection = (line == "[project]");
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = TrimRight(line.substr(0, eq));
        std::string value = TrimRight(line.substr(eq + 1));

        if (key == "effect.name")
            currentEffectName = value;

        std::string topId = TopLevelIdOf(currentSection);
        if (key == "layer") {
            int64_t v;
            if (TryParseInt(value, v))
                objectInfoByIndex[topId].layer = v;
        } else if (key == "frame") {
            auto comma = value.find(',');
            if (comma != std::string::npos) {
                int64_t s, e;
                if (TryParseInt(value.substr(0, comma), s) &&
                    TryParseInt(value.substr(comma + 1), e)) {
                    objectInfoByIndex[topId].frameStart = s;
                    objectInfoByIndex[topId].frameEnd = e;
                }
            }
        }

        if (!IsAbsolutePath(value))
            continue;

        if (key == "output.file")
            continue;

        PathEntry entry;
        entry.lineno = lineno;
        entry.section = currentSection;
        entry.effectName = currentEffectName;
        if (auto infoIt = objectInfoByIndex.find(topId); infoIt != objectInfoByIndex.end()) {
            entry.layer = infoIt->second.layer;
            entry.frameStart = infoIt->second.frameStart;
            entry.frameEnd = infoIt->second.frameEnd;
        }
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

bool SaveAup2(Aup2Document& doc, const std::filesystem::path& destPath) {
    const std::filesystem::path bakPath = doc.sourcePath.native() + L".bak";
    try {
        std::filesystem::copy_file(
            doc.sourcePath, bakPath,
            std::filesystem::copy_options::overwrite_existing);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }

    for (const auto& entry : doc.entries) {
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

    if (!ofs.good())
        return false;
    ofs.close();
    if (!ofs)
        return false;

    std::error_code ec;
    auto writeTime = std::filesystem::last_write_time(destPath, ec);
    if (!ec)
        doc.loadedWriteTime = writeTime;
    return true;
}

std::vector<CheckResult> CheckPaths(const Aup2Document& doc) {
    std::vector<CheckResult> results;
    results.reserve(doc.entries.size());
    for (const auto& entry : doc.entries) {
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