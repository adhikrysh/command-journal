#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace journal {
struct Command {
    std::uint64_t sequence{};
    std::uint64_t expected_sequence{};
    std::uint32_t expected_value{};
    std::uint32_t value{};
    bool operator==(const Command &) const = default;
};
enum class Kind : std::uint8_t { accepted = 1, completed = 2 };
struct Event {
    Kind kind;
    Command command; // completed events use only command.sequence
};
struct Scan {
    std::vector<Event> events;
    std::size_t valid_bytes{};
    std::size_t incomplete_tail{};
};

// Frame: "CJR1", big-endian u32 payload length, payload, big-endian CRC32C.
// CRC covers header and payload. Payload lengths are fixed by event kind.
std::uint32_t crc32c(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode(Event event);
// Only an incomplete final frame is recoverable. Complete bad checksums,
// unknown versions, invalid lengths, and invalid payloads fail closed.
Scan scan(std::span<const std::uint8_t> bytes);
} // namespace journal
