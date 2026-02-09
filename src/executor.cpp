#include "journal/executor.hpp"
#include "journal/codec.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace journal {
namespace {
[[noreturn]] void io_error(const char *action) {
    throw std::runtime_error(std::string(action) + ": " + std::strerror(errno));
}
void sync_fd(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR)
            io_error("fsync");
    }
}
class File {
  public:
    File(const std::filesystem::path &path, bool writable) {
        const int flags =
            (writable ? O_RDWR | O_CREAT | O_APPEND : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW;
        fd_ = ::open(path.c_str(), flags, 0600);
        if (fd_ < 0)
            io_error("open journal");
        try {
            if (::flock(fd_, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
                io_error("lock journal");
            struct stat info{};
            if (::fstat(fd_, &info) != 0)
                io_error("stat journal");
            if (!S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 64 * 1024 * 1024) {
                throw std::runtime_error("journal must be a regular file no larger than 64 MiB");
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto count = ::pread(fd_, bytes.data() + offset, bytes.size() - offset,
                                           static_cast<off_t>(offset));
                if (count < 0) {
                    if (errno == EINTR)
                        continue;
                    io_error("read journal");
                }
                if (count == 0)
                    throw std::runtime_error("journal changed during locked read");
                offset += static_cast<std::size_t>(count);
            }
            decoded = scan(bytes);
        } catch (...) {
            ::close(fd_);
            fd_ = -1;
            throw;
        }
    }
    ~File() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    void repair() {
        if (decoded.incomplete_tail == 0)
            return;
        if (::ftruncate(fd_, static_cast<off_t>(decoded.valid_bytes)) != 0)
            io_error("truncate incomplete journal tail");
        sync_fd(fd_);
    }
    void append(Event event, const std::string &stage, const FaultHook &fault) {
        const auto bytes = encode(event);
        if (decoded.valid_bytes + bytes.size() > 64 * 1024 * 1024)
            throw std::runtime_error("journal size limit reached");
        const auto half = bytes.size() / 2;
        write_all(std::span(bytes).first(half));
        if (fault)
            fault(stage + "_half");
        write_all(std::span(bytes).subspan(half));
        if (fault)
            fault(stage + "_write");
        sync_fd(fd_);
        if (fault)
            fault(stage + "_sync");
        decoded.valid_bytes += bytes.size();
    }
    Scan decoded;

  private:
    void write_all(std::span<const std::uint8_t> bytes) {
        while (!bytes.empty()) {
            const auto count = ::write(fd_, bytes.data(), bytes.size());
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                io_error("append journal");
            }
            if (count == 0)
                throw std::runtime_error("journal write made no progress");
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
    }
    int fd_{-1};
};

struct Host {
    std::uint64_t sequence{};
    std::uint32_t value{};
    std::optional<Command> pending;
};
Host replay_host(const std::vector<Event> &events) {
    Host host;
    for (const auto &event : events) {
        const auto &command = event.command;
        if (event.kind == Kind::accepted) {
            if (host.pending || host.sequence == std::numeric_limits<std::uint64_t>::max() ||
                command.sequence != host.sequence + 1 ||
                command.expected_sequence != host.sequence || command.expected_value != host.value)
                throw std::runtime_error("invalid host command transition");
            host.pending = command;
        } else {
            if (!host.pending || command.sequence != host.pending->sequence)
                throw std::runtime_error("completion without matching accepted command");
            host.sequence = command.sequence;
            host.value = host.pending->value;
            host.pending.reset();
        }
    }
    return host;
}
struct Device {
    std::uint64_t sequence{};
    std::uint32_t value{};
    std::optional<Command> last;
    std::size_t applications{};
};
Device replay_device(const std::vector<Event> &events) {
    Device device;
    for (const auto &event : events) {
        const auto &command = event.command;
        if (event.kind != Kind::accepted ||
            device.sequence == std::numeric_limits<std::uint64_t>::max() ||
            command.sequence != device.sequence + 1 ||
            command.expected_sequence != device.sequence || command.expected_value != device.value)
            throw std::runtime_error("invalid payload command transition");
        device = {command.sequence, command.value, command, device.applications + 1};
    }
    return device;
}
std::filesystem::path prepare_directory(const std::filesystem::path &directory, bool writable) {
    if (writable)
        std::filesystem::create_directories(directory);
    if (!std::filesystem::is_directory(directory))
        throw std::runtime_error("state directory does not exist");
    return directory;
}
void sync_directory(const std::filesystem::path &directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        io_error("open state directory");
    try {
        sync_fd(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
}
} // namespace

struct Executor::Impl {
    std::filesystem::path directory;
    File commands;
    File payload;
    Host host;
    Device device;
    bool writable;
    bool poisoned{};
    FaultHook fault;
    std::size_t incomplete{};

    Impl(const std::filesystem::path &path, bool write, FaultHook hook)
        : directory(prepare_directory(path, write)), commands(directory / "commands.log", write),
          payload(directory / "payload.log", write), host(replay_host(commands.decoded.events)),
          device(replay_device(payload.decoded.events)), writable(write), fault(std::move(hook)) {
        const bool aligned = device.sequence == host.sequence && device.value == host.value;
        const bool applied_pending = host.pending && device.last && *host.pending == *device.last;
        if (!aligned && !applied_pending)
            throw std::runtime_error("host and payload histories are inconsistent");
        incomplete = commands.decoded.incomplete_tail + payload.decoded.incomplete_tail;
        // Validate both histories before modifying either file. Only physically
        // incomplete final frames are truncated; complete corruption is an error.
        if (writable) {
            commands.repair();
            payload.repair();
            sync_directory(directory);
        }
    }
    void ready() const {
        if (poisoned)
            throw std::runtime_error("executor had an I/O failure; close and reopen it to recover");
    }
    void require_write() const {
        ready();
        if (!writable)
            throw std::logic_error("executor was opened read-only");
    }
    bool recover() {
        require_write();
        if (!host.pending)
            return false;
        const auto command = *host.pending;
        try {
            if (device.sequence == command.sequence) {
                if (!device.last || *device.last != command)
                    throw std::runtime_error("command ID reused with different content");
                // Lost acknowledgement: the simulated payload can prove this
                // exact command is durable, so it must not apply it a second time.
            } else {
                if (device.sequence != command.expected_sequence ||
                    device.value != command.expected_value) {
                    throw std::runtime_error("payload precondition failed during recovery");
                }
                payload.append({Kind::accepted, command}, "payload", fault);
                device = {command.sequence, command.value, command, device.applications + 1};
            }
            commands.append({Kind::completed, {command.sequence, 0, 0, 0}}, "commit", fault);
            host = {command.sequence, command.value, std::nullopt};
            return true;
        } catch (...) {
            poisoned = true;
            throw;
        }
    }
};

Executor::Executor(const std::filesystem::path &directory, bool writable, FaultHook fault)
    : impl_(std::make_unique<Impl>(directory, writable, std::move(fault))) {}
Executor::~Executor() = default;
bool Executor::recover() {
    return impl_->recover();
}

void Executor::set(std::uint32_t expected, std::uint32_t value) {
    impl_->require_write();
    if (expected > 1023 || value > 1023)
        throw std::invalid_argument("payload register range is [0,1023]");
    impl_->recover();
    if (impl_->host.value != expected)
        throw std::invalid_argument("current payload value does not match precondition");
    if (impl_->host.sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("command sequence exhausted");
    const Command command{impl_->host.sequence + 1, impl_->host.sequence, expected, value};
    try {
        impl_->commands.append({Kind::accepted, command}, "accept", impl_->fault);
        impl_->host.pending = command;
        impl_->recover();
    } catch (...) {
        impl_->poisoned = true;
        throw;
    }
}

Status Executor::status() const {
    impl_->ready();
    return {impl_->host.sequence,
            impl_->host.value,
            impl_->device.sequence,
            impl_->device.value,
            impl_->host.pending.has_value(),
            impl_->device.applications,
            impl_->writable ? 0 : impl_->incomplete,
            impl_->writable ? impl_->incomplete : 0};
}
} // namespace journal
