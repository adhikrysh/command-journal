# command-journal

the most dangerous moment in commanding a spacecraft is very short. it starts when a command leaves the ground system and ends when someone writes down that it left.

crash inside that gap and you wake up not knowing what happened. did the payload act? if you send the command again, maybe it acts twice. if you do not, maybe it never acts at all. "fire the thruster for ten seconds" is not a command you want to run zero times or two times.

this is the oldest problem in distributed systems wearing a flight suit: two parties, two memories, and a crash at the worst possible instant. command-journal is a small experiment in surviving it, with the ordering of every write and every sync chosen on purpose.

## what it does

command-journal models a ground-side command executor that dies between sending a command and recording its result. the simulated payload is a register from 0 to 1023.

a command is a compare-and-set operation. it has a sequence number, the sequence and value it expects to find, and the value it wants to write. the payload checks those preconditions before applying a command.

## the durable boundary

the host first appends an **accepted** record to its own journal and calls `fsync`. only then may it dispatch the command to the payload. the payload checks the same precondition, appends the exact command to its own durable log, syncs it, and only then acknowledges it. after the acknowledgement, the host appends and syncs a **completed** record.

```text
host              payload
 | accept + sync     |
 v                   |
commands log         |
 | dispatch -------->|
 |                   v
 |              apply + sync
 |<------ durable ack|
 v
complete + sync
```

that ordering records the host's accepted command before the payload can change state. recovery has at most one pending command, and the payload may be at most one durable command ahead.

in the lost-acknowledgement case, command 2 changes the register from 42 to 84, the payload syncs it, and the process exits before the host records completion. on restart, both histories are replayed before either file changes. the payload already stores the same sequence and command body, so recovery writes the missing host completion without sending command 2 again.

if the payload has not stored the accepted command, recovery checks the expected sequence and value before applying it. a failed check stops recovery. reusing a sequence number with different command content is also an error. these rules reject old commands and split-brain histories.

## validating the journal

each frame has a magic and version, bounded length, typed payload, and crc32c checksum. accepted frames carry the full command; completion frames carry the sequence they close. replay checks state transitions as well as bytes: sequence order, expected state, one pending command, and matching completion.

the scanner distinguishes an incomplete final tail from corruption. after both journals validate, a writable open truncates only the incomplete tail and syncs the repaired files and directory. a read-only open only reports it. a complete frame with a bad checksum, length, kind, or transition is rejected. the scanner never searches for a later magic marker, because skipping damaged bytes could also skip a real command.

recovery verifies **both** logs before repair. truncating one log first could remove the evidence needed to decide whether the payload already acted. after an I/O failure, the executor becomes poisoned and rejects further calls. close it, reopen it, replay the durable histories, and recover from what reached disk.

## file access

the executor takes a non-blocking exclusive write lock and a shared inspection lock. it opens logs with `O_NOFOLLOW`, requires regular files, retries interrupted I/O, and caps each journal at 64 mib. these checks prevent competing writers, symlink redirects, shortened I/O, and unbounded replay memory.

the payload log records an effect; the host log records intent and completion. recovery does not infer either one from a transient acknowledgement or process memory.

## verification

the test executable has five checks. it verifies the crc32c known-answer value, every prefix of a two-frame stream, and 464 one-bit corruptions. it rejects malformed transitions and symlinked logs, checks stale preconditions and the single-writer lock, and proves that an executor poisoned by an I/O-boundary failure must be reopened.

the central recovery test starts fresh processes and deliberately exits at all nine write and sync boundaries: acceptance, payload application, and host completion. it runs recovery twice and confirms that the final history is stable and the payload application count does not increase on the second pass.

requires C++20, CMake 3.20+, and a POSIX system. you can reproduce the lost-acknowledgement case:

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

the crash exits 75. before recovery, the host is at sequence 1 while the payload is at sequence 2; after recovery, both are at sequence 2 with value 84 and two payload applications total.

this provides exactly-once *recovery* only because the simulated payload durably remembers command ids and command contents. it does not establish exactly-once control for a physical actuator that can lose that memory, act after power loss without recording the result, or have its own write cache report durability incorrectly. the tests exercise process death, not sudden power removal or failed storage hardware. compaction is absent, so the 64 mib journals bound this experiment and do not suit a long-running flight system.

the [executor api](include/journal/executor.hpp), [record format](docs/format.md), and [tests](tests/test_journal.cpp) describe the implementation at this project revision. JPL's [F Prime command sequencer](https://fprime.jpl.nasa.gov/v4.3.0/Svc/CmdSequencer/docs/sdd/) covers command validation and sequencing; this project implements a separate persistence experiment.

the takeaway fits on a sticky note: write down what you are about to do, make sure it reached disk, and never trust anything that only lived in memory when the lights went out.
