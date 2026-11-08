// Replay driver (WP-28): without CML_FUZZ every fuzz target is linked with this main instead of libFuzzer's, and runs over
// its checked-in seed corpus as an ordinary ctest (`fuzz.replay.<target>`, every CI cell). Each argument is a file or a
// directory (searched recursively, in sorted order so a failure is reproducible); every file is one input.
//
//   cml_fuzz_coff tests/fuzz/corpus/coff            replay a corpus
//   cml_fuzz_coff crash-1234                         replay one reproducer
//
// Exit code: 0 when every input returned, 1 when no input was found (a corpus that went missing must not pass), 2 for
// an unreadable argument or file. A crashing input crashes the process, which is what ctest reports.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "fuzz_common.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        const fs::path p(argv[i]);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code ec2;
                if (it->is_regular_file(ec2)) {
                    files.push_back(it->path());
                }
            }
        } else if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
        } else {
            std::fprintf(stderr, "replay: '%s' is neither a file nor a directory\n", argv[i]);
            return 2;
        }
    }
    std::sort(files.begin(), files.end());
    std::size_t bytes = 0;
    for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "replay: cannot read '%s'\n", f.string().c_str());
            return 2;
        }
        const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bytes += data.size();
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }
    std::printf("REPLAY inputs=%zu bytes=%zu\n", files.size(), bytes);
    if (files.empty()) {
        std::fprintf(stderr, "replay: no input files found\n");
        return 1;
    }
    return 0;
}
