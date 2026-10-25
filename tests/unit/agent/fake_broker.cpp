// A "broker" for the relaunch back-off test: it listens on the pipe it is given with --pipe,
// records its start in the file named by CML_FAKE_BROKER_LOG, accepts one connection and exits at
// once, so the agent keeps finding no broker and keeps wanting to start a new one.
#include <windows.h>

#include <cstring>
#include <string>

#include "cml/ipc/pipe.h"

int main(int argc, char** argv) {
    std::string pipe;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--pipe") == 0) {
            pipe = argv[i + 1];
        }
    }
    if (pipe.empty()) {
        return 2;
    }
    cml::Result<cml::ipc::PipeServer> server = cml::ipc::PipeServer::create(pipe);
    if (!server) {
        return 3;
    }
    wchar_t log_path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"CML_FAKE_BROKER_LOG", log_path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "launch\n", 7, &written, nullptr);
            CloseHandle(file);
        }
    }
    cml::Result<cml::ipc::PipeServer::Accepted> accepted = server->accept(10000);
    (void)accepted;
    return 0;  // the connection is dropped with the process
}
