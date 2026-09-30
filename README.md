# Peer-to-Peer Distributed File Sharing System (DFSS) - Mini BitTorrent

A robust, multi-threaded **Hybrid Peer-to-Peer (P2P) File Sharing System** written in C++17. The system uses central Trackers for managing peer metadata, user authentication, group management, and file indexing, while piece-by-piece file transfers are executed directly between peers over TCP sockets.

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
│   ├── seeder/               # Peer serving (Seeder) module
│   │   ├── seeder.h          # Seeder listener thread & peer request handler
│   │   └── seeder.cpp
│   └── utils/                # Utility & cryptographic functions
│       ├── crypto_utils.h    # SHA1 calculation and hex string utilities
│       ├── crypto_utils.cpp
│       ├── client_utils.h    # File metadata, tracker info reader, socket check
│       └── client_utils.cpp
└── tracker/                  # Tracker Server Application
    ├── Makefile              # Build script for Tracker executable
    ├── tracker_info.txt      # Tracker configuration file
    ├── updates.txt           # Persistent log file for sync recovery during failover
    ├── main.cpp              # Tracker main server socket listener & entry point
    ├── state/                # Tracker global state & data structures
    │   ├── tracker_state.h   # Maps for users, groups, file metadata & mutexes
    │   └── tracker_state.cpp
    ├── sync/                 # Inter-tracker synchronization & failover
    │   ├── tracker_sync.h    # Heartbeat, sync messages, and log recovery
    │   └── tracker_sync.cpp
    ├── handler/              # Client command processor
    │   ├── client_handler.h  # Request parsing & command handling logic
    │   └── client_handler.cpp
    └── utils/                # Tracker utilities
        ├── tracker_utils.h   # Socket checks and tracker config reader
        └── tracker_utils.cpp
```

---

## 2. Compilation and Setup

### 2.1. Prerequisites
- **Compiler**: `g++` (C++17 standard support)
- **Libraries**: OpenSSL (`-lcrypto`) for SHA1 calculations, Posix Threads (`-pthread`)

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
g++ -o client client/main.cpp client/downloader/*.cpp client/seeder/*.cpp client/utils/*.cpp -std=c++17 -pthread -lcrypto -Iclient
```

---

## 3. How to Run

### 3.1. Tracker Setup Configuration (`tracker_info.txt`)
Create or edit `tracker_info.txt` containing IP and Port addresses for Tracker 1 (Primary) and Tracker 2 (Backup):
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

| Component | Architecture & Responsibilities | Concurrency Model |
| :--- | :--- | :--- |
| **Tracker (T1 & T2)** | Manages user credentials, group memberships, group file indexes, and active seeder IP:Port mappings. Maintains state consistency across primary and backup instances. | **Multithreaded**: Spawns a dedicated thread per client socket connection. |
| **Client (Peer)** | Functions simultaneously as a **Seeder** (listening server serving 512KB pieces to peers) and a **Leecher** (multi-threaded piece downloader using custom ThreadPool). | **Multithreaded**: Dedicated seeder thread + worker thread pool for parallel piece downloading. |
| **Communication** | Direct peer-to-peer piece downloads over TCP sockets with SHA1 checksum verification. | Async task execution with `pwrite` thread-safe file writing. |

### 4.1. Tracker Failover & High Availability
- **Heartbeat Monitoring**: T2 periodically checks T1's liveness.
- **Failover**: If T1 fails, clients automatically reconnect to T2. T2 logs all state updates into `updates.txt`.
- **Sync Back**: When T1 comes back online, T2 replays logged state changes from `updates.txt` to sync T1 up to date.

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

## 6. Optimization Algorithms & Implementation Details

1. **ThreadPool Downloader**: Avoids thread-creation overhead by reusing worker threads. Uses $2 \times \text{CPU cores}$ for parallel network piece fetching.
2. **512 KB Piece Partitioning**: Files are segmented into 512 KB pieces. Each piece hash is verified against SHA1 hashes retrieved from the tracker before saving to disk via `pwrite`.
3. **Seeder Promotion**: Upon successful completion of a file download, the client automatically notifies the tracker via `promote_seeder` to register itself as an active seeder for other leechers.
