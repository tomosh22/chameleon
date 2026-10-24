#include <cstdio>
#include <cstring>

#include "cml/core/version.h"

namespace {

void print_usage(std::FILE* out) {
    std::fputs("usage: cml_broker [--version | --help]\n", out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("cml_broker %s\n", cml::core::version_string());
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        print_usage(stdout);
        return 0;
    }
    print_usage(stderr);
    return 2;
}
