# command-journal

the scariest moment in commanding a spacecraft is right after the command goes out and right before you write down that it went. crash there and on restart nobody knows if the payload acted. resend and maybe the thruster fires twice. don't, and maybe it never fires. both are bad days.

this is a little crash-recovery experiment for that gap. the "thruster" is a simulated register from 0 to 1023, and commands are compare-and-set, so a command only applies if the payload is in the state it expects.

## ordering

the host writes **accepted** to its journal and `fsync`s before sending anything. the payload checks the precondition, writes the command to its own log, syncs, then acks. the host then writes **completed**.

```text
host: accept + sync -> dispatch -> payload: apply + sync -> ack -> host: complete + sync
```

so at recovery there's at most one command in doubt, and the payload is at most one step ahead. in the lost-ack case (payload applied 42 -> 84, then the process died), recovery replays both logs, sees the payload already has that exact command, writes the missing completion, and doesn't resend. if the payload never got it, recovery checks the preconditions before applying, and a reused sequence number with different contents is an error, which keeps stale commands and split-brain out.

## the journal

frames have a magic, a version, a bounded length and a crc32c, and replay checks the state transitions too, not just the bytes. a half-written tail gets truncated, but a complete frame with a bad checksum is rejected outright, and it never skips ahead looking for the next good frame, because skipping garbage can also skip a real command. both logs get verified before either is repaired, since fixing one first can destroy the evidence of what the payload did. any i/o error poisons the executor until you reopen it.

## tests

the main test spawns real processes and kills them at all nine write and sync points, runs recovery twice, and checks the payload's apply count doesn't move the second time. there's also 464 single-bit corruptions, every prefix of a stream, symlinked logs and a single-writer lock.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure
d=$(mktemp -d); ./build/command-journal set "$d" 0 42
./build/command-journal set "$d" 42 84 --crash payload_sync; ./build/command-journal recover "$d"
```

this only gets exactly-once recovery because the fake payload remembers what it did. a real actuator that forgets, or a disk cache that lies about syncing, is a different problem. the tests kill processes, they don't pull the power. no compaction either, so the 64 MiB journal cap means this isn't flying anywhere long.
