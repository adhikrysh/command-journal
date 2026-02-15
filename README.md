# command-journal

Execute a simulated payload command, crash between persistence steps, and recover it in a fresh process. The payload is a register with a value from 0 to 1023. Commands carry a sequence number and an expected prior state.

The difficult case is a lost acknowledgement: the payload has applied a command, but the host still thinks it is pending. Recovery asks the payload about that exact command before deciding whether to apply it again.

## Build and try a crash

Requires C++20, CMake 3.20+, and a POSIX system such as Linux or macOS. No runtime dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure

journal_demo_dir=$(mktemp -d)
./build/command-journal set "$journal_demo_dir" 0 42
./build/command-journal set "$journal_demo_dir" 42 84 --crash payload_sync || test "$?" -eq 75
./build/command-journal status "$journal_demo_dir"
./build/command-journal recover "$journal_demo_dir"
```

The deliberate crash exits with code 75. Before recovery, the host has completed command 1 while the payload has applied command 2. Recovery records command 2 as complete. The final value is 84 and the payload application count is two, not three.

`status` is read-only. `recover` and `set` may truncate an incomplete final record before continuing. A complete record with a bad checksum is rejected; it is not silently discarded. All commands return JSON on stdout and errors on stderr.

## What is persisted

The host appends an accepted command and calls `fsync` before dispatch. The payload validates the expected state, persists the command under its sequence number, and syncs before acknowledging it. The host then appends and syncs a completion record.

On restart, both logs are checked before either is repaired. If a command is accepted but incomplete, recovery either applies it or recognises the identical command already stored by the payload. Reusing a sequence number with different content is an error.

Files are exclusively locked for writes. Records have a version, bounded length, and CRC32C checksum. Interrupted reads and writes are retried. After an I/O-boundary failure, the executor refuses more work until it is closed and reopened, because its in-memory state may no longer match disk.

## What the tests establish

The test executable starts fresh processes and terminates them at nine write/sync boundaries. It then runs recovery twice and checks that the accepted state is recovered without duplicate payload applications. Other checks cover every prefix of a two-record stream, 464 single-bit corruptions, malformed transitions, stale preconditions, concurrent writers, and symlink rejection.

This guarantee depends on the simulated payload remembering command IDs durably. A physical actuator that cannot identify a previously applied command has an ambiguity the host journal cannot solve. I would not claim exactly-once control for arbitrary hardware.

The tests exercise process death, not power removal or failing storage hardware. Persistence depends on the filesystem's `fsync` and locking behaviour. New state-directory creation and hardware write caches need platform-specific treatment before making a power-loss guarantee. Journals are limited to 64 MiB each and replayed in memory; compaction is not implemented.

The API is [executor.hpp](include/journal/executor.hpp), and [the record format](docs/format.md) is documented separately. Use `--help` for all crash stages. Sanitizers are enabled with `-DJOURNAL_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`. JPL's [F Prime command sequencer](https://fprime.jpl.nasa.gov/v4.3.0/Svc/CmdSequencer/docs/sdd/) is useful context for command validation and sequencing; this project is a separate persistence experiment.
