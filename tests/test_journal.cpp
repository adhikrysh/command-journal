#include "journal/codec.hpp"
#include "journal/executor.hpp"

#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace journal;
void require(bool condition, const char *why) {
    if (!condition)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception &) {
        threw = true;
    }
    require(threw, "invalid operation accepted");
}
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "command-journal-test-XXXXXX").string();
        if (!::mkdtemp(pattern.data()))
            throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
std::vector<std::uint8_t> read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output)
        throw std::runtime_error("cannot write test file");
}
int run(const std::string &executable, std::vector<std::string> args) {
    const pid_t child = ::fork();
    if (child < 0)
        throw std::runtime_error("fork failed");
    if (child == 0) {
        const int sink = ::open("/dev/null", O_WRONLY);
        if (sink < 0 || ::dup2(sink, STDOUT_FILENO) < 0)
            ::_exit(126);
        ::close(sink);
        std::vector<char *> pointers;
        pointers.push_back(const_cast<char *>(executable.c_str()));
        for (auto &arg : args)
            pointers.push_back(arg.data());
        pointers.push_back(nullptr);
        ::execv(executable.c_str(), pointers.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::runtime_error("waitpid failed");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void codec() {
    const std::string reference = "123456789";
    const std::vector<std::uint8_t> digits(reference.begin(), reference.end());
    require(crc32c(digits) == 0xe3069283U, "CRC32C known-answer mismatch");
    const Command command{1, 0, 0, 42};
    auto bytes = encode({Kind::accepted, command});
    const auto completion = encode({Kind::completed, {1, 0, 0, 0}});
    bytes.insert(bytes.end(), completion.begin(), completion.end());
    const auto parsed = scan(bytes);
    require(parsed.events.size() == 2 && parsed.events[0].command == command, "codec round trip");
    for (std::size_t length = 0; length <= bytes.size(); ++length) {
        const auto prefix = std::span(bytes).first(length);
        const auto result = scan(prefix);
        require(result.valid_bytes + result.incomplete_tail == length, "prefix accounting");
        std::vector<std::uint8_t> restored;
        for (const auto &event : result.events) {
            const auto frame = encode(event);
            restored.insert(restored.end(), frame.begin(), frame.end());
        }
        require(restored.size() == result.valid_bytes, "prefix decoded partial event");
        require(std::equal(restored.begin(), restored.end(), prefix.begin()),
                "prefix data changed");
    }
    for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto corrupt = bytes;
            corrupt[byte] ^= static_cast<std::uint8_t>(1U << bit);
            rejects([&] { (void)scan(corrupt); });
        }
    }
}

void normal_execution() {
    Temporary directory;
    {
        Executor executor(directory.path);
        require(executor.status().device_value == 0, "initial device state");
        for (std::uint32_t i = 0; i < 40; ++i)
            executor.set(i, i + 1);
        require(executor.status().completed_sequence == 40, "wrong completed count");
        require(executor.status().device_applications == 40, "wrong device application count");
        rejects([&] { executor.set(0, 41); });
        rejects([&] { executor.set(40, 2048); });
        require(!executor.recover(), "completed work should not replay");
        rejects([&] { Executor second_writer(directory.path); });
    }
    const auto before = read(directory.path / "commands.log");
    {
        Executor reader(directory.path, false);
        require(reader.status().completed_value == 40, "read-only replay lost state");
        rejects([&] { reader.set(40, 41); });
    }
    require(before == read(directory.path / "commands.log"), "read-only open modified journal");
}

void crashes(const std::string &executable) {
    for (const std::string stage :
         {"accept_half", "accept_write", "accept_sync", "payload_half", "payload_write",
          "payload_sync", "commit_half", "commit_write", "commit_sync"}) {
        Temporary directory;
        require(run(executable, {"set", directory.path.string(), "0", "42"}) == 0,
                "baseline command failed");
        require(run(executable, {"set", directory.path.string(), "42", "84", "--crash", stage}) ==
                    75,
                "fault injection was not reached");
        const auto before = read(directory.path / "commands.log");
        {
            Executor reader(directory.path, false);
            const auto state = reader.status();
            require(state.completed_sequence >= 1, "previous durable command lost");
        }
        require(before == read(directory.path / "commands.log"),
                "inspection repaired tail without permission");
        require(run(executable, {"recover", directory.path.string()}) == 0,
                "fresh-process recovery failed");
        {
            Executor inspect(directory.path, false);
            const auto state = inspect.status();
            const std::uint64_t expected_sequence = stage == "accept_half" ? 1 : 2;
            require(state.completed_sequence == expected_sequence,
                    "recovery completed wrong command sequence");
            require(state.device_sequence == expected_sequence, "host and payload diverged");
            require(state.device_applications == expected_sequence,
                    "command applied more than once");
            require(state.device_value == (expected_sequence == 1 ? 42 : 84),
                    "wrong recovered payload value");
            require(!state.pending && state.incomplete_tail_bytes == 0,
                    "recovery left a partial transaction");
        }
        const auto settled = read(directory.path / "commands.log");
        require(run(executable, {"recover", directory.path.string()}) == 0,
                "second recovery failed");
        require(settled == read(directory.path / "commands.log"),
                "repeated recovery appended duplicate work");
    }
}

void corrupt_history() {
    Temporary directory;
    {
        Executor executor(directory.path);
        executor.set(0, 42);
    }
    const auto commands = read(directory.path / "commands.log");
    auto payload = read(directory.path / "payload.log");
    payload[20] ^= 1;
    write(directory.path / "payload.log", payload);
    rejects([&] { Executor executor(directory.path); });
    require(payload == read(directory.path / "payload.log") &&
                commands == read(directory.path / "commands.log"),
            "complete corruption was silently repaired");
    Temporary bad_transition;
    write(bad_transition.path / "commands.log", encode({Kind::completed, {1, 0, 0, 0}}));
    write(bad_transition.path / "payload.log", {});
    rejects([&] { Executor executor(bad_transition.path); });
    Temporary symlink;
    std::filesystem::create_symlink(directory.path / "commands.log", symlink.path / "commands.log");
    rejects([&] { Executor executor(symlink.path); });
}

void poisoned_executor() {
    Temporary directory;
    {
        Executor executor(directory.path, true, [](std::string_view stage) {
            if (stage == "accept_sync")
                throw std::runtime_error("injected I/O-boundary failure");
        });
        rejects([&] { executor.set(0, 17); });
        rejects([&] { executor.set(0, 18); });
        rejects([&] { (void)executor.status(); });
    }
    Executor reopened(directory.path);
    require(reopened.recover(), "accepted command disappeared after exception");
    require(reopened.status().device_value == 17 && reopened.status().device_applications == 1,
            "wrong result after reopening");
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    unsigned failed = 0;
    auto test = [&](const char *name, const std::function<void()> &body) {
        try {
            body();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("known CRC, every prefix, and 464 single-bit corruptions", codec);
    test("normal execution, preconditions, and writer lock", normal_execution);
    test("nine fresh-process crash/recovery boundaries", [&] { crashes(argv[1]); });
    test("complete corruption, invalid transitions, and symlinks", corrupt_history);
    test("failed executor must be reopened", poisoned_executor);
    return failed == 0 ? 0 : 1;
}
