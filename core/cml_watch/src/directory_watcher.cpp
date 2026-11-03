#include <string>
#include <vector>

#include "cml/util/log.h"
#include "cml/watch/watch.h"
#include "cml/watch/watch_diag_ids.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <thread>
#endif

namespace cml::watch {

#if !defined(_WIN32)

Result<std::unique_ptr<DirectoryWatcher>> DirectoryWatcher::start(const std::vector<WatchRoot>&, Sink, std::vector<Diag>*) {
    return Diag::make(diag::kUnsupported, "directory watching (ReadDirectoryChangesW) is only available on Windows",
                      "Use the `reload` command to pick up edits on this host.");
}

#else

namespace {

constexpr DWORD kBufferBytes = 64 * 1024;
constexpr std::size_t kMaxRoots = MAXIMUM_WAIT_OBJECTS - 1;  // one wait slot is the stop event

std::string utf8_from_wide(const wchar_t* text, std::size_t length) {
    if (length == 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    return out;
}

struct Root {
    std::string directory;
    bool recursive = false;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<std::uint64_t> buffer;  // 8-byte aligned, as FILE_NOTIFY_INFORMATION requires (DWORD)
    bool armed = false;                 // a ReadDirectoryChangesW is outstanding
};

class WindowsWatcher final : public DirectoryWatcher {
public:
    WindowsWatcher(DirectoryWatcher::Sink sink) : sink_(std::move(sink)) {}

    ~WindowsWatcher() override {
        if (stop_event_ != nullptr) {
            SetEvent(stop_event_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        for (Root& r : roots_) {
            close_root(r);
        }
        if (stop_event_ != nullptr) {
            CloseHandle(stop_event_);
        }
    }

    // Opens the root and issues the first read. False (and *why set) when it cannot be watched.
    bool add_root(const WatchRoot& root, std::string* why) {
        Root r;
        r.directory = root.directory;
        r.recursive = root.recursive;
        const std::wstring wide = wide_from_utf8(root.directory);
        r.handle = CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (r.handle == INVALID_HANDLE_VALUE) {
            *why = "error " + std::to_string(GetLastError());
            return false;
        }
        r.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        r.buffer.resize(kBufferBytes / sizeof(std::uint64_t));
        roots_.push_back(std::move(r));
        return true;
    }

    bool start_thread(std::string* why) {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event_ == nullptr) {
            *why = "cannot create the stop event";
            return false;
        }
        for (Root& r : roots_) {
            if (!arm(r)) {
                *why = "ReadDirectoryChangesW failed for '" + r.directory + "' (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }

private:
    bool arm(Root& r) {
        ResetEvent(r.event);
        r.overlapped = OVERLAPPED{};
        r.overlapped.hEvent = r.event;
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
        if (!ReadDirectoryChangesW(r.handle, r.buffer.data(), kBufferBytes, r.recursive ? TRUE : FALSE, filter, nullptr, &r.overlapped, nullptr)) {
            r.armed = false;
            return false;
        }
        r.armed = true;
        return true;
    }

    void close_root(Root& r) {
        if (r.handle != INVALID_HANDLE_VALUE) {
            if (r.armed) {
                CancelIoEx(r.handle, &r.overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(r.handle, &r.overlapped, &ignored, TRUE);  // the kernel is done with the buffer
                r.armed = false;
            }
            CloseHandle(r.handle);
            r.handle = INVALID_HANDLE_VALUE;
        }
        if (r.event != nullptr) {
            CloseHandle(r.event);
            r.event = nullptr;
        }
    }

    void deliver(const Root& r, DWORD bytes) {
        const auto* base = reinterpret_cast<const unsigned char*>(r.buffer.data());
        DWORD offset = 0;
        for (;;) {
            if (offset + sizeof(FILE_NOTIFY_INFORMATION) > bytes) {
                break;
            }
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            FsEvent event;
            switch (info->Action) {
                case FILE_ACTION_ADDED: event.action = FsAction::Added; break;
                case FILE_ACTION_REMOVED: event.action = FsAction::Removed; break;
                case FILE_ACTION_MODIFIED: event.action = FsAction::Modified; break;
                case FILE_ACTION_RENAMED_OLD_NAME: event.action = FsAction::RenamedFrom; break;
                case FILE_ACTION_RENAMED_NEW_NAME: event.action = FsAction::RenamedTo; break;
                default: event.action = FsAction::Modified; break;
            }
            event.path = r.directory + "\\" + utf8_from_wide(info->FileName, info->FileNameLength / sizeof(wchar_t));
            sink_(event);
            if (info->NextEntryOffset == 0) {
                break;
            }
            offset += info->NextEntryOffset;
        }
    }

    void run() {
        std::vector<HANDLE> handles;
        std::vector<std::size_t> index;  // wait slot -> root
        for (;;) {
            handles.assign(1, stop_event_);
            index.assign(1, 0);
            for (std::size_t i = 0; i < roots_.size(); ++i) {
                if (roots_[i].armed) {
                    handles.push_back(roots_[i].event);
                    index.push_back(i);
                }
            }
            if (handles.size() == 1) {
                // Every directory is gone: nothing left to wait for but the stop request.
                WaitForSingleObject(stop_event_, INFINITE);
                return;
            }
            const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                return;
            }
            if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + handles.size()) {
                CML_LOG(Error, "directory watcher: wait failed (error %lu)", static_cast<unsigned long>(GetLastError()));
                return;
            }
            Root& r = roots_[index[w - WAIT_OBJECT_0]];
            DWORD bytes = 0;
            if (!GetOverlappedResult(r.handle, &r.overlapped, &bytes, FALSE)) {
                const DWORD error = GetLastError();
                r.armed = false;
                if (error != ERROR_OPERATION_ABORTED) {
                    // The directory was deleted or the volume went away: events cannot come any more. Whatever
                    // happened to the files in it, a re-check is the safe answer.
                    CML_LOG(Warn, "directory watcher: '%s' is no longer watched (error %lu)", r.directory.c_str(), static_cast<unsigned long>(error));
                    FsEvent lost;
                    lost.action = FsAction::Overflow;
                    sink_(lost);
                }
                continue;
            }
            if (bytes == 0) {
                // The buffer overflowed: events were lost.
                FsEvent lost;
                lost.action = FsAction::Overflow;
                sink_(lost);
            } else {
                deliver(r, bytes);
            }
            if (!arm(r)) {
                CML_LOG(Warn, "directory watcher: cannot re-arm '%s' (error %lu)", r.directory.c_str(), static_cast<unsigned long>(GetLastError()));
            }
        }
    }

    DirectoryWatcher::Sink sink_;
    HANDLE stop_event_ = nullptr;
    std::vector<Root> roots_;
    std::thread thread_;
};

}  // namespace

Result<std::unique_ptr<DirectoryWatcher>> DirectoryWatcher::start(const std::vector<WatchRoot>& roots, Sink sink, std::vector<Diag>* skipped) {
    auto watcher = std::make_unique<WindowsWatcher>(std::move(sink));
    std::size_t opened = 0;
    for (const WatchRoot& root : roots) {
        std::string why;
        if (opened >= kMaxRoots) {
            why = "more than " + std::to_string(kMaxRoots) + " directories";
        } else if (watcher->add_root(root, &why)) {
            ++opened;
            continue;
        }
        if (skipped != nullptr) {
            skipped->push_back(Diag::make(diag::kRootNotWatched, "the directory '" + root.directory + "' cannot be watched (" + why + ")",
                                          "Saves in that directory are picked up by the next `reload` instead."));
        }
    }
    if (opened == 0) {
        return Diag::make(diag::kWatchFailed, "none of the " + std::to_string(roots.size()) + " source directories could be watched",
                          "Check that the source directories exist; use the `reload` command meanwhile.");
    }
    std::string why;
    if (!watcher->start_thread(&why)) {
        return Diag::make(diag::kWatchFailed, "the directory watcher could not start: " + why, "Use the `reload` command meanwhile.");
    }
    return std::unique_ptr<DirectoryWatcher>(std::move(watcher));
}

#endif

}  // namespace cml::watch
