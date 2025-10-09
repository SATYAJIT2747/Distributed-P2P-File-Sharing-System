#include <iostream>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <openssl/sha.h>
#include <stdio.h>
#include <mutex>
#include <errno.h>
#include <unordered_map>
#include <queue>
#include <functional>
#include <condition_variable>
#include <atomic>

using namespace std;

#define msg_buff_size 200000

enum DownloadStatus
{
    IN_PROGRESS,
    COMPLETED
};

struct DownloadInfo
{
    string group_id;
    string file_name;
    DownloadStatus status;
};

// Thread Pool Manager
class ThreadPool
{
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    atomic<bool> stop;
    atomic<int> active_tasks;

public:
    ThreadPool(size_t num_threads) : stop(false), active_tasks(0)
    {
        for (size_t i = 0; i < num_threads; ++i)
        {
            workers.emplace_back([this]
                                 {
                while(true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { 
                            return this->stop || !this->tasks.empty(); 
                        });
                        
                        if(this->stop && this->tasks.empty())
                            return;
                        
                        task = move(this->tasks.front());
                        this->tasks.pop();
                    }
                    active_tasks++;
                    task();
                    active_tasks--;
                } });
        }
    }

    template <class F>
    void enqueue(F &&f)
    {
        {
            unique_lock<mutex> lock(queue_mutex);
            tasks.emplace(forward<F>(f));
        }
        condition.notify_one();
    }

    void wait_until_idle()
    {
        while (true)
        {
            {
                unique_lock<mutex> lock(queue_mutex);
                if (tasks.empty() && active_tasks == 0)
                    return;
            }
            usleep(10000); // 10ms
        }
    }

    int get_active_count()
    {
        return active_tasks.load();
    }

    ~ThreadPool()
    {
        stop = true;
        condition.notify_all();
        for (thread &worker : workers)
            if (worker.joinable())
                worker.join();
    }
};

//  key = file_name + group_id so that each download is unique
// value will be the info about that download
unordered_map<string, DownloadInfo> download_status_map;
mutex download_status_mutex;

// global lock to make sure only one thread writes to the file at a time
mutex file_write_mutex;

void handle_peer_request(int peer_socket);

// check if something failed
void check(int exp, const char *msg)
{
    if (exp < 0)
    {
        perror(msg);
        exit(1);
    }
}

// global variable for seeder port
volatile int global_seeder_port = 0;

// Seeder
void act_as_seeder()
{
    int seeder_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (seeder_socket == -1)
    {
        cerr << "couldn't create seeder socket\n";
        return;
    }

    sockaddr_in seeder_address;
    bzero(&seeder_address, sizeof(seeder_address));
    seeder_address.sin_family = AF_INET;
    seeder_address.sin_addr.s_addr = INADDR_ANY;
    seeder_address.sin_port = 0;

    if (bind(seeder_socket, (struct sockaddr *)&seeder_address, sizeof(seeder_address)) == -1)
    {
        perror("bind failed");
        close(seeder_socket);
        return;
    }

    socklen_t len = sizeof(seeder_address);
    if (getsockname(seeder_socket, (struct sockaddr *)&seeder_address, &len) == -1)
    {
        perror("getsockname failed");
        close(seeder_socket);
        return;
    }
    int seeder_port = ntohs(seeder_address.sin_port);
    global_seeder_port = seeder_port;

    if (listen(seeder_socket, 5) == -1)
    {
        cerr << "listen failed\n";
        close(seeder_socket);
        return;
    }

    cout << "Seeder listening on port " << seeder_port << endl;

    while (true)
    {
        sockaddr_in peer_addr;
        socklen_t addr_size = sizeof(peer_addr);
        int peer_socket = accept(seeder_socket, (struct sockaddr *)&peer_addr, &addr_size);
        if (peer_socket < 0)
        {
            perror("accept failed");
            continue;
        }
        cout << "Seeder got a peer connection\n";
        thread(handle_peer_request, peer_socket).detach();
    }
    close(seeder_socket);
}

//  SHA1 helpers
string sha1_to_hex(unsigned char *hash)
{
    char hex_str[41];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
    {
        sprintf(&hex_str[i * 2], "%02x", hash[i]);
    }
    hex_str[40] = '\0';
    return string(hex_str);
}

string calculate_sha1(const char *data, size_t len)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)data, len, hash);
    return sha1_to_hex(hash);
}

// --- Seeder request handler ---
void handle_peer_request(int peer_socket)
{
    char request_buffer[msg_buff_size];
    bzero(request_buffer, msg_buff_size);
    if (recv(peer_socket, request_buffer, msg_buff_size, 0) <= 0)
    {
        close(peer_socket);
        return;
    }

    vector<string> parts;
    stringstream ss(request_buffer);
    string token;
    while (ss >> token)
    {
        parts.push_back(token); // removed strtok as not thread-safe
    }

    if (parts.size() < 3 || parts[0] != "REQUEST_PIECE")
    {
        close(peer_socket);
        return;
    }

    string file_name = parts[1];
    int piece_index = stoi(parts[2]);
    const int PIECE_SIZE = 512 * 1024;
    string local_path = "./" + file_name;

    int fd = open(local_path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        cerr << "Cannot open " << local_path << endl;
        close(peer_socket);
        return;
    }

    if (lseek(fd, (off_t)piece_index * PIECE_SIZE, SEEK_SET) == (off_t)-1)
    {
        cerr << "lseek failed\n";
        close(fd);
        close(peer_socket);
        return;
    }

    char piece_data[PIECE_SIZE];
    ssize_t bytes_read = read(fd, piece_data, PIECE_SIZE);
    close(fd);

    if (bytes_read > 0)
    {
        ssize_t total_sent = 0;
        while (total_sent < bytes_read)
        {
            ssize_t n = send(peer_socket,
                             piece_data + total_sent,
                             bytes_read - total_sent,
                             MSG_NOSIGNAL); // Use MSG_NOSIGNAL to prevent a crash if the peer disconnects during send as crashing

            if (n < 0)
            {
                if (errno == EINTR)
                    continue; // interrupted, retry
                perror("send");
                break;
            }
            if (n == 0)
                break; // connection closed
            total_sent += n;
        }

        if (total_sent != bytes_read)
        {
            cerr << "Warning: only " << total_sent
                 << " of " << bytes_read << " bytes sent for piece "
                 << piece_index << endl;
        }
    }
    close(peer_socket);
}

// File helpers
string get_local_path(const string &file_name) { return "./" + file_name; }

string calculate_file_metadata(const string &file_path, long long &file_size)
{
    ifstream file(file_path, ios::binary | ios::ate);
    if (!file.is_open())
    {
        file_size = 0;
        return "";
    }
    file_size = file.tellg();
    file.seekg(0, ios::beg);

    const int PIECE_SIZE = 512 * 1024;
    char buffer[PIECE_SIZE];
    string concatenated_hashes;
    int piece_index = 0;

    while (!file.eof())
    {
        file.read(buffer, PIECE_SIZE);
        long bytes_read = file.gcount();
        if (!bytes_read)
            break;
        concatenated_hashes += calculate_sha1(buffer, bytes_read);
    }
    file.close();
    return concatenated_hashes;
}

// Peer connection
int open_peer_connection(const string &seeder_address)
{
    string peer_ip = seeder_address.substr(0, seeder_address.find(":"));
    int peer_port = stoi(seeder_address.substr(seeder_address.find(":") + 1));
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    sockaddr_in peer_addr;
    bzero(&peer_addr, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(peer_port);
    inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0)
    {
        perror("connect");
        close(sock);
        return -1;
    }
    return sock;
}

// Piece selection
int select_next_piece_index(const vector<string> &all_piece_hashes, const vector<bool> &local_bitfield)
{
    for (size_t i = 0; i < local_bitfield.size(); i++)
        if (!local_bitfield[i])
            return i;
    return -1;
}

// Tracker info
void read_tracker_info(string filename, vector<string> &info)
{
    ifstream file(filename);
    if (!file.is_open())
    {
        cerr << "cannot open tracker file\n";
        exit(1);
    }
    string line;
    while (getline(file, line))
        info.push_back(line);
    file.close();
}

// this function tries to download a single piece of a file from one specific peer/seeder
bool download_piece_from_peer(
    const string &file_name,      // name of the file we want
    int piece_index,              // which piece number we want
    const string &seeder_address, // address of the seeder we are requesting from
    const string &expected_hash,  // hash to check integrity
    int dest_fd)                  // file descriptor of the destination file
{
    const int PIECE_SIZE = 512 * 1024; // size of each piece in bytes (512 KB)

    // open a connection to the seeder
    int peer_socket = open_peer_connection(seeder_address);
    if (peer_socket < 0)
    {
        // if connection fails, we just return false to retry later
        cerr << "warning: failed to connect to seeder " << seeder_address
             << " for piece " << piece_index << endl;
        return false;
    }

    // send request for this specific piece to the peer
    string request_msg = "REQUEST_PIECE " + file_name + " " + to_string(piece_index);
    send(peer_socket, request_msg.c_str(), request_msg.length() + 1, 0);

    // buffer to receive the piece data    changed    recv() to loop until all bytes are read becoz get to know that TCP does not guarantee you’ll get the full piece in a single recv() call.
    char piece_buffer[PIECE_SIZE];
    ssize_t bytes_received = 0;
    while (bytes_received < PIECE_SIZE)
    {
        ssize_t n = recv(peer_socket, piece_buffer + bytes_received, PIECE_SIZE - bytes_received, 0);
        if (n <= 0)
            break; // error or closed connection
        bytes_received += n;
    }

    // close the socket after receiving
    close(peer_socket);

    if (bytes_received <= 0)
    {
        // if no data received, warn and return false
        cerr << "warning: received 0 bytes for piece " << piece_index
             << " from " << seeder_address << endl;
        return false;
    }

    // calculate hash of received piece to verify integrity
    string received_hash = calculate_sha1(piece_buffer, bytes_received);

    if (received_hash != expected_hash)
    {
        // if hashes don't match, integrity failed, must retry
        cerr << "warning: integrity check failed for piece " << piece_index
             << ". received hash: " << received_hash << endl;
        return false;
    }

    // thread-safe write to the correct place in the file
    lock_guard<mutex> lock(file_write_mutex); // only one thread writes at a time
    if (pwrite(dest_fd, piece_buffer, bytes_received, (off_t)piece_index * PIECE_SIZE) != bytes_received)
    {
        cerr << "error: thread-safe write failed for piece " << piece_index << endl;
        return false;
    }

    // success message
    cout << "downloaded piece " << piece_index << " successfully from " << seeder_address << endl;
    return true; // piece downloaded and verified successfully
}

// Helper function to get optimal thread count
size_t get_optimal_thread_count()
{
    unsigned int hw_threads = thread::hardware_concurrency();
    if (hw_threads == 0)
        hw_threads = 4; // fallback

    // Use 2x CPU cores for I/O bound tasks (network downloads)
    size_t optimal = hw_threads * 2;

    cout << "System has " << hw_threads << " hardware threads" << endl;
    cout << "Using " << optimal << " threads for downloads" << endl;

    return optimal;
}

// Updated download_file_pieces with thread pool
void download_file_pieces(
    const string &group_id,
    const string &file_name,
    const string &dest_path,
    const vector<string> &seeders,
    const vector<string> &expected_hashes)
{
    const int PIECE_SIZE = 512 * 1024;
    int total_pieces = expected_hashes.size();

    if (total_pieces == 0)
    {
        cerr << "error: no pieces to download.\n";
        return;
    }
    if (seeders.empty())
    {
        cerr << "error: no seeders available.\n";
        return;
    }

    vector<bool> local_bitfield(total_pieces, false);
    mutex bitfield_mutex;
    atomic<int> pieces_downloaded(0);

    int fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0)
    {
        cerr << "cannot open destination file " << dest_path << endl;
        return;
    }

    cout << "starting download of " << total_pieces << " pieces from "
         << seeders.size() << " seeders.\n";

    // Create thread pool with optimal size
    size_t num_threads = get_optimal_thread_count();
    ThreadPool pool(num_threads);

    // Enqueue all download tasks
    for (int piece_index = 0; piece_index < total_pieces; piece_index++)
    {
        pool.enqueue([=, &local_bitfield, &bitfield_mutex, &pieces_downloaded, &seeders, &expected_hashes]()
                     {
            bool success = false;
            int max_retries = 3;
            int retry_count = 0;

            while(!success && retry_count < max_retries) {
                // Round-robin or random seeder selection
                int seeder_idx = (piece_index + retry_count) % seeders.size();
                string seeder_address = seeders[seeder_idx];

                success = download_piece_from_peer(
                    file_name,
                    piece_index,
                    seeder_address,
                    expected_hashes[piece_index],
                    fd
                );

                if(!success) {
                    retry_count++;
                    cerr << "retry " << retry_count << " for piece " << piece_index << endl;
                    usleep(100000); // 100ms before retry
                }
            }

            if(success) {
                lock_guard<mutex> lock(bitfield_mutex);
                local_bitfield[piece_index] = true;
                pieces_downloaded++;
                
                // Progress indicator
                if(pieces_downloaded % 10 == 0 || pieces_downloaded == total_pieces) {
                    cout << "Progress: " << pieces_downloaded << "/" << total_pieces 
                         << " pieces (" << (pieces_downloaded * 100 / total_pieces) << "%)" << endl;
                }
            } else {
                cerr << "Failed to download piece " << piece_index 
                     << " after " << max_retries << " retries" << endl;
            } });
    }

    // Wait for all tasks to complete
    pool.wait_until_idle();
    close(fd);

    string key = file_name + group_id;

    if (pieces_downloaded == total_pieces)
    {
        cout << "download complete! All " << total_pieces << " pieces downloaded.\n";

        lock_guard<mutex> lock(download_status_mutex);
        if (download_status_map.count(key))
            download_status_map[key].status = COMPLETED;

        // Promote to seeder
        string promotion_msg = "promote_seeder " + file_name + " " + group_id;
        int temp_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (temp_sock >= 0)
        {
            struct sockaddr_in server_address;
            string tracker_ip = "127.0.0.1";
            int tracker_port = 8989;

            bzero(&server_address, sizeof(server_address));
            server_address.sin_family = AF_INET;
            server_address.sin_port = htons(tracker_port);
            inet_pton(AF_INET, tracker_ip.c_str(), &server_address.sin_addr);

            if (connect(temp_sock, (struct sockaddr *)&server_address, sizeof(server_address)) >= 0)
            {
                send(temp_sock, promotion_msg.c_str(), promotion_msg.length() + 1, 0);
                close(temp_sock);
                cout << "told tracker: now seeding " << file_name << ".\n";
            }
        }
    }
    else
    {
        cerr << "download failed, only " << pieces_downloaded << "/" << total_pieces
             << " pieces downloaded.\n";
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        cerr << "usage: ./client <IP>:<PORT> <tracker_info_file>\n";
        return -1;
    }

    // Parse IP:PORT from first argument
    string client_ip_port = argv[1];
    string client_ip = client_ip_port.substr(0, client_ip_port.find(":"));
    int client_port = stoi(client_ip_port.substr(client_ip_port.find(":") + 1));

    vector<string> tracker_info;
    read_tracker_info(argv[2], tracker_info); // tracker_info.txt is now second argument
    string tracker1_address = tracker_info[0];
    string tracker2_address = tracker_info[1];

    int client_socket;
    struct sockaddr_in server_address;

    string main_ip = tracker1_address.substr(0, tracker1_address.find(":"));
    int main_port = stoi(tracker1_address.substr(tracker1_address.find(":") + 1));

    string backup_ip = tracker2_address.substr(0, tracker2_address.find(":"));
    int backup_port = stoi(tracker2_address.substr(tracker2_address.find(":") + 1));

    check((client_socket = socket(AF_INET, SOCK_STREAM, 0)), "socket failed");
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(main_port);
    inet_pton(AF_INET, main_ip.c_str(), &server_address.sin_addr);

    if (connect(client_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        close(client_socket);
        cout << "main tracker down, trying backup...\n";
        check((client_socket = socket(AF_INET, SOCK_STREAM, 0)), "socket failed");
        bzero(&server_address, sizeof(server_address));
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(backup_port);
        inet_pton(AF_INET, backup_ip.c_str(), &server_address.sin_addr);
        check(connect(client_socket, (struct sockaddr *)&server_address, sizeof(server_address)), "both trackers down");
        cout << "connected to backup tracker\n";
    }
    else
        cout << "connected to main tracker\n";

    char server_response[msg_buff_size];
    string user_input;

    thread seeder_thread(act_as_seeder);
    while (global_seeder_port == 0)
        usleep(10000);

    cout << "commands: create_user, login <user> <pass>, logout , create_group, list_groups, join_group, list_requests, accept_request, leave_group, logout, upload_file <group_id> <file_path>, list_files <group_id>, download_file <group_id> <file_name> <destination_path>,  stop_share ,quit\n";

    // store names  gid if a download command was just sent
    string pending_download_group_id = "";
    string pending_download_file_name = "";
    string pending_download_dest_path = "";

    while (true)
    {
        getline(cin, user_input);
        if (user_input == "quit")
            break;

        if (user_input.find("login") == 0)
            user_input += " 127.0.0.1:" + to_string(global_seeder_port);

        //  upload_file
        if (user_input.find("upload_file") == 0)
        {
            stringstream ss(user_input);
            string command, group_id, file_path;
            ss >> command >> group_id >> file_path;

            if (ss.fail() || file_path.empty())
            {
                cout << "error: use like -> upload_file <group_id> <file_path>\n";
                continue;
            }

            long long file_size;
            string concatenated_hashes = calculate_file_metadata(file_path, file_size);

            if (file_size == 0 || concatenated_hashes.empty())
            {
                cout << "error: file is empty or cannot be opened\n";
                continue;
            }

            string tracker_msg = command + " " + file_path + " " + group_id + " " +
                                 to_string(file_size) + " " + concatenated_hashes;

            send(client_socket, tracker_msg.c_str(), tracker_msg.length() + 1, 0);
        }
        // download_file logic
        else if (user_input.find("download_file") == 0)
        {
            stringstream ss(user_input);
            string command, group_id, file_name, dest_path;
            ss >> command >> group_id >> file_name >> dest_path;

            if (ss.fail() || dest_path.empty())
            {
                cout << "error: use like -> download_file <group_id> <file_name> <destination_path>\n";
                continue;
            }
            pending_download_group_id = group_id; // store gid
            pending_download_file_name = file_name;
            pending_download_dest_path = dest_path;

            send(client_socket, user_input.c_str(), user_input.length() + 1, 0);
        }

        else if (user_input.find("stop_share") == 0)
        {
            stringstream ss(user_input);
            string command, group_id, file_name;
            ss >> command >> group_id >> file_name;

            // check if user typed the file name, otherwise show how to use
            if (ss.fail() || file_name.empty())
            {
                cout << "error: use like -> stop_share <group_id> <file_name>\n";
                continue;
            }

            // just send this command to the tracker
            send(client_socket, user_input.c_str(), user_input.length() + 1, 0);
        }

        // show_downloads
        else if (user_input == "show_downloads")
        {
            lock_guard<mutex> lock(download_status_mutex);

            // if nothing is being downloaded
            if (download_status_map.empty())
            {
                cout << "no downloads right now\n";
                continue;
            }

            cout << "--- current downloads ---\n";
            for (const auto &pair : download_status_map)
            {
                const DownloadInfo &info = pair.second;

                //  [C] for completed, [P] for in progress
                string status_char = (info.status == COMPLETED) ? "[C]" : "[P]";

                // format is : [C]/[P] group_id filename
                cout << status_char << " "
                     << info.group_id << " "
                     << info.file_name << endl;
            }

            continue; // don’t send this to tracker
        }

        else
        {
            pending_download_file_name = "";
            pending_download_dest_path = "";
            send(client_socket, user_input.c_str(), user_input.length() + 1, 0);
        }

        char server_response[200000]; // Use a larger buffer to match tracker's new size
        bzero(server_response, 200000);

        ssize_t bytes_recv = recv(client_socket, server_response, 200000, 0);

        if (bytes_recv <= 0)
        {
            cerr << "tracker disconnected or error occurred\n";
            break;
        }

        // Only construct the string from the received bytes up to the null terminator
        string response_str(server_response);

        cout << "server says: " << response_str << endl;

        // handle download response
        if (response_str.find("metadata_and_seeders") == 0)
        {
            stringstream ss(response_str.substr(string("metadata_and_seeders").length()));
            string s_file_size, s_concatenated_hashes;

            ss >> s_file_size >> s_concatenated_hashes;

            if (ss.fail())
            {
                cout << "error parsing file metadata from tracker\n";
                pending_download_file_name = "";
                pending_download_dest_path = "";
                pending_download_group_id = "";
                continue;
            }

            // Validate the extracted hash string (it should not contain the seeder address now)
            const int SHA1_HASH_LENGTH = 40;
            if (s_concatenated_hashes.length() % SHA1_HASH_LENGTH != 0)
            {
                cout << "error: concatenated hashes length is corrupt\n";
                pending_download_file_name = "";
                pending_download_dest_path = "";
                pending_download_group_id = "";
                continue;
            }

            long long file_size = stoll(s_file_size);

            vector<string> seeders;
            string addr;
            // This loop now only reads the seeder IP:PORT tokens
            while (ss >> addr)
                seeders.push_back(addr);

            cout << "tracker gave peers: ";
            for (auto &s : seeders)
                cout << s << " ";
            cout << endl;

            int total_pieces = s_concatenated_hashes.length() / SHA1_HASH_LENGTH;
            vector<string> piece_hashes(total_pieces);
            for (int i = 0; i < total_pieces; i++)
            {
                piece_hashes[i] = s_concatenated_hashes.substr(i * SHA1_HASH_LENGTH, SHA1_HASH_LENGTH);
            }

            if (!pending_download_file_name.empty() && !seeders.empty())
            {
                string key = pending_download_file_name + pending_download_group_id;

                //  mark this file as "in progress"
                {
                    lock_guard<mutex> lock(download_status_mutex);
                    download_status_map[key] = {
                        pending_download_group_id,
                        pending_download_file_name,
                        IN_PROGRESS};
                }

                // we will start the download in a new detached thread
                thread(
                    download_file_pieces,
                    pending_download_group_id, // pass group id too
                    pending_download_file_name,
                    pending_download_dest_path,
                    seeders,
                    piece_hashes)
                    .detach();

                cout << "download started in background. use 'show_downloads' to see progress.\n";
            }
            else if (!pending_download_file_name.empty() && seeders.empty())
            {
                // no seeders found even though metadata is there
                cout << "error: got file info but no active seeders.\n";
            }

            pending_download_file_name = "";
            pending_download_dest_path = "";
            pending_download_group_id = "";
        }
    }

    close(client_socket);
    seeder_thread.detach();
    return 0;
}
