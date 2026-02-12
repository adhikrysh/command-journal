#include "journal/executor.hpp"

#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
std::uint32_t value(const char *text) {
    const std::string input(text);
    std::uint32_t result{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (error != std::errc{} || end != input.data() + input.size())
        throw std::invalid_argument("invalid register value");
    return result;
}
void print(journal::Status status) {
    std::cout << std::boolalpha << "{\"completed_sequence\":" << status.completed_sequence
              << ",\"completed_value\":" << status.completed_value
              << ",\"device_sequence\":" << status.device_sequence
              << ",\"device_value\":" << status.device_value << ",\"pending\":" << status.pending
              << ",\"device_applications\":" << status.device_applications
              << ",\"incomplete_tail_bytes\":" << status.incomplete_tail_bytes
              << ",\"repaired_tail_bytes\":" << status.repaired_tail_bytes << "}\n";
}
constexpr const char *usage =
    "Usage:\n"
    "  command-journal set DIRECTORY EXPECTED_VALUE NEW_VALUE [--crash STAGE]\n"
    "  command-journal recover DIRECTORY\n"
    "  command-journal status DIRECTORY\n"
    "Values: 0..1023. Initial value: 0. Status is read-only.\n"
    "Crash stages: accept_half, accept_write, accept_sync, payload_half, payload_write,\n"
    "payload_sync, commit_half, commit_write, commit_sync. A deliberate crash exits 75.\n"
    "Writable open repairs incomplete final records; complete corruption is rejected.\n";
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << usage;
            return 0;
        }
        if (argc < 3) {
            std::cerr << usage;
            return 2;
        }
        const std::string action(argv[1]);
        if (action == "status" && argc == 3) {
            journal::Executor executor(argv[2], false);
            print(executor.status());
        } else if (action == "recover" && argc == 3) {
            journal::Executor executor(argv[2]);
            executor.recover();
            print(executor.status());
        } else if (action == "set" && (argc == 5 || argc == 7)) {
            const auto expected = value(argv[3]), next = value(argv[4]);
            std::string crash;
            if (argc == 7) {
                const std::set<std::string> allowed = {
                    "accept_half",  "accept_write", "accept_sync",  "payload_half", "payload_write",
                    "payload_sync", "commit_half",  "commit_write", "commit_sync"};
                if (std::string(argv[5]) != "--crash" || !allowed.contains(argv[6]))
                    throw std::invalid_argument("invalid crash stage");
                crash = argv[6];
            }
            journal::Executor executor(argv[2], true, [&](std::string_view stage) {
                if (stage == crash)
                    ::_exit(75);
            });
            executor.set(expected, next);
            print(executor.status());
        } else {
            std::cerr << usage;
            return 2;
        }
        if (!std::cout)
            throw std::runtime_error("failed writing output");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "command-journal: " << e.what() << '\n';
        return 1;
    }
}
