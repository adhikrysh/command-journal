# command-journal

run commands on a simulated spacecraft payload and recover unfinished work after a process crash. the payload is a register, a stored value from 0 to 1023. each command includes a sequence number and the state it expects to find.

it handles a lost acknowledgement: the payload applied a command, but the host still marks it pending. recovery asks the payload about that exact command before deciding whether to apply it again.

## build and try a crash

requires C++20, CMake 3.20+, and a POSIX system such as Linux or macOS. it has no runtime dependencies.

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

the deliberate crash exits with code 75. before recovery, the host has completed command 1 and the payload has applied command 2. recovery records command 2 as complete. the final value is 84 and the payload application count is two, not three.

`status` is read-only. `recover` and `set` may truncate an incomplete final record before continuing. a complete record with a bad checksum is rejected, not silently discarded. every command writes JSON to stdout and errors to stderr.

## persisted state

the host appends an accepted command and calls `fsync` to flush the log to storage before dispatching it. the payload checks the expected state, stores the command under its sequence number, and syncs before acknowledging it. the host then appends and syncs a completion record.

on restart, the program checks both logs before repairing either one. for an accepted but incomplete command, recovery either applies it or recognises the identical command already stored by the payload. reusing a sequence number with different content is an error.

files have exclusive write locks. records carry a version, bounded length, and CRC32C checksum. interrupted reads and writes are retried. after a failed persistence step, the executor refuses more work until it is closed and reopened because its in-memory state may no longer match disk.

## tests and limits

the test executable starts fresh processes and stops them at nine write or sync boundaries. it then runs recovery twice and checks that the accepted state returns without duplicate payload applications. other checks cover every prefix of a two-record stream, 464 single-bit corruptions, malformed transitions, stale preconditions, concurrent writers, and symlink rejection.

this result depends on the simulated payload durably remembering command IDs. a physical actuator that cannot identify a command it already applied leaves an ambiguity that the host journal cannot resolve. this project does not claim exactly-once control for arbitrary hardware.

the tests cover process death, not power removal or failing storage hardware. persistence depends on the filesystem's `fsync` and locking behavior. new state-directory creation and hardware write caches need platform-specific work before this can make a power-loss guarantee. each journal is limited to 64 MiB and replayed in memory; compaction is not implemented.

the API is [executor.hpp](include/journal/executor.hpp), and [the record format](docs/format.md) describes the on-disk records. use `--help` for every crash stage. enable sanitizers with `-DJOURNAL_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`. JPL's [F Prime command sequencer](https://fprime.jpl.nasa.gov/v4.3.0/Svc/CmdSequencer/docs/sdd/) provides context for command validation and sequencing; this project is a separate persistence experiment.
