// SPDX-License-Identifier: Apache-2.0
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "qmpctl.hpp"

namespace {
extern "C" void on_signal(int) {
    qmpctl::request_stop();
}
} // namespace

int main(int argc, char **argv) {
    (void)std::signal(SIGINT, on_signal);
    (void)std::signal(SIGTERM, on_signal);
    // A reader that went away (qmpctl subscribe ... | head) ends the stream with a write
    // error instead of killing the process before it unsubscribes.
    (void)std::signal(SIGPIPE, SIG_IGN);
    std::vector<std::string> args(argv + 1, argv + argc);
    return qmpctl::run(args, std::cin, std::cout, std::cerr);
}
