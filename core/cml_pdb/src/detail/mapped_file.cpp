#include "detail/mapped_file.h"

#include <cstring>
#include <string>

#include "cml/pdb/diag_ids.h"
#include "detail/counting.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace cml::pdb::detail {

namespace {

Diag cannot_open(const std::string& path, const char* what, unsigned long os_error) {
    return Diag::make(diag::kCannotOpenFile,
                      "Cannot open '" + path + "': " + what + " (os error " + std::to_string(os_error) + ").",
                      "Check that the file exists, is readable and is not locked exclusively by another process.");
}

#if defined(_WIN32)
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}
#endif

}  // namespace

Result<std::shared_ptr<const MappedFile>> MappedFile::open(const std::string& path_utf8) {
    std::shared_ptr<MappedFile> file(new MappedFile());
    file->path_ = path_utf8;
#if defined(_WIN32)
    const std::wstring wide = widen(path_utf8);
    if (wide.empty()) {
        return cannot_open(path_utf8, "empty or invalid path", 0);
    }
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return cannot_open(path_utf8, "CreateFile failed", GetLastError());
    }
    file->os_handle_a_ = h;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        return cannot_open(path_utf8, "GetFileSizeEx failed", GetLastError());
    }
    if (size.QuadPart <= 0) {
        return Diag::make(diag::kNotAPdb, "'" + path_utf8 + "' is empty.",
                          "Point Chameleon at the .pdb the linker produced.");
    }
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m == nullptr) {
        return cannot_open(path_utf8, "CreateFileMapping failed", GetLastError());
    }
    file->os_handle_b_ = m;
    void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        return cannot_open(path_utf8, "MapViewOfFile failed", GetLastError());
    }
    file->data_ = static_cast<const std::uint8_t*>(view);
    file->size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
    const int fd = ::open(path_utf8.c_str(), O_RDONLY);
    if (fd < 0) {
        return cannot_open(path_utf8, "open failed", static_cast<unsigned long>(errno));
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        const bool empty = st.st_size == 0;
        ::close(fd);
        if (empty) {
            return Diag::make(diag::kNotAPdb, "'" + path_utf8 + "' is empty.",
                              "Point Chameleon at the .pdb the linker produced.");
        }
        return cannot_open(path_utf8, "fstat failed", static_cast<unsigned long>(errno));
    }
    void* view = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (view == MAP_FAILED) {
        return cannot_open(path_utf8, "mmap failed", static_cast<unsigned long>(errno));
    }
    file->data_ = static_cast<const std::uint8_t*>(view);
    file->size_ = static_cast<std::uint64_t>(st.st_size);
#endif
    note_map(static_cast<std::size_t>(file->size_));
    return std::shared_ptr<const MappedFile>(std::move(file));
}

MappedFile::~MappedFile() {
#if defined(_WIN32)
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
        note_unmap(static_cast<std::size_t>(size_));
    }
    if (os_handle_b_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(os_handle_b_));
    }
    if (os_handle_a_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(os_handle_a_));
    }
#else
    if (data_ != nullptr) {
        ::munmap(const_cast<std::uint8_t*>(data_), static_cast<std::size_t>(size_));
        note_unmap(static_cast<std::size_t>(size_));
    }
#endif
}

bool file_exists(const std::string& path_utf8) {
    if (path_utf8.empty()) {
        return false;
    }
#if defined(_WIN32)
    const std::wstring wide = widen(path_utf8);
    const DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st {};
    return ::stat(path_utf8.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    const char last = dir.back();
    if (last == '\\' || last == '/') {
        return dir + name;
    }
    const char sep = dir.find('\\') != std::string::npos ? '\\' : '/';
    return dir + sep + name;
}

std::string dir_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return std::string();
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

std::string file_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace cml::pdb::detail
