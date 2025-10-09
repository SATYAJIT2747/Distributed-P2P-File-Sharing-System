#include <iostream>
#include <unistd.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <signal.h> // added to ignore SIGPIPE because after socket was closed after 1st sync the process was also killed so this
// prevents the tracker from being killed when writing to a closed socket.

#define msg_buff_size 524288 // 512KB for large file metadata

using namespace std;
volatile bool is_main_tracker_up = true; // we will assume main tracker is  alive initially
string my_ip_port;
string main_tracker_ip_port; // store current and main tracker addresses.

int my_tracker_number; // mstores whether this tracker is T1 or T2.

void synchronize_with_peer(const string &message);

// username -> password
unordered_map<string, string> registered_users;

// groupname -> list of usernames
unordered_map<string, vector<string>> group_to_members;

// groupname -> owner username
unordered_map<string, string> group_to_owner;

// groupname -> pending join requests
unordered_map<string, vector<string>> group_to_requests;

//  data structures for file tracking
// file path -> file metadata like its SHA, size, piece hashes
unordered_map<string, string> file_to_hashes;

// grp id  -> list of files  shared in that group
unordered_map<string, vector<string>> group_files;

// file path ->list of users who have the complete file
unordered_map<string, vector<string>> file_owners;

// file path -> (size, piece hashes)
unordered_map<string, pair<long long, string>> file_metadata;

// user -> ip: port of the client's active seeder thread
unordered_map<string, string> logged_in_clients;

// lock to prevent race conditions
mutex global_data_lock;

string peer_ip_port; // store peer address globally so all functions can use it

void ensure_success(int result, const char *error_message)
{
    if (result < 0)
    {
        perror(error_message);
        exit(1);
    }
}

void log_update_to_file(const string &message)
{
    ofstream log_file("updates.txt", ios::app); // append mode
    if (log_file.is_open())
    {
        log_file << message << endl;
        log_file.close();
    }
}

void recover_from_log()
{
    ifstream log_file("updates.txt");
    if (!log_file.is_open())
        return;

    cout << "Recovering state from log file..." << endl;
    string line;
    while (getline(log_file, line))
    {
        synchronize_with_peer(line); // replay update to peer
        usleep(100000);              // 100 ms
                                     //  small delay
    }
    log_file.close();

    ofstream clear_file("updates.txt", ios::trunc); // clear log after recovery
    clear_file.close();
    cout << "recovery complete. log text file is  cleared." << endl;
}

// T2 WILL PERIODICALLY CHECK T1 THAT ITS RUNNING OR DOWN

void heartbeat_thread()
{
    bool was_main_tracker_up = true; // track previous state

    while (true)
    {
        if (my_tracker_number == 2)
        { // only T2 backup checks main
            int main_socket;
            struct sockaddr_in main_address;

            // create socket
            main_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (main_socket < 0)
            {
                cerr << "unable to create socket for heartbeat." << endl;
                sleep(5);
                continue;
            }

            // extract main tracker IP and port
            string main_ip = main_tracker_ip_port.substr(0, main_tracker_ip_port.find(":"));
            int main_port = stoi(main_tracker_ip_port.substr(main_tracker_ip_port.find(":") + 1));

            bzero(&main_address, sizeof(main_address));
            main_address.sin_family = AF_INET;
            main_address.sin_port = htons(main_port);
            inet_pton(AF_INET, &main_ip[0], &main_address.sin_addr);

            // try connecting to main tracker
            if (connect(main_socket, (struct sockaddr *)&main_address, sizeof(main_address)) < 0)
            {
                is_main_tracker_up = false;
                if (was_main_tracker_up)
                {
                    cout << "Main tracker is down. T2 will take over..." << endl;
                }
            }
            else
            {
                is_main_tracker_up = true;
                if (!was_main_tracker_up)
                {
                    cout << "Main tracker is back online. Initiating recovery..." << endl;
                    recover_from_log(); // replay logged updates
                }
                close(main_socket);
            }

            was_main_tracker_up = is_main_tracker_up;
        }

        sleep(5); // periodic check
    }
}

// send updates to the peer tracker whenever our state changes
void synchronize_with_peer(const string &message)
{
    // If main tracker is down and this is T2, log the update instead of sending
    if (!is_main_tracker_up && my_tracker_number == 2)
    {
        log_update_to_file(message); // save the update for later
        cout << "Main tracker is down. Logging update: " << message << endl;
        return;
    }

    int peer_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_socket < 0)
    {
        cerr << "Error creating sync socket" << endl;
        return;
    }

    string peer_ip = peer_ip_port.substr(0, peer_ip_port.find(":"));
    int peer_port = stoi(peer_ip_port.substr(peer_ip_port.find(":") + 1));

    struct sockaddr_in peer_address;
    bzero(&peer_address, sizeof(peer_address));
    peer_address.sin_family = AF_INET;
    peer_address.sin_port = htons(peer_port);
    inet_pton(AF_INET, &peer_ip[0], &peer_address.sin_addr);

    // try to connect
    if (connect(peer_socket, (struct sockaddr *)&peer_address, sizeof(peer_address)) < 0)
    {
        cerr << "Could not connect to peer for sync, maybe offline" << endl;
        is_main_tracker_up = false;
        close(peer_socket);

        if (my_tracker_number == 2)
        {
            log_update_to_file(message);
            cout << "Sync failed. Logged update for later." << endl;
        }
        return;
    }

    // debugging message
    cout << "Syncing with peer now... message: \"" << message << "\"" << endl;

    // send message (avoid SIGPIPE)
    if (send(peer_socket, &message[0], message.length() + 1, MSG_NOSIGNAL) < 0)
    {
        perror("Send to peer failed");
        close(peer_socket);

        if (my_tracker_number == 2)
        {
            log_update_to_file(message);
            cout << "Send failed. Logged update for later." << endl;
        }
        return;
    }

    close(peer_socket);
}

// full client handler with all command handling
void client_handler(int client_socket)
{
    char *incoming_message = new char[msg_buff_size];
    string current_user = "";

    while (true)
    {
        memset(incoming_message, 0, msg_buff_size);
        
        // Read all available data (may need multiple recv calls for large messages)
        ssize_t total_received = 0;
        ssize_t bytes_received = 0;
        
        while (total_received < msg_buff_size - 1)
        {
            bytes_received = recv(client_socket, 
                                 incoming_message + total_received, 
                                 msg_buff_size - total_received - 1, 
                                 0);
            
            if (bytes_received <= 0)
                break;
            
            total_received += bytes_received;
            
            // Check if we got a null terminator (end of message)
            if (incoming_message[total_received - 1] == '\0')
                break;
        }
        
        if (total_received <= 0)
        {
            break; // client disconnected
        }

        vector<string> command_parts;
        char *saveptr;
        char *word = strtok_r(incoming_message, " ", &saveptr);
        while (word != NULL)
        {
            command_parts.push_back(string(word));
            word = strtok_r(NULL, " ", &saveptr);
        }

        string response_message;

        if (command_parts.empty())
        {
            response_message = "err: no command given.";
        }
        else if (command_parts[0] == "SYNC")
        {
            // incoming sync from peer
            if (command_parts[1] == "create_user")
            {
                lock_guard<mutex> lock(global_data_lock);
                registered_users[command_parts[2]] = command_parts[3];
            }
            else if (command_parts[1] == "create_group")
            {
                lock_guard<mutex> lock(global_data_lock);
                group_to_members[command_parts[2]].push_back(command_parts[3]);
                group_to_owner[command_parts[2]] = command_parts[3];
            }
            else if (command_parts[1] == "join_group")
            {
                lock_guard<mutex> lock(global_data_lock);
                group_to_requests[command_parts[2]].push_back(command_parts[3]);
            }
            else if (command_parts[1] == "accept_request")
            {
                lock_guard<mutex> lock(global_data_lock);
                auto &requests = group_to_requests[command_parts[2]];
                auto it = find(requests.begin(), requests.end(), command_parts[3]);
                if (it != requests.end())
                {
                    requests.erase(it);
                    group_to_members[command_parts[2]].push_back(command_parts[3]);
                }
            }
            else if (command_parts[1] == "leave_group")
            {
                lock_guard<mutex> lock(global_data_lock);
                auto &members = group_to_members[command_parts[2]];
                auto it = find(members.begin(), members.end(), command_parts[3]);
                if (it != members.end())
                    members.erase(it);
            }
            else if (command_parts[1] == "change_owner")
            {
                lock_guard<mutex> lock(global_data_lock);
                group_to_owner[command_parts[2]] = command_parts[3];
            }
            else if (command_parts[1] == "delete_group")
            {
                lock_guard<mutex> lock(global_data_lock);
                group_to_members.erase(command_parts[2]);
                group_to_owner.erase(command_parts[2]);
                group_to_requests.erase(command_parts[2]);
            }

            else if (command_parts[1] == "upload_file")
            {
                // SYNC upload_file <path> <group_id> <size> <hashes> <user_id>
                string file_path = command_parts[2];
                string group_id = command_parts[3];
                long long file_size = stoll(command_parts[4]);
                string piece_hashes = command_parts[5];
                string user_id = command_parts[6];

                lock_guard<mutex> lock(global_data_lock);

                // 1. update file info
                file_metadata[file_path] = {file_size, piece_hashes};

                // 2. add file to group list if not there
                auto &group_files_list = group_files[group_id];
                if (find(group_files_list.begin(), group_files_list.end(), file_path) == group_files_list.end())
                {
                    group_files_list.push_back(file_path);
                }

                // 3. add user as owner if not already
                auto &owners_list = file_owners[file_path];
                if (find(owners_list.begin(), owners_list.end(), user_id) == owners_list.end())
                {
                    owners_list.push_back(user_id);
                }

                response_message = "sync done (upload_file)";
            }

            // handle stop_share sync from another tracker
            else if (command_parts[1] == "stop_share")
            {
                // expected format: SYNC stop_share <file_name> <user_id>
                string file_name = command_parts[2];
                string user_id = command_parts[3];

                lock_guard<mutex> lock(global_data_lock); // make sure no one else is changing the data at the same time

                auto it = file_owners.find(file_name);
                if (it != file_owners.end())
                {
                    auto &owners_list = it->second;
                    auto user_it = find(owners_list.begin(), owners_list.end(), user_id);
                    if (user_it != owners_list.end())
                    {
                        owners_list.erase(user_it);                  // removed the user from the owners list
                        response_message = "sync done (stop_share)"; // let tracker know sync was successful
                    }
                    else
                    {
                        response_message = "sync: user not found in owners list."; // user wasn't sharing, nothing to remove
                    }
                }
                else
                {
                    response_message = "sync: file not found in file_owners."; // file not tracked at all
                }
            }

            else if (command_parts[1] == "logout")
            {
                // sync: remove a user who logged out (other tracker told us)
                lock_guard<mutex> lock(global_data_lock);
                string user_to_logout = command_parts[2];
                logged_in_clients.erase(user_to_logout);
                response_message = "sync applied (logout).";
            }

            else if (command_parts[1] == "stop_share_user")
            {
                // sync: remove this user from the list of seeders for a file if logout
                lock_guard<mutex> lock(global_data_lock);
                string file_name = command_parts[2];
                string user_id = command_parts[3];

                if (file_owners.count(file_name))
                {
                    auto &owners = file_owners[file_name];
                    auto it = find(owners.begin(), owners.end(), user_id);
                    if (it != owners.end())
                    {
                        owners.erase(it); // remove user if found
                    }
                }
                response_message = "sync applied (stop_share_user).";
            }

            else if (command_parts[1] == "promote_seeder")
            {
                // this is a sync message from another tracker: promote_seeder <file_name> <user_id>
                lock_guard<mutex> lock(global_data_lock);
                string file_name = command_parts[2];
                string user_id = command_parts[3];

                // check if the file is tracked
                if (file_owners.count(file_name))
                {
                    auto &owners = file_owners[file_name];
                    // add this user to the seeder list if not already there
                    auto it = find(owners.begin(), owners.end(), user_id);
                    if (it == owners.end())
                    {
                        owners.push_back(user_id);
                    }
                }
                response_message = "sync applied (promote_seeder).";
            }

            //  handle file deletion from peer tracker -
            else if (command_parts[1] == "delete_file")
            {
                // SYNC delete_file <group_id> <file_name>
                string group_id = command_parts[2];
                string file_name = command_parts[3];

                lock_guard<mutex> lock(global_data_lock); // lock so no one else messes with these maps while we update

                // remove the file from our main maps
                file_owners.erase(file_name);
                file_metadata.erase(file_name);

                // also remove it from the group's file list
                auto group_it = group_files.find(group_id);
                if (group_it != group_files.end())
                {
                    auto &group_file_list = group_it->second;
                    auto file_it = find(group_file_list.begin(), group_file_list.end(), file_name);
                    if (file_it != group_file_list.end())
                    {
                        group_file_list.erase(file_it);
                    }
                }

                response_message = "sync done (delete_file)";
            }

            else if (command_parts[1] == "login")
            { // sync login info from peer tracker

                lock_guard<mutex> lock(global_data_lock);
                string user = command_parts[2];
                string ip_port = command_parts[3];
                logged_in_clients[user] = ip_port;
            }

            // reply to peer
            response_message = "sync applied.";
        }
        else if (command_parts[0] == "create_user")
        {
            lock_guard<mutex> lock(global_data_lock);
            if (registered_users.count(command_parts[1]))
            {
                response_message = " user already exists.";
            }
            else
            {
                registered_users[command_parts[1]] = command_parts[2];
                response_message = " user created.";
                // forward to peer
                synchronize_with_peer("SYNC create_user " + command_parts[1] + " " + command_parts[2]);
            }
        }
        else if (command_parts[0] == "login") // save client ip:port when they login, forward to peer

        {
            lock_guard<mutex> lock(global_data_lock);
            string user = command_parts[1];
            string pass = command_parts[2];

            if (!registered_users.count(user) || registered_users[user] != pass)
            {
                response_message = " wrong username or password.";
            }
            else
            {
                current_user = user;
                // expect ip:port from client
                if (command_parts.size() >= 4)
                {
                    logged_in_clients[user] = command_parts[3];
                    // forward to peer tracker
                    synchronize_with_peer("SYNC login " + user + " " + command_parts[3]);
                }
                response_message = " login successful.";
            }
        }

        else if (command_parts[0] == "create_group")
        {
            if (current_user.empty())
            {
                response_message = " login first.";
            }
            else
            {
                lock_guard<mutex> lock(global_data_lock);
                if (group_to_members.count(command_parts[1]))
                {
                    response_message = " group already exists.";
                }
                else
                {
                    group_to_members[command_parts[1]].push_back(current_user);
                    group_to_owner[command_parts[1]] = current_user;
                    response_message = " group created.";
                    // forward to peer
                    synchronize_with_peer("SYNC create_group " + command_parts[1] + " " + current_user);
                }
            }
        }
        else if (command_parts[0] == "list_groups")
        {
            lock_guard<mutex> lock(global_data_lock);
            if (group_to_members.empty())
            {
                response_message = "no groups available.";
            }
            else
            {
                response_message = "groups:";
                for (auto it : group_to_members)
                {
                    response_message += " " + it.first;
                }
            }
        }
        else if (command_parts[0] == "join_group")
        {
            if (current_user.empty())
            {
                response_message = " please login first.";
            }
            else
            {
                string group_id = command_parts[1];
                lock_guard<mutex> lock(global_data_lock);
                if (!group_to_members.count(group_id))
                {
                    response_message = " group does not exist.";
                }
                else if (find(group_to_members[group_id].begin(), group_to_members[group_id].end(), current_user) != group_to_members[group_id].end())
                {
                    response_message = " you are already a member of this group.";
                }
                else
                {
                    group_to_requests[group_id].push_back(current_user);
                    response_message = " join request sent.";
                    // forward to peer
                    synchronize_with_peer("SYNC join_group " + group_id + " " + current_user);
                }
            }
        }
        else if (command_parts[0] == "list_requests")
        {
            if (current_user.empty())
            {
                response_message = " please login first.";
            }
            else
            {
                string group_id = command_parts[1];
                lock_guard<mutex> lock(global_data_lock);
                if (!group_to_owner.count(group_id) || group_to_owner[group_id] != current_user)
                {
                    response_message = " you are not the owner of this group.";
                }
                else if (!group_to_requests.count(group_id) || group_to_requests[group_id].empty())
                {
                    response_message = " no pending requests.";
                }
                else
                {
                    response_message = "pending requests:";
                    for (auto user_id : group_to_requests[group_id])
                    {
                        response_message += " " + user_id;
                    }
                }
            }
        }
        else if (command_parts[0] == "accept_request")
        {
            if (current_user.empty())
            {
                response_message = " please login first.";
            }
            else
            {
                string group_id = command_parts[1];
                string user_to_add = command_parts[2];
                lock_guard<mutex> lock(global_data_lock);
                if (!group_to_owner.count(group_id) || group_to_owner[group_id] != current_user)
                {
                    response_message = " you are not the owner of this group.";
                }
                else
                {
                    auto &requests = group_to_requests[group_id];
                    auto it = find(requests.begin(), requests.end(), user_to_add);
                    if (it != requests.end())
                    {
                        requests.erase(it);
                        group_to_members[group_id].push_back(user_to_add);
                        response_message = " request accepted.";
                        // forward to peer
                        synchronize_with_peer("SYNC accept_request " + group_id + " " + user_to_add);
                    }
                    else
                    {
                        response_message = " user has not requested to join.";
                    }
                }
            }
        }
        else if (command_parts[0] == "leave_group")
        {
            if (current_user.empty())
            {
                response_message = " please login first.";
            }
            else
            {
                string group_id = command_parts[1];
                lock_guard<mutex> lock(global_data_lock);
                if (!group_to_members.count(group_id))
                {
                    response_message = " group does not exist.";
                }
                else
                {
                    auto &members = group_to_members[group_id];
                    auto it = find(members.begin(), members.end(), current_user);
                    if (it == members.end())
                    {
                        response_message = " you are not a member of this group.";
                    }
                    else
                    {
                        members.erase(it);
                        if (group_to_owner[group_id] == current_user)
                        {
                            if (!members.empty())
                            {
                                string new_owner = members[0];
                                group_to_owner[group_id] = new_owner;
                                response_message = "You left the group. New owner is " + new_owner;
                                // forward owner change
                                synchronize_with_peer("SYNC change_owner " + group_id + " " + new_owner);
                            }
                            else
                            {
                                group_to_members.erase(group_id);
                                group_to_owner.erase(group_id);
                                group_to_requests.erase(group_id);
                                response_message = " you left the group. The group has been deleted.";
                                // forward group deletion
                                synchronize_with_peer("SYNC delete_group " + group_id);
                            }
                        }
                        else
                        {
                            if (members.empty())
                            {
                                group_to_members.erase(group_id);
                                group_to_owner.erase(group_id);
                                group_to_requests.erase(group_id);
                                response_message = " you left the group. The group has been deleted.";
                                // forward group deletion
                                synchronize_with_peer("SYNC delete_group " + group_id);
                            }
                            else
                            {
                                response_message = " you left the group.";
                                // forward leave
                                synchronize_with_peer("SYNC leave_group " + group_id + " " + current_user);
                            }
                        }
                    }
                }
            }
        }

        else if (command_parts[0] == "upload_file")
        {
            if (current_user.empty())
            {
                response_message = " error: please login first";
            }
            // format must be: upload_file <path> <group_id> <size> <hashes>
            else if (command_parts.size() < 5)
            {
                response_message = " error: wrong format, need path group size hashes";
            }
            else
            {
                string file_path = command_parts[1];
                string group_id = command_parts[2];
                long long file_size = stoll(command_parts[3]); // convert string to number
                string piece_hashes = command_parts[4];

                lock_guard<mutex> lock(global_data_lock);

                // check if group exists and user is in it
                auto it = group_to_members.find(group_id);
                if (it == group_to_members.end() ||
                    find(it->second.begin(), it->second.end(), current_user) == it->second.end())
                {
                    response_message = " error: group not found or you are not a member";
                }
                else
                {
                    // save file info
                    file_metadata[file_path] = {file_size, piece_hashes};

                    // add file into group list (only once)
                    bool is_new_file = true;
                    auto &group_files_list = group_files[group_id];
                    if (find(group_files_list.begin(), group_files_list.end(), file_path) == group_files_list.end())
                    {
                        group_files_list.push_back(file_path);
                    }
                    else
                    {
                        is_new_file = false;
                    }

                    // add current user as file owner
                    auto &owners_list = file_owners[file_path];
                    if (find(owners_list.begin(), owners_list.end(), current_user) == owners_list.end())
                    {
                        owners_list.push_back(current_user);
                    }

                    if (is_new_file)
                    {
                        response_message = " file uploaded successfully";
                    }
                    else
                    {
                        response_message = " file already existed, metadata updated";
                    }

                    // sync with backup tracker
                    string sync_msg = "SYNC upload_file " + file_path + " " + group_id + " " +
                                      command_parts[3] + " " + command_parts[4] + " " + current_user;
                    synchronize_with_peer(sync_msg);
                }
            }
        }

        else if (command_parts[0] == "download_file")
        {
            if (current_user.empty())
            {
                response_message = " error: please login first";
            }
            else if (command_parts.size() < 4)
            {
                // download_file <group_id> <file_name> <destination_path>
                response_message = " error: missing file/destination arguments";
            }
            else
            {
                string group_id = command_parts[1];
                string file_name = command_parts[2];
                // string dest_path = command_parts[3]; // client handles destination

                lock_guard<mutex> lock(global_data_lock);

                // 1. check if user is member of group
                auto group_it = group_to_members.find(group_id);
                if (group_it == group_to_members.end() ||
                    find(group_it->second.begin(), group_it->second.end(), current_user) == group_it->second.end())
                {
                    response_message = " error: group does not exist or you are not a member";
                }
                // 2. check if file exists in metadata
                else if (file_metadata.find(file_name) == file_metadata.end())
                {
                    response_message = " error: file metadata not found";
                }
                else
                {
                    // 3. get file size and hashes
                    long long file_size = file_metadata[file_name].first;
                    string piece_hashes = file_metadata[file_name].second;

                    // 4. find active seeders
                    const vector<string> &owners_list = file_owners[file_name];
                    string active_seeder_addresses = "";
                    int active_count = 0;

                    for (const string &owner_id : owners_list)
                    {
                        if (logged_in_clients.count(owner_id))
                        {
                            active_seeder_addresses += logged_in_clients[owner_id] + " ";
                            active_count++;
                        }
                    }

                    if (active_count > 0)
                    {
                        // send size + hashes + seeder list
                        // main.cpp tracker change:
                        response_message = "metadata_and_seeders " + to_string(file_size) +
                                           " " + piece_hashes + "  " + active_seeder_addresses;
                    }
                    else
                    {
                        response_message = " error: no active seeders found for this file";
                    }
                }
            }
        }

        else if (command_parts[0] == "stop_share")
        {
            if (current_user.empty())
            {
                response_message = " error: please login first"; // user not logged in, tell them
            }
            else if (command_parts.size() < 3)
            {
                response_message = " error: wrong format, need group_id file_name"; // missing info, show correct usage
            }

            else
            {
                string group_id = command_parts[1];
                string file_name = command_parts[2];

                lock_guard<mutex> lock(global_data_lock); // make sure no one else messes with the data while we're updating it

                // 1. check if the tracker even knows about this file
                if (file_metadata.find(file_name) == file_metadata.end())
                {
                    response_message = " error: file metadata not found on tracker.";
                }
                else
                {
                    // 2. check if the current user is actually sharing this file
                    auto it_owners = file_owners.find(file_name);

                    if (it_owners == file_owners.end())
                    {
                        response_message = " error: no active seeders recorded for this file.";
                    }
                    else
                    {
                        auto &owners_list = it_owners->second;
                        auto it = find(owners_list.begin(), owners_list.end(), current_user);

                        if (it != owners_list.end())
                        {
                            // a. success: remove this user from the seeders list
                            owners_list.erase(it);
                            response_message = " successfully stopped sharing file " + file_name;

                            // b. check if anyone else is still sharing it
                            if (owners_list.empty())
                            {
                                // no one else left, delete all info about this file
                                file_owners.erase(it_owners);
                                file_metadata.erase(file_name);

                                // remove file from the group's file list too
                                auto &group_file_list = group_files[group_id];
                                auto file_it = find(group_file_list.begin(), group_file_list.end(), file_name);
                                if (file_it != group_file_list.end())
                                {
                                    group_file_list.erase(file_it);
                                }

                                response_message += " (file deleted from system because no one is sharing it)";

                                // tell the peer tracker that this file got deleted
                                synchronize_with_peer("SYNC delete_file " + group_id + " " + file_name);
                            }
                            else
                            {
                                // file still has other seeders, just sync the stop_share action
                                string sync_msg = "SYNC stop_share " + file_name + " " + current_user;
                                synchronize_with_peer(sync_msg);
                            }
                        }
                        else
                        {
                            response_message = " error: you are not currently sharing this file.";
                        }
                    }
                }
            }
        }

        else if (command_parts[0] == "promote_seeder")
        {
            if (current_user.empty())
            {
                response_message = " error: please login first.";
            }
            else if (command_parts.size() < 3)
            {
                response_message = " error: missing file_name or group_id.";
            }
            else
            {
                string file_name = command_parts[1];
                string group_id = command_parts[2];
                lock_guard<mutex> lock(global_data_lock);

                // check if this file is already tracked
                if (file_owners.find(file_name) == file_owners.end())
                {
                    response_message = " error: file not tracked. promotion failed.";
                }
                // add the current user to the seeder list
                else
                {
                    auto &owners = file_owners[file_name];
                    auto it = find(owners.begin(), owners.end(), current_user);

                    if (it == owners.end())
                    {
                        owners.push_back(current_user);
                        response_message = " successfully promoted to seeder for " + file_name + ".";

                        // sync this info with other trackers/peers
                        synchronize_with_peer("SYNC promote_seeder " + file_name + " " + current_user);
                    }
                    else
                    {
                        response_message = " you are already listed as a seeder for " + file_name + ".";
                    }
                }
            }
        }

        else if (command_parts[0] == "logout")
        {
            if (current_user.empty())
            {
                response_message = " you are not logged in.";
            }
            else
            {
                lock_guard<mutex> lock(global_data_lock);
                string user_to_logout = current_user;

                // remove the user from the list of people who are logged in
                logged_in_clients.erase(user_to_logout);

                // go through every file and remove this user from the seeder/owner list
                // (basically stop sharing all their files)
                for (auto &pair : file_owners)
                {
                    vector<string> &owners = pair.second;
                    auto it = find(owners.begin(), owners.end(), user_to_logout);
                    if (it != owners.end())
                    {
                        owners.erase(it);
                        // also tell the other tracker that this user stopped sharing
                        synchronize_with_peer("SYNC stop_share_user " + pair.first + " " + user_to_logout);
                    }
                }

                // reset the current_user since they logged out
                current_user = "";
                response_message = " you have logged out and stopped sharing all files.";

                // also sync the logout event with the peer tracker
                synchronize_with_peer("SYNC logout " + user_to_logout);
            }
        }

        else if (command_parts[0] == "list_files")
        { // show all files in a group with size

            if (current_user.empty())
            {
                response_message = " login first.";
            }
            else
            {
                string group_id = command_parts[1];
                lock_guard<mutex> lock(global_data_lock);

                if (group_files.find(group_id) == group_files.end() || group_files[group_id].empty())
                {
                    response_message = " no files shared in this group.";
                }
                else
                {
                    response_message = "files:";
                    for (auto &file : group_files[group_id])
                    {
                        auto &meta = file_metadata[file];
                        response_message += " " + file + "(" + to_string(meta.first) + " bytes)";
                    }
                }
            }
        }

        else if (command_parts[0] == "quit")
        {
            response_message = " goodbye!";
            send(client_socket, &response_message[0], response_message.length() + 1, MSG_NOSIGNAL);
            break;
        }
        else
        {
            response_message = "err: unknown command.";
        }

        // reply to client
        if (command_parts[0] != "SYNC")
        { // ONLY reply if it was a real client command
            send(client_socket, &response_message[0], response_message.length() + 1, MSG_NOSIGNAL);
        }
    }
    delete[] incoming_message; // free memory before closing
    close(client_socket);
}

// same read_tracker_info() as before
void read_tracker_info(string filename, vector<string> &info)
{
    ifstream file(filename);
    if (!file.is_open())
    {
        cerr << " could not open tracker info file." << endl;
        exit(1);
    }
    string line;
    while (getline(file, line))
        info.push_back(line);
    file.close();
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN); // ignore SIGPIPE so tracker doesn't crash if socket closed

    if (argc < 3)
    {
        cerr << "the command should look like : ./tracker <tracker_info_file> <tracker_number>" << endl;
        return -1;
    }

    // read tracker info from file
    vector<string> tracker_info;
    string info_filename = argv[1];
    read_tracker_info(info_filename, tracker_info);

    // get tracker number (1 = T1, 2 = T2)
    int tracker_number = stoi(argv[2]);
    my_tracker_number = tracker_number;

    // set IP:PORT of self and peer tracker
    if (tracker_number == 1)
    {
        my_ip_port = tracker_info[0];
        main_tracker_ip_port = tracker_info[0]; // T1 itself
        peer_ip_port = tracker_info[1];         // T2 address
    }
    else if (tracker_number == 2)
    {
        my_ip_port = tracker_info[1];
        main_tracker_ip_port = tracker_info[0]; // T1 address
        peer_ip_port = tracker_info[0];         // T1 address (peer)
    }
    else
    {
        cerr << "err0r: not a valid tracker number. use eithe 1 or 2." << endl;
        return -1;
    }

    int my_port = stoi(my_ip_port.substr(my_ip_port.find(":") + 1));
    int main_tracker_port = stoi(main_tracker_ip_port.substr(main_tracker_ip_port.find(":") + 1));

    cout << "my address: " << my_ip_port << endl;
    cout << "peer address: " << peer_ip_port << endl;

    // start heartbeat thread named hb_thread for backup tracker to check T1 status
    if (tracker_number == 2)
    {
        thread hb_thread(heartbeat_thread);
        hb_thread.detach();
    }

    // recover pending updates if T2 starts while T1 is down
    if (tracker_number == 2 && !is_main_tracker_up)
    {
        cout << "T1 is down at startup. Recovering pending updates..." << endl;
        recover_from_log();
    }

    int tracker_socket;
    struct sockaddr_in tracker_address, client_address;

    // create tracker socket
    ensure_success((tracker_socket = socket(AF_INET, SOCK_STREAM, 0)), "socket creation is failed!");

    bzero(&tracker_address, sizeof(tracker_address));
    tracker_address.sin_family = AF_INET;
    tracker_address.sin_addr.s_addr = INADDR_ANY;

    // if T2 and main tracker is down, bind to main tracker port to temporarily take over
    int listening_port = my_port;
    if (tracker_number == 2 && !is_main_tracker_up)
    {
        listening_port = main_tracker_port;
    }
    tracker_address.sin_port = htons(listening_port);

    ensure_success((bind(tracker_socket, (struct sockaddr *)&tracker_address, sizeof(tracker_address))), "bind failed!");
    ensure_success((listen(tracker_socket, 10)), "listen failed!");

    cout << "tracker is listening on the port ..:" << listening_port << endl;

    // accept client connections in infinite loop
    while (true)
    {
        socklen_t client_address_length = sizeof(client_address);

        int client_socket = accept(tracker_socket, (struct sockaddr *)&client_address, &client_address_length);
        ensure_success(client_socket, "accept failed!");
        // cout << "client connected from ip: " << inet_ntoa(client_address.sin_addr) << endl;

        // create a thread and pass socket by value so that each one have its own copy of the socket
        thread(client_handler, client_socket).detach();
    }

    close(tracker_socket);
    return 0;
}
