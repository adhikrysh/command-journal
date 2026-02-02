#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace journal {
struct Status {
    std::uint64_t completed_sequence{};
    std::uint32_t completed_value{};
    std::uint64_t device_sequence{};
    std::uint32_t device_value{};
    bool pending{};
    std::size_t device_applications{};
    std::size_t incomplete_tail_bytes{};
    std::size_t repaired_tail_bytes{};
};
using FaultHook = std::function<void(std::string_view)>;

// A single-writer, POSIX command executor and a durable simulated payload.
// Writable open creates the directory/files and repairs incomplete final frames.
// Read-only open never repairs or creates files. Calls must be serialized.
class Executor {
  public:
    explicit Executor(const std::filesystem::path &directory, bool writable = true,
                      FaultHook fault = {});
    ~Executor();
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    // Complete an accepted command after restart. Returns whether work was pending.
    bool recover();
    // Compare-and-set a simulated payload register in [0,1023]. Recover any
    // earlier accepted command first, then require the expected current value.
    void set(std::uint32_t expected_value, std::uint32_t new_value);
    [[nodiscard]] Status status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace journal
