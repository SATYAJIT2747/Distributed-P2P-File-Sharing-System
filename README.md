

---

# Peer-to-Peer Distributed File Sharing System (DFSS)

---

## 1. How to Compile and Run

To compile and run this project, I need the OpenSSL library (for calculating SHA1 hashes) and a standard g++ compiler.

### 1.1. Compilation

I include the necessary libraries (`-pthread` for threads and `-lcrypto` for SHA1).

**Command Format:**

```bash
g++ -o <executable_name> <source_file>.cpp -std=c++17 -pthread -lcrypto
```

**Actual Commands:**

```bash
# Compile the Tracker (creates the 'tracker' executable)
g++ -o tracker tracker.cpp -std=c++17 -pthread -lcrypto  or make

# Compile the Client (creates the 'client' executable)
g++ -o client client.cpp -std=c++17 -pthread -lcrypto or make
```

### 1.2. Setup File: `tracker_info.txt`

Before running anything, I create a file named **`tracker_info.txt`** that contains the IP and port for both the main tracker (T1) and the backup tracker (T2).

**Example `tracker_info.txt`:**

```
127.0.0.1:8989
127.0.0.1:8990
```

### 1.3. Execution

I run the two trackers first, and then I start multiple client peers.

**Start Tracker 1 (T1 - Main):**

```bash
# Terminal 1
./tracker tracker_info.txt 1
```

**Start Tracker 2 (T2 - Backup):**

```bash
# Terminal 2
./tracker tracker_info.txt 2
```

**Start Client:**

```bash
# Terminal 3 (or 4, etc.)
./client <Client_IP>:<Client_Port> tracker_info.txt

# Example:
./client 127.0.0.1:5000 tracker_info.txt
```

---

## 2. Architectural Overview (Hybrid P2P Design)

My system uses a **Hybrid P2P Architecture**. I use centralized servers (Trackers) only for metadata, while actual file transfers happen directly between peers.

| Component         | Role                                                                      | Concurrency                                                      |
| ----------------- | ------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| Tracker (T1 & T2) | Keeps track of users, groups, file hashes, and IP:PORT of active seeders. | Multithreaded – spawns a new thread for every client connection. |
| Client (Peer)     | Acts as both downloader (Leecher) and uploader (Seeder).                  | Runs a Seeder Thread and a Thread Pool for downloading.          |
| Communication     | All done using TCP sockets for reliability.                               | TCP chosen for guaranteed delivery and correctness.              |

### 2.1. Tracker Synchronization (High Availability)

* **Primary/Backup Model**: T1 is primary, T2 is backup.
* **Heartbeat**: T2 constantly pings T1.
* **Failover**: If T1 goes down, T2 takes over and logs updates.
* **Recovery**: When T1 returns, T2 replays updates to sync.

---

## 3. Key Algorithms and Data Structures

### 3.1. Download Optimization: Thread Pool

* Initially, spawning a thread per piece slowed performance.
* I implemented a **Thread Pool** (≈2×CPU cores).
* Benefit: Reduced 1GB download time by ~95%.

### 3.2. Piece Strategy and Integrity

* **Strategy**: Sequential piece download.
* **Check**: SHA1 hash per 512 KB piece.
* **Retries**: Up to 3 attempts with seeder switching.

### 3.3. Data Structures Used

```cpp
// username -> password
unordered_map<string, string> registered_users;

// groupname -> list of usernames
unordered_map<string, vector<string>> group_to_members;

// groupname -> owner username
unordered_map<string, string> group_to_owner;

// groupname -> pending join requests
unordered_map<string, vector<string>> group_to_requests;

// file path -> file metadata (SHA, size, piece hashes)
unordered_map<string, string> file_to_hashes;

// group id -> list of files shared
unordered_map<string, vector<string>> group_files;

// file path -> list of users with complete file
unordered_map<string, vector<string>> file_owners;

// file path -> (size, piece hashes)
unordered_map<string, pair<long long, string>> file_metadata;

// user -> ip:port of active client
unordered_map<string, string> logged_in_clients;

// lock to prevent race conditions
mutex global_data_lock;
```

---

## 4. Network Protocol Design and Message Formats

### 4.1. Tracker Protocol (Client → Tracker)

Commands are space-separated strings.

* **Login**:

  ```
  login <user> <pass> <ip:port>
  ```
* **Upload**:

  ```
  upload_file <path> <group> <size> <hashes>
  ```
* **Download Response**:

  ```
  metadata_and_seeders <size> <hashes> <ip1:port1> <ip2:port2> ...
  ```

### 4.2. Peer-to-Peer Protocol (Client → Seeder)

* **Request**:

  ```
  REQUEST_PIECE <file_name> <piece_index>
  ```
* **Response**:
  Binary 512KB chunk.

---

## 5. Features, Limitations, and Assumptions

| Status | Feature               | Notes                                                                |
| ------ | --------------------- | -------------------------------------------------------------------- |
| Done | User/Group Management | create_user, login, create_group, join_group, accept_request, logout |
|  Done | File Operations       | upload_file, list_files, download_file, stop_share, show_downloads   |
|  Done | High Availability     | Tracker failover with sync back                                      |

**Assumptions**:

* `tracker_info.txt` contains valid addresses.
* Client IP:Port is reachable.
* File paths are relative to client’s directory.

---

## 6. Testing Procedures

### 6.1. Basic Functionality Test

```bash
# Terminal 1 - Tracker 1
./tracker tracker_info.txt 1

# Terminal 2 - Tracker 2
./tracker tracker_info.txt 2

# Terminal 3 - Client 1 (Seeder)
./client 127.0.0.1:5000 tracker_info.txt
create_user seeder pass
login seeder pass
create_group TestGroup
upload_file TestGroup fivehundred.bin
list_files TestGroup

# Terminal 4 - Client 2 (Leecher)
./client 127.0.0.1:5001 tracker_info.txt
create_user leech pass
login leech pass
list_groups
join_group TestGroup

# Back to Client 1
list_requests TestGroup
accept_request TestGroup leech

# Back to Client 2
list_files TestGroup
download_file TestGroup fivehundred.bin ./downloaded_file.bin
show_downloads
```

### 6.2. Large File (1GB)

```bash
dd if=/dev/urandom of=onegb.bin bs=1M count=1024
upload_file TestGroup onegb.bin
download_file TestGroup onegb.bin ./onegb_downloaded.bin
show_downloads
```

### 6.3. Tracker Failover

* Kill T1 during download (`Ctrl+C`).
* Client auto-switches to T2.
* Restart T1 → T2 syncs back updates.

### 6.4. `stop_share`

```bash
stop_share TestGroup fivehundred.bin
```

* File remains listed but with no seeders.

### 6.5. `logout`

```bash
logout
```

* Seeder stops sharing all files.


