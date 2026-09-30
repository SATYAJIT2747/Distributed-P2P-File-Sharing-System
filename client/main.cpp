#include "utils/client_utils.h"
#include "utils/crypto_utils.h"
#include "downloader/download_manager.h"
#include "downloader/peer_downloader.h"
#include "seeder/seeder.h"
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <sstream>
#include <mutex>

using namespace std;

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        cerr << "usage: ./client <IP>:<PORT> <tracker_info_file>\n";
        return -1;
    }

    string client_ip_port = argv[1];
    (void)client_ip_port;

    vector<string> tracker_info;
    read_tracker_info(argv[2], tracker_info);
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

    string user_input;

    thread seeder_thread(act_as_seeder);
    while (global_seeder_port == 0)
        usleep(10000);

    cout << "commands: create_user, login <user> <pass>, logout , create_group, list_groups, join_group, list_requests, accept_request, leave_group, logout, upload_file <group_id> <file_path>, list_files <group_id>, download_file <group_id> <file_name> <destination_path>,  stop_share ,quit\n";

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
            pending_download_group_id = group_id;
            pending_download_file_name = file_name;
            pending_download_dest_path = dest_path;

            send(client_socket, user_input.c_str(), user_input.length() + 1, 0);
        }
        else if (user_input.find("stop_share") == 0)
        {
            stringstream ss(user_input);
            string command, group_id, file_name;
            ss >> command >> group_id >> file_name;

            if (ss.fail() || file_name.empty())
            {
                cout << "error: use like -> stop_share <group_id> <file_name>\n";
                continue;
            }

            send(client_socket, user_input.c_str(), user_input.length() + 1, 0);
        }
        else if (user_input == "show_downloads")
        {
            lock_guard<mutex> lock(download_status_mutex);

            if (download_status_map.empty())
            {
                cout << "no downloads right now\n";
                continue;
            }

            cout << "--- current downloads ---\n";
            for (const auto &pair : download_status_map)
            {
                const DownloadInfo &info = pair.second;

                string status_char = (info.status == COMPLETED) ? "[C]" : "[P]";

                cout << status_char << " "
                     << info.group_id << " "
                     << info.file_name << endl;
            }

            continue;
        }
        else
        {
            pending_download_file_name = "";
            pending_download_dest_path = "";
            send(client_socket, user_input.c_str(), user_input.length() + 1, 0);
        }

        char server_response[200000];
        bzero(server_response, 200000);

        ssize_t bytes_recv = recv(client_socket, server_response, 200000, 0);

        if (bytes_recv <= 0)
        {
            cerr << "tracker disconnected or error occurred\n";
            break;
        }

        string response_str(server_response);

        cout << "server says: " << response_str << endl;

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
            (void)file_size;

            vector<string> seeders;
            string addr;
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

                {
                    lock_guard<mutex> lock(download_status_mutex);
                    download_status_map[key] = {
                        pending_download_group_id,
                        pending_download_file_name,
                        IN_PROGRESS};
                }

                thread(
                    download_file_pieces,
                    pending_download_group_id,
                    pending_download_file_name,
                    pending_download_dest_path,
                    seeders,
                    piece_hashes)
                    .detach();

                cout << "download started in background. use 'show_downloads' to see progress.\n";
            }
            else if (!pending_download_file_name.empty() && seeders.empty())
            {
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
