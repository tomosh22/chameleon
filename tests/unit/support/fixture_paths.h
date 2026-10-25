#pragma once

// Locates fixture build outputs for the COFF/PE tests. CMake writes a `key=value` text file at
// build-generation time (see tests/unit/coff/CMakeLists.txt); its path is CML_TEST_PATHS_FILE.
// Values that are lists (object files) are `;`-separated.

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef CML_TEST_PATHS_FILE
#define CML_TEST_PATHS_FILE ""
#endif

namespace cml_test {

class FixturePaths {
public:
    static const FixturePaths& get() {
        static const FixturePaths instance;
        return instance;
    }

    bool loaded() const { return loaded_; }

    std::string value(const std::string& key) const {
        const auto it = values_.find(key);
        return it == values_.end() ? std::string() : it->second;
    }

    std::string config() const { return value("config"); }

    // Path of the object whose file name is `file_name` (case-insensitive) in list `key`.
    std::string object(const std::string& key, const std::string& file_name) const {
        std::string list = value(key);
        std::size_t start = 0;
        while (start <= list.size()) {
            std::size_t end = list.find(';', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            const std::string path = list.substr(start, end - start);
            if (!path.empty() && iequals(basename(path), file_name)) {
                return path;
            }
            start = end + 1;
        }
        return std::string();
    }

    static std::string basename(const std::string& path) {
        const std::size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
            const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
            if (ca != cb) {
                return false;
            }
        }
        return true;
    }

private:
    FixturePaths() {
        std::ifstream in(CML_TEST_PATHS_FILE);
        if (!in) {
            return;
        }
        loaded_ = true;
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos) {
                values_[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
    }

    bool loaded_ = false;
    std::map<std::string, std::string> values_;
};

}  // namespace cml_test
