# Journal records and recovery

Every frame uses big-endian integers:

| Field | Bytes | Meaning |
| --- | ---: | --- |
| Magic | 4 | ASCII `CJR1` |
| Payload length | 4 | 25 for accepted commands, 9 for completions |
| Payload | 9 or 25 | Event kind and command data |
| CRC32C | 4 | Checksum over the header and payload |

An accepted payload contains kind 1, an eight-byte sequence, an eight-byte expected sequence, a four-byte expected value, and a four-byte new value. A completion contains kind 2 and its eight-byte sequence.

There can be at most one pending command. An acceptance must use the next sequence and match the last completed value. A completion must match the pending command. The payload log contains only accepted-command records, each representing a durable application of the simulated register change.

The scanner returns complete validated records and the length of an incomplete final tail. It never searches ahead for another magic marker after corruption: doing so could skip a command while producing an apparently healthy history. A complete frame with invalid magic, length, kind, checksum, or values fails the scan.

Replay checks command ordering and preconditions as well as byte integrity. Host and payload histories must agree, except that the payload may already contain the host's one pending command. Any other divergence requires investigation.

Writable open truncates only an incomplete final tail after both histories have passed these checks. It then syncs the repaired files. Read-only open reports the tail without changing it. The JSON status distinguishes bytes still incomplete from bytes repaired during this open.

Fault injection can stop after half a frame, after the complete write, or after sync, at each of the acceptance, payload, and completion stages. A process exit does not clear the operating system's cache; consequently the write-before-sync tests do not simulate a power failure. Their purpose is to check recovery logic and the absence of duplicate effects under process death.

CRC32C detects accidental corruption. It is not authentication. The state directory is assumed to be under the application's control; hostile modification of both files is outside this model.
