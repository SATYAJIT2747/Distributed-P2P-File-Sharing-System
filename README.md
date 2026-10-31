# Peer-to-Peer Distributed File Sharing System (DFSS) - Mini BitTorrent

A robust, multi-threaded **Hybrid Peer-to-Peer (P2P) File Sharing System** written in C++17. The system uses central Trackers for managing peer metadata, user authentication, group management, and file indexing, while piece-by-piece file transfers are executed directly between peers over TCP sockets.

**Highlights**

- Two synchronised Trackers with acknowledged sync, log-based recovery in both directions, and **mid-session client failover with automatic re-login**.
- Downloaders become real seeders, and a seeder only serves files that were deliberately shared (no path traversal). See [Section 10.1](#101-v2-reliability--security-hardening).
- Every client is both a **Seeder** and a **Leecher**.
- Parallel piece downloads through a fixed-size **ThreadPool** (2 x CPU cores).
- **SHA1 verification** of every 512 KB piece, with automatic retry from a different seeder.
- A 1 GB file downloads in about **3.4 seconds** in the logged runs (versus roughly 3 minutes with the original thread-per-piece design). See [Section 8](#8-performance-results).

---

## Table of Contents

1. [Directory & Code Structure](#1-directory--code-structure)
2. [Compilation and Setup](#2-compilation-and-setup)
3. [How to Run](#3-how-to-run)
4. [Architectural Overview](#4-architectural-overview-hybrid-p2p-design)
5. [Key Features & Supported Commands](#5-key-features--supported-commands)
6. [Quick Start Walkthrough](#6-quick-start-walkthrough)
7. [Implementation Details](#7-optimization-algorithms--implementation-details)
8. [Performance Results](#8-performance-results)
9. [Fault Tolerance](#9-fault-tolerance)
10. [Engineering Challenges and Fixes](#10-engineering-challenges-and-fixes)
11. [Limitations and Notes](#11-limitations-and-notes)
12. [Troubleshooting](#12-troubleshooting)
13. [License](#13-license)

---

## 1. Directory & Code Structure

The project is organized into structured subdirectories categorized by responsibility:

```
MINI_BIT_TORRENT/
├── README.md                 # System Documentation
├── tracker_info.txt          # Shared tracker configuration (IP:Port for T1 & T2)
├── client/                   # Client / Peer Application
│   ├── Makefile              # Build script for Client executable
│   ├── tracker_info.txt      # Client-side tracker address file
│   ├── main.cpp              # Client CLI entry point and main socket event loop
│   ├── downloader/           # Downloader and piece transfer management
│   │   ├── thread_pool.h     # Concurrent ThreadPool manager for piece downloads
│   │   ├── download_manager.h# Status tracker for active/completed downloads
│   │   ├── download_manager.cpp
│   │   ├── peer_downloader.h # Parallel piece downloading & verification logic
│   │   └── peer_downloader.cpp
│   ├── network/              # Tracker session shared by all client threads
│   │   ├── tracker_connection.h # Request/reply, failover to the other tracker, auto re-login
│   │   └── tracker_connection.cpp
│   ├── seeder/               # Peer serving (Seeder) module
│   │   ├── seeder.h          # Seeder listener, peer request handler & shared-files list
│   │   └── seeder.cpp
│   └── utils/                # Utility & cryptographic functions
│       ├── crypto_utils.h    # SHA1 calculation and hex string utilities
│       ├── crypto_utils.cpp
│       ├── client_utils.h    # File metadata, tracker info reader, socket check
│       └── client_utils.cpp
└── tracker/                  # Tracker Server Application
    ├── Makefile              # Build script for Tracker executable
    ├── tracker_info.txt      # Tracker configuration file
    ├── updates_t1.txt / updates_t2.txt # Created at runtime: updates waiting for the other tracker
    ├── main.cpp              # Tracker main server socket listener & entry point
    ├── state/                # Tracker global state & data structures
    │   ├── tracker_state.h   # Maps for users, groups, file metadata & mutexes
    │   └── tracker_state.cpp
    ├── sync/                 # Inter-tracker synchronization & failover
    │   ├── tracker_sync.h    # Heartbeat, acknowledged sync queue, and log recovery
    │   └── tracker_sync.cpp
    ├── handler/              # Client command processor
    │   ├── client_handler.h  # Request parsing & command handling logic
    │   └── client_handler.cpp
    └── utils/                # Tracker utilities
        ├── tracker_utils.h   # Socket checks and tracker config reader
        └── tracker_utils.cpp
```

### Module responsibilities

| Module | Purpose |
| :--- | :--- |
| `client/downloader/thread_pool.h` | Fixed-size worker pool with a task queue for piece downloads |
| `client/downloader/peer_downloader` | Parallel piece download, seeder selection, retries, SHA1 verification |
| `client/downloader/download_manager` | Tracks active `[P]` and completed `[C]` downloads |
| `client/network/tracker_connection` | One thread-safe tracker session for the CLI and download threads; switches tracker mid-session and logs in again |
| `client/seeder/` | Listener thread that serves pieces to other peers, only for files in its shared-files list |
| `client/utils/crypto_utils` | SHA1 calculation and hex-string helpers |
| `tracker/state/` | Shared maps (users, groups, file metadata, owners) and the mutex protecting them |
| `tracker/sync/` | Bidirectional heartbeat, background sync worker (ACK + retry), `updates_t<N>.txt` log and ordered replay |
| `tracker/handler/` | Parses and executes client commands; applies `SYNC` updates and replies `SYNC_ACK` |

---

## 2. Compilation and Setup

### 2.1. Prerequisites
- **Compiler**: `g++` (C++17 standard support)
- **Libraries**: OpenSSL (`-lcrypto`) for SHA1 calculations, Posix Threads (`-pthread`)
- **Platform**: Linux / POSIX sockets (the project uses `pwrite`, `recv`, `strtok_r`)

### 2.2. Building the Project

**Option 1: Using Makefile (Recommended)**
```bash
# Build Tracker executable
cd tracker
make

# Build Client executable
cd ../client
make
```

**Option 2: Direct Compilation Command**
```bash
# Compile Tracker
g++ -o tracker tracker/main.cpp tracker/state/*.cpp tracker/sync/*.cpp tracker/handler/*.cpp tracker/utils/*.cpp -std=c++17 -pthread -lcrypto -Itracker

# Compile Client
g++ -o client client/main.cpp client/downloader/*.cpp client/network/*.cpp client/seeder/*.cpp client/utils/*.cpp -std=c++17 -pthread -lcrypto -Iclient
```

---

## 3. How to Run

### 3.1. Tracker Setup Configuration (`tracker_info.txt`)
Create or edit `tracker_info.txt` containing IP and Port addresses for Tracker 1 (Primary) and Tracker 2 (Backup), one per line:
```
127.0.0.1:8989
127.0.0.1:8990
```

### 3.2. Execution Order

1. **Start Primary Tracker (T1):**
   ```bash
   ./tracker tracker_info.txt 1
   ```

2. **Start Secondary/Backup Tracker (T2):**
   ```bash
   ./tracker tracker_info.txt 2
   ```

3. **Start Client Instance(s):**
   ```bash
   ./client <Client_IP>:<Client_Port> tracker_info.txt

   # Example:
   ./client 127.0.0.1:5000 tracker_info.txt
   ```

Each client needs its own unique `IP:Port`, because that port is where its seeder thread listens for other peers.

---

## 4. Architectural Overview (Hybrid P2P Design)

```
                       +-----------------------+
                       |    Primary Tracker    |
                       |         (T1)          |
                       +-----------+-----------+
                                   | Heartbeat / Sync
                                   v
                       +-----------------------+
                       |    Backup Tracker     |
                       |         (T2)          |
                       +-----------------------+
                                ^     ^
            Metadata Queries /  |     | Metadata Queries /
            File Registration   |     | File Registration
                                v     v
             +--------------------+ +--------------------+
             | Peer 1 (Uploader)  |<| Peer 2 (Leecher)   |
             +--------------------+ +--------------------+
                        Direct Piece Transfer (TCP)
```

There are three kinds of communication, all over TCP:

1. **Client to Tracker**: commands, authentication, file metadata.
2. **Peer to Peer**: file pieces, transferred directly (the tracker never carries file data).
3. **Tracker to Tracker**: heartbeat and state synchronisation.

| Component | Architecture & Responsibilities | Concurrency Model |
| :--- | :--- | :--- |
| **Tracker (T1 & T2)** | Manages user credentials, group memberships, group file indexes, and active seeder IP:Port mappings. Maintains state consistency across primary and backup instances. | **Multithreaded**: Spawns a dedicated thread per client socket connection. |
| **Client (Peer)** | Functions simultaneously as a **Seeder** (listening server serving 512KB pieces to peers) and a **Leecher** (multi-threaded piece downloader using custom ThreadPool). | **Multithreaded**: Dedicated seeder thread + worker thread pool for parallel piece downloading. |
| **Communication** | Direct peer-to-peer piece downloads over TCP sockets with SHA1 checksum verification. | Async task execution with `pwrite` thread-safe file writing. |

### 4.1. Why TCP instead of UDP
File sharing needs reliable delivery. With UDP the application would have to implement lost-packet detection, retransmission and ordering itself; TCP provides all of this automatically.

### 4.2. Tracker Failover & High Availability
- **Heartbeat Monitoring**: Each tracker checks the other's liveness every 5 seconds.
- **Reliable Sync**: Every state change is queued and sent to the other tracker by a background sync thread, which waits for a `SYNC_ACK` and retries up to 3 times. Client requests never wait on this network work.
- **Failover**: If the client's tracker stops responding mid-session, the client switches to the other tracker, logs in again automatically, and resends the command.
- **Sync Back**: While the other tracker is down, updates are appended to `updates_t<N>.txt` (on T1 *or* T2). When it comes back, they are replayed oldest first, before any newer update.
- **Fast Restart**: Trackers set `SO_REUSEADDR`, so a crashed tracker can restart on the same port immediately.

### 4.3. Tracker State
The tracker keeps its state in in-memory hash maps, guarded by a global mutex:

```cpp
unordered_map<string, string> registered_users;
unordered_map<string, vector<string>> group_to_members;
unordered_map<string, pair<long long, string>> file_metadata;
unordered_map<string, vector<string>> file_owners;
```

---

## 5. Key Features & Supported Commands

| Command Category | Command Syntax | Description |
| :--- | :--- | :--- |
| **User Management** | `create_user <user_id> <password>` | Register a new user account |
| | `login <user_id> <password>` | Authenticate and register active seeder endpoint |
| | `logout` | Logout user and unregister active seedings |
| **Group Management**| `create_group <group_id>` | Create a new peer group (creator becomes owner) |
| | `join_group <group_id>` | Send request to join an existing group |
| | `list_requests <group_id>` | (Owner) View pending join requests |
| | `accept_request <group_id> <user_id>` | (Owner) Accept a user's join request |
| | `leave_group <group_id>` | Leave group (triggers owner re-assignment or group deletion) |
| | `list_groups` | View all available groups on the network |
| **File Sharing** | `upload_file <group_id> <file_path>` | Compute piece hashes and register file with tracker |
| | `list_files <group_id>` | List all files shared within a group |
| | `download_file <group_id> <file_name> <dest_path>` | Query seeders & download file pieces in parallel |
| | `show_downloads` | View status of active `[P]` and completed `[C]` downloads |
| | `stop_share <group_id> <file_name>` | Stop seeding a specific file |

---

## 6. Quick Start Walkthrough

A typical session with two clients (commands are typed at each client's prompt):

```
# Client A (127.0.0.1:5000)
create_user alice pass123
login alice pass123
create_group team1
upload_file team1 bigfile.iso      # run from the file's folder: the name is registered exactly as typed

# Client B (127.0.0.1:5001)
create_user bob pass456
login bob pass456
join_group team1

# Back on Client A
list_requests team1
accept_request team1 bob

# Client B
list_files team1
download_file team1 bigfile.iso /path/to/save/bigfile.iso   # destination is a file path
show_downloads          # [P] while running, [C] when complete
```

When Client B's download completes, it notifies the tracker (`promote_seeder`) over its logged-in session and becomes a seeder for that file too, serving it from the destination path it was saved to.

---

## 7. Optimization Algorithms & Implementation Details

1. **ThreadPool Downloader**: Avoids thread-creation overhead by reusing worker threads. Uses $2 \times \text{CPU cores}$ for parallel network piece fetching. Downloading is mostly waiting on the network, so while one worker blocks in `recv()`, another can use the CPU.
2. **512 KB Piece Partitioning**: Files are segmented into 512 KB pieces. Each piece hash is verified against SHA1 hashes retrieved from the tracker before saving to disk via `pwrite`.
3. **Seeder Promotion**: Upon successful completion of a file download, the client adds the file to its shared-files list and sends `promote_seeder` on its logged-in tracker session to register itself as an active seeder for other leechers. The tracker's reply is checked; on failure the file is removed from the shared list again.
4. **Shared-Files Allow-List**: The seeder maps each shared file name to its local path and only serves files that were uploaded or fully downloaded. A `REQUEST_PIECE` for anything else (for example `../../etc/passwd`) is refused.

### 7.1. Piece Selection (Sequential)
Pieces are enqueued in order from `0` to `N-1`. All tasks enter the queue immediately and the pool workers pick them up and download in parallel. There is no rarest-first strategy.

```cpp
for (int piece_index = 0; piece_index < total_pieces; piece_index++) {
    pool.enqueue([=, &local_bitfield, ...]() {
        // Download this piece
    });
}
```

### 7.2. Seeder Selection with Retries
If a piece fails, the next seeder in the list is tried, up to 3 retries, so a slow or unstable seeder does not block the whole download:

```cpp
int seeder_idx = (piece_index + retry_count) % seeders.size();
```

### 7.3. Integrity Checking
1. Calculate the SHA1 hash of the received piece.
2. Compare it with the expected hash from the tracker.
3. If equal: keep the piece and write it with `pwrite`.
4. If different: discard it and retry from a different seeder.

### 7.4. Metadata Size
A 1 GB file split into 512 KB pieces has about 2000 pieces. Each piece has a 40-character SHA1 hex hash, so roughly 80 KB of hash data is sent from client to tracker on upload.

---

## 8. Performance Results

All runs completed with a **100% success rate**. Throughput is in MB/s.

| Threads | File size (MB) | Time (s) | Throughput (MB/s) |
| ---: | ---: | ---: | ---: |
| 2 | 1 | 0.02 | 47.95 |
| 2 | 50 | 0.23 | 214.32 |
| 2 | 100 | 0.46 | 219.37 |
| 2 | 200 | 0.94 | 212.41 |
| 2 | 500 | 2.26 | 221.53 |
| 2 | 1024 | 5.02 | 204.17 |
| 4 | 1 | 0.02 | 44.26 |
| 4 | 50 | 0.21 | 237.69 |
| 4 | 100 | 0.42 | 238.87 |
| 4 | 200 | 0.87 | 230.39 |
| 4 | 500 | 1.59 | 314.25 |
| 4 | 1024 | 3.64 | 281.22 |
| 8 | 1 | 0.01 | 82.14 |
| 8 | 50 | 0.23 | 218.39 |
| 8 | 100 | 1.06 | 93.94 |
| 8 | 200 | 0.98 | 204.53 |
| 8 | 500 | 1.70 | 294.12 |
| 8 | 1024 | 3.44 | 297.41 |
| 16 | 1 | 0.01 | 78.08 |
| 16 | 10 | 1.09 | 9.18 |
| 16 | 50 | 1.04 | 48.21 |
| 16 | 100 | 1.07 | 93.84 |
| 16 | 200 | 1.11 | 179.88 |
| 16 | 500 | 1.65 | 302.17 |
| 16 | 1024 | 3.44 | 297.48 |

The 32-thread configuration was also charted (roughly 3.3 s and 314 MB/s at 1024 MB, read from the plots), but it is not in the log table above.

**1 GB summary**

| Threads | Time (s) | Throughput (MB/s) |
| ---: | ---: | ---: |
| 2 | 5.02 | 204.17 |
| 4 | 3.64 | 281.22 |
| 8 | 3.44 | 297.41 |
| 16 | 3.44 | 297.48 |

### 8.1. Before vs. after the ThreadPool

| Design | 1 GB download | Notes |
| :--- | :--- | :--- |
| One thread per piece (~2000 threads) | about 3 minutes (author's measurement, not in the log above) | Very high CPU use, mostly thread scheduling overhead |
| ThreadPool (2 x CPU cores) | 3.44 s at 8 and 16 threads | Workers reused; tasks pulled from a queue |

### 8.2. Observations
- **Start-up overhead dominates small files.** At 1 MB every configuration shows low throughput.
- **More threads help large files.** From 2 to 8 threads, the 1 GB time drops from 5.02 s to 3.44 s (about 31% less time, a 1.46x speed-up).
- **Diminishing returns beyond about 8 threads.** At 1 GB, 8 and 16 threads are effectively identical, which suggests a network or CPU limit.
- **8 threads is the most efficient setting** in these tests: near-maximum throughput without the resource cost of 16+.
- **More threads do not always help small and medium files.** At 10-200 MB, the 16-thread runs (about 1.0-1.1 s) were slower than the 2- and 4-thread runs (0.2-0.9 s), and 8 threads at 100 MB (1.06 s) was also slow. The cause was not investigated.

> The hardware, network and number of seeders used for these tests are not recorded, so treat the numbers as relative comparisons between thread counts rather than absolute benchmarks.

---

## 9. Fault Tolerance

### 9.1. Tracker Failover

| Event | Observed |
| :--- | :--- |
| Backup tracker detects main tracker is down | 5 seconds |
| Backup takes over | less than 3-4 seconds |
| Connected clients | Switch to the other tracker on their next command, log in again automatically, and resend the command |
| Either tracker down | The other one logs updates to `updates_t<N>.txt` after 3 unacknowledged attempts |
| State recovery | Logged updates are replayed oldest first, each confirmed with `SYNC_ACK`, before newer updates |

### 9.2. Download Failures
- If a piece download fails, the system retries with a different seeder (up to 3 retries).
- If the SHA1 hash does not match (corrupted data), the piece is discarded and re-downloaded automatically.

---

## 10. Engineering Challenges and Fixes

| Problem | Root cause | Fix |
| :--- | :--- | :--- |
| "Too many open files" on the tracker | Socket descriptors leaked on some code paths | `close(socket)` on every path, including error handling |
| Random segmentation faults with many clients | Multiple threads read/wrote the shared maps at once | `global_data_lock` mutex with `lock_guard` around all shared data access |
| Crashes while parsing commands | `strtok()` keeps hidden static state and is not thread-safe | Replaced with `strtok_r()` and a per-thread `saveptr` |
| `std::bad_alloc` / stack overflow with large metadata | 512 KB receive buffer allocated on each thread's stack | Heap allocation with `new` / `delete[]`, freed before the thread exits |
| Commands had to be typed twice ("type twice" issue) | A single `recv()` can return only part of a message (TCP is a byte stream) | Loop on `recv()` and accumulate data until the null terminator arrives, then parse |
| 1 GB download took about 3 minutes | ~2000 threads (one per piece) caused heavy scheduling overhead | Fixed-size ThreadPool (2 x CPU cores) with a task queue |

### Example: thread-safe command handling

```cpp
else if (command_parts[0] == "create_user") {
    lock_guard<mutex> lock(global_data_lock);   // released automatically
    if (registered_users.count(command_parts[1])) {
        response_message = "user already exists.";
    } else {
        registered_users[command_parts[1]] = command_parts[2];
        synchronize_with_peer("SYNC create_user " + ...);
    }
}
```

### Example: reading a complete message

```cpp
ssize_t total_received = 0;
while (total_received < msg_buff_size - 1) {
    bytes_received = recv(client_socket,
                          incoming_message + total_received,
                          msg_buff_size - total_received - 1, 0);
    if (bytes_received <= 0) break;
    total_received += bytes_received;
    if (incoming_message[total_received - 1] == '\0') break;  // terminator
}
```

### 10.1. v2: Reliability & Security Hardening

A review of the failover and seeding paths found five defects that the benchmarks did not exercise. All are fixed and were verified with a scripted end-to-end kill test (2 trackers, 3 clients) on Ubuntu.

| Problem | Root cause | Fix | Verified by |
| :--- | :--- | :--- | :--- |
| Seeder promotion never registered | `promote_seeder` was sent on a new socket to a hard-coded `127.0.0.1:8989`; that connection had no logged-in user, so the tracker replied "please login first", and the reply was never read | Sent through `tracker_request()` on the logged-in session (whichever tracker is active); the reply is checked | Leecher printed `now seeding f.bin`; the update reached the other tracker |
| Completed downloads could not be served | The file was saved to `dest_path`, but the seeder only opened `./<file_name>` | Completed files are registered in the shared-files map at their real path | Original seeder killed; a third client downloaded the whole file from the leecher alone (byte-identical) |
| Path traversal in the seeder | The seeder opened `./` + the name from `REQUEST_PIECE` without checking it was shared | Shared-files allow-list; any other name is refused | `REQUEST_PIECE /etc/passwd 0` refused, 0 bytes returned |
| Client failover only at startup | The client tried T2 only when it first connected; a later tracker crash ended the client | New `client/network` module: on a failed send/recv it connects to the other tracker, repeats the last login, and resends the command | T1 killed mid-session; both clients switched to T2, logged in again, and their next commands succeeded |
| Tracker sync could lose updates | Only T2 logged missed updates (T1 dropped them when T2 was down); sync was fire-and-forget with no ACK or retry | Background sync worker with `SYNC_ACK`, 3 retries, logs on both trackers, oldest-first replay | 3 updates replayed T2 to T1 and 1 update replayed T1 to T2 after restarts; logs emptied |

Related changes made while fixing these:

- `synchronize_with_peer()` only queues the update, so request handlers no longer do network I/O while holding `global_data_lock`.
- `SYNC create_group` / `SYNC join_group` skip entries that already exist, so a retried update cannot create duplicates.
- Trackers set `SO_REUSEADDR`, so a restarted tracker no longer fails with `Address already in use`.
- Client replies are read until the `\0` terminator, and the client exits cleanly when standard input closes.

---

## 11. Limitations and Notes

- **Sequential piece order** (no rarest-first selection as in the original BitTorrent).
- **Single global mutex** on the tracker: simple and safe, but it serialises all tracker state access.
- **Test environment undocumented**: hardware, network and seeder count are not recorded with the benchmarks.
- **Small/medium file performance** does not improve (and can worsen) with 16+ threads.
- **Tracker state is in memory.** A restarted tracker receives the updates logged while it was down, but not the state it held before it crashed; a full state snapshot on reconnect would close this gap.
- **The sync queue is in memory**: updates not yet delivered or logged are lost if that tracker process itself crashes.
- **Resent commands after failover** may be rejected as duplicates if the old tracker applied them but crashed before replying (for example `user already exists`).
- **Seeder address** sent at login is always `127.0.0.1:<port>`, so peers must run on the same machine.
- Passwords are stored and compared as plain strings in the tracker's user map, and traffic is not encrypted.

---

## 12. Troubleshooting

| Symptom | Likely cause / fix |
| :--- | :--- |
| `Address already in use` when starting a client | Another process is using that `IP:Port`; pick a different port |
| Client cannot reach a tracker | Check `tracker_info.txt` (correct `IP:Port` per line) and that T1 / T2 are running |
| Build error about SHA1 / OpenSSL | Install the OpenSSL development package and keep `-lcrypto` in the link command |
| Download stalls or a piece keeps failing | The seeder may be offline or have run `stop_share`; check `list_files` and `show_downloads` |
| Commands rejected after login issues | Run `login` again; `logout` unregisters your active seedings |
| `could not log in again on the new tracker` after a failover | The other tracker had not received your account yet; run `login` manually |
| Seeder prints `refusing request ... not a shared file` | The file was never uploaded / fully downloaded by that client, or it ran `stop_share` / `logout` |
| `download_file` fails with "file metadata not found" | Use the file name exactly as it was given to `upload_file` (see `list_files`) |

---

## 13. License

Released under the MIT License. See [LICENSE](LICENSE).
