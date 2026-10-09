# command-journal

if a ground system crashes after sending a command but before recording that it was sent, it restarts without knowing whether the payload acted. resending risks executing the command twice; not resending risks never executing it. command-journal is an experiment in recovering from that crash window deterministically.

the payload is a simulated register (0 to 1023) and every command is a compare-and-set, applied only if the payload is in the expected state.

## protocol

the host appends an **accepted** record to its journal and `fsync`s before dispatching. the payload validates the precondition, appends the command to its own log, syncs, and only then acknowledges. the host then appends and syncs **completed**.

```text
host: accept + sync -> dispatch -> payload: apply + sync -> ack -> host: complete + sync
```

this ordering bounds recovery: at most one command is in doubt, and the payload is at most one durable command ahead of the host. in the lost-acknowledgement case (the payload applied 42 -> 84 and the process died before the host recorded completion), recovery replays both logs, finds the identical command in the payload log, writes the missing completion and doesn't resend. if the payload never received the command, recovery re-checks the precondition before applying it, and a reused sequence number with different contents is rejected, which keeps stale commands and divergent histories out.

## journal format

frames carry a magic number, version, bounded length and crc32c, and replay validates state transitions as well as checksums. an incomplete trailing frame is truncated; a complete frame that fails validation is rejected, and the scanner never skips ahead to the next valid-looking frame, since that could silently drop a real command. both logs are validated before either is repaired, because repairing one first could destroy the evidence of what the payload did. any i/o error poisons the executor until it is reopened.

## verification

the central test spawns real processes, kills them at each of the nine write and sync boundaries, runs recovery twice, and checks that the payload's apply count doesn't change on the second run. other tests cover 464 single-bit corruptions, every prefix of a frame stream, symlinked logs and the single-writer lock.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure
d=$(mktemp -d); ./build/command-journal set "$d" 0 42
./build/command-journal set "$d" 42 84 --crash payload_sync; ./build/command-journal recover "$d"
```

exactly-once recovery depends on the simulated payload durably recording command ids and contents. an actuator that can act without recording it, or storage that misreports durability, needs a different design. the tests cover process crashes, not power loss or storage failure, and without compaction the 64 MiB journal cap limits this to an experiment.
