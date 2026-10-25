#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "cml/util/result.h"

namespace cml::pdb::detail {

// A read-only memory-mapped file. Windows: CreateFileMapping / MapViewOfFile with all share modes
// allowed. POSIX (portable syntax-check builds only): mmap.
class MappedFile {
public:
    static Result<std::shared_ptr<const MappedFile>> open(const std::string& path_utf8);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* data() const noexcept { return data_; }
    std::uint64_t size() const noexcept { return size_; }
    const std::string& path() const noexcept { return path_; }

private:
    MappedFile() = default;
    const std::uint8_t* data_ = nullptr;
    std::uint64_t size_ = 0;
    std::string path_;
    [[maybe_unused]] void* os_handle_a_ = nullptr;  // Windows: file handle
    [[maybe_unused]] void* os_handle_b_ = nullptr;  // Windows: mapping handle
};

// True when `path_utf8` names an existing regular file.
bool file_exists(const std::string& path_utf8);

// Joins a directory and a file name with a native-style separator ("\\" when the directory uses
// it, "/" otherwise). Directory may be empty.
std::string join_path(const std::string& dir, const std::string& name);
// Directory part (without trailing separator); empty when there is none.
std::string dir_name(const std::string& path);
// File name part (after the last '\\' or '/').
std::string file_name(const std::string& path);

}  // namespace cml::pdb::detail
