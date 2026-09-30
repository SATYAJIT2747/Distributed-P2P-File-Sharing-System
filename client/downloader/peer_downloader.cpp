#include "downloader/peer_downloader.h"
#include "downloader/download_manager.h"
#include "downloader/thread_pool.h"
#include "utils/crypto_utils.h"
#include "utils/client_utils.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>

using namespace std;

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

int select_next_piece_index(const vector<string> &all_piece_hashes, const vector<bool> &local_bitfield)
{
    (void)all_piece_hashes;
    for (size_t i = 0; i < local_bitfield.size(); i++)
        if (!local_bitfield[i])
            return i;
    return -1;
}

bool download_piece_from_peer(
    const string &file_name,
    int piece_index,
    const string &seeder_address,
    const string &expected_hash,
    int dest_fd)
{
    const int PIECE_SIZE = 512 * 1024;

    int peer_socket = open_peer_connection(seeder_address);
    if (peer_socket < 0)
    {
        cerr << "warning: failed to connect to seeder " << seeder_address
             << " for piece " << piece_index << endl;
        return false;
    }

    string request_msg = "REQUEST_PIECE " + file_name + " " + to_string(piece_index);
    send(peer_socket, request_msg.c_str(), request_msg.length() + 1, 0);

    char piece_buffer[PIECE_SIZE];
    ssize_t bytes_received = 0;
    while (bytes_received < PIECE_SIZE)
    {
        ssize_t n = recv(peer_socket, piece_buffer + bytes_received, PIECE_SIZE - bytes_received, 0);
        if (n <= 0)
            break;
        bytes_received += n;
    }

    close(peer_socket);

    if (bytes_received <= 0)
    {
        cerr << "warning: received 0 bytes for piece " << piece_index
             << " from " << seeder_address << endl;
        return false;
    }

    string received_hash = calculate_sha1(piece_buffer, bytes_received);

    if (received_hash != expected_hash)
    {
        cerr << "warning: integrity check failed for piece " << piece_index
             << ". received hash: " << received_hash << endl;
        return false;
    }

    lock_guard<mutex> lock(file_write_mutex);
    if (pwrite(dest_fd, piece_buffer, bytes_received, (off_t)piece_index * PIECE_SIZE) != bytes_received)
    {
        cerr << "error: thread-safe write failed for piece " << piece_index << endl;
        return false;
    }

    cout << "downloaded piece " << piece_index << " successfully from " << seeder_address << endl;
    return true;
}

void download_file_pieces(
    const string &group_id,
    const string &file_name,
    const string &dest_path,
    const vector<string> &seeders,
    const vector<string> &expected_hashes)
{
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

    size_t num_threads = get_optimal_thread_count();
    ThreadPool pool(num_threads);

    for (int piece_index = 0; piece_index < total_pieces; piece_index++)
    {
        pool.enqueue([=, &local_bitfield, &bitfield_mutex, &pieces_downloaded, &seeders, &expected_hashes]()
                     {
            bool success = false;
            int max_retries = 3;
            int retry_count = 0;

            while(!success && retry_count < max_retries) {
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
                    usleep(100000);
                }
            }

            if(success) {
                lock_guard<mutex> lock(bitfield_mutex);
                local_bitfield[piece_index] = true;
                pieces_downloaded++;
                
                if(pieces_downloaded % 10 == 0 || pieces_downloaded == total_pieces) {
                    cout << "Progress: " << pieces_downloaded << "/" << total_pieces 
                         << " pieces (" << (pieces_downloaded * 100 / total_pieces) << "%)" << endl;
                }
            } else {
                cerr << "Failed to download piece " << piece_index 
                     << " after " << max_retries << " retries" << endl;
            } });
    }

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
