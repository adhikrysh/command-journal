#include "journal/codec.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace journal {
namespace {
void put32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void put64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint32_t get32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < 4; ++i)
        result = (result << 8) | bytes[offset + i];
    return result;
}
std::uint64_t get64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i)
        result = (result << 8) | bytes[offset + i];
    return result;
}
void validate(Command command) {
    if (command.sequence == 0 || command.expected_sequence >= command.sequence ||
        command.expected_value > 1023 || command.value > 1023) {
        throw std::invalid_argument("invalid command sequence or payload range");
    }
}
} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
    }
    return ~crc;
}

std::vector<std::uint8_t> encode(Event event) {
    const bool accepted = event.kind == Kind::accepted;
    if (!accepted && event.kind != Kind::completed)
        throw std::invalid_argument("unknown event kind");
    if (event.command.sequence == 0)
        throw std::invalid_argument("zero command sequence");
    if (accepted)
        validate(event.command);
    std::vector<std::uint8_t> bytes{'C', 'J', 'R', '1'};
    put32(bytes, accepted ? 25U : 9U);
    bytes.push_back(static_cast<std::uint8_t>(event.kind));
    put64(bytes, event.command.sequence);
    if (accepted) {
        put64(bytes, event.command.expected_sequence);
        put32(bytes, event.command.expected_value);
        put32(bytes, event.command.value);
    }
    put32(bytes, crc32c(bytes));
    return bytes;
}

Scan scan(std::span<const std::uint8_t> bytes) {
    Scan result;
    while (result.valid_bytes < bytes.size()) {
        const auto remaining = bytes.subspan(result.valid_bytes);
        if (remaining.size() < 8)
            break;
        if (remaining[0] != 'C' || remaining[1] != 'J' || remaining[2] != 'R' ||
            remaining[3] != '1') {
            throw std::runtime_error("journal magic or version mismatch at byte " +
                                     std::to_string(result.valid_bytes));
        }
        const std::size_t length = get32(remaining, 4);
        if (length != 9 && length != 25)
            throw std::runtime_error("invalid journal frame length");
        if (remaining.size() >= 9) {
            const auto kind = static_cast<Kind>(remaining[8]);
            if (!((kind == Kind::accepted && length == 25) ||
                  (kind == Kind::completed && length == 9))) {
                throw std::runtime_error("journal kind/length mismatch");
            }
        }
        if (remaining.size() < length + 12)
            break;
        if (crc32c(remaining.first(length + 8)) != get32(remaining, length + 8)) {
            throw std::runtime_error("journal checksum mismatch at byte " +
                                     std::to_string(result.valid_bytes));
        }
        const auto kind = static_cast<Kind>(remaining[8]);
        if (!((kind == Kind::accepted && length == 25) ||
              (kind == Kind::completed && length == 9))) {
            throw std::runtime_error("journal kind/length mismatch");
        }
        Command command;
        command.sequence = get64(remaining, 9);
        if (command.sequence == 0)
            throw std::runtime_error("zero journal sequence");
        if (kind == Kind::accepted) {
            command.expected_sequence = get64(remaining, 17);
            command.expected_value = get32(remaining, 25);
            command.value = get32(remaining, 29);
            validate(command);
        }
        result.events.push_back({kind, command});
        result.valid_bytes += length + 12;
    }
    result.incomplete_tail = bytes.size() - result.valid_bytes;
    return result;
}
} // namespace journal
