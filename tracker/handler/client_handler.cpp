#include "handler/client_handler.h"
#include "state/tracker_state.h"
#include "sync/tracker_sync.h"
#include "utils/tracker_utils.h"
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace std;

void client_handler(int client_socket)
{
    char *incoming_message = new char[msg_buff_size];
    string current_user = "";

    while (true)
    {
        memset(incoming_message, 0, msg_buff_size);
        
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
            
            if (incoming_message[total_received - 1] == '\0')
                break;
        }
        
        if (total_received <= 0)
        {
            break;
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
                string file_path = command_parts[2];
                string group_id = command_parts[3];
                long long file_size = stoll(command_parts[4]);
                string piece_hashes = command_parts[5];
                string user_id = command_parts[6];

                lock_guard<mutex> lock(global_data_lock);

                file_metadata[file_path] = {file_size, piece_hashes};

                auto &group_files_list = group_files[group_id];
                if (find(group_files_list.begin(), group_files_list.end(), file_path) == group_files_list.end())
                {
                    group_files_list.push_back(file_path);
                }

                auto &owners_list = file_owners[file_path];
                if (find(owners_list.begin(), owners_list.end(), user_id) == owners_list.end())
                {
                    owners_list.push_back(user_id);
                }

                response_message = "sync done (upload_file)";
            }
            else if (command_parts[1] == "stop_share")
            {
                string file_name = command_parts[2];
                string user_id = command_parts[3];

                lock_guard<mutex> lock(global_data_lock);

                auto it = file_owners.find(file_name);
                if (it != file_owners.end())
                {
                    auto &owners_list = it->second;
                    auto user_it = find(owners_list.begin(), owners_list.end(), user_id);
                    if (user_it != owners_list.end())
                    {
                        owners_list.erase(user_it);
                        response_message = "sync done (stop_share)";
                    }
                    else
                    {
                        response_message = "sync: user not found in owners list.";
                    }
                }
                else
                {
                    response_message = "sync: file not found in file_owners.";
                }
            }
            else if (command_parts[1] == "logout")
            {
                lock_guard<mutex> lock(global_data_lock);
                string user_to_logout = command_parts[2];
                logged_in_clients.erase(user_to_logout);
                response_message = "sync applied (logout).";
            }
            else if (command_parts[1] == "stop_share_user")
            {
                lock_guard<mutex> lock(global_data_lock);
                string file_name = command_parts[2];
                string user_id = command_parts[3];

                if (file_owners.count(file_name))
                {
                    auto &owners = file_owners[file_name];
                    auto it = find(owners.begin(), owners.end(), user_id);
                    if (it != owners.end())
                    {
                        owners.erase(it);
                    }
                }
                response_message = "sync applied (stop_share_user).";
            }
            else if (command_parts[1] == "promote_seeder")
            {
                lock_guard<mutex> lock(global_data_lock);
                string file_name = command_parts[2];
                string user_id = command_parts[3];

                if (file_owners.count(file_name))
                {
                    auto &owners = file_owners[file_name];
                    auto it = find(owners.begin(), owners.end(), user_id);
                    if (it == owners.end())
                    {
                        owners.push_back(user_id);
                    }
                }
                response_message = "sync applied (promote_seeder).";
            }
            else if (command_parts[1] == "delete_file")
            {
                string group_id = command_parts[2];
                string file_name = command_parts[3];

                lock_guard<mutex> lock(global_data_lock);

                file_owners.erase(file_name);
                file_metadata.erase(file_name);

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
            {
                lock_guard<mutex> lock(global_data_lock);
                string user = command_parts[2];
                string ip_port = command_parts[3];
                logged_in_clients[user] = ip_port;
            }

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
                synchronize_with_peer("SYNC create_user " + command_parts[1] + " " + command_parts[2]);
            }
        }
        else if (command_parts[0] == "login")
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
                if (command_parts.size() >= 4)
                {
                    logged_in_clients[user] = command_parts[3];
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
                                synchronize_with_peer("SYNC change_owner " + group_id + " " + new_owner);
                            }
                            else
                            {
                                group_to_members.erase(group_id);
                                group_to_owner.erase(group_id);
                                group_to_requests.erase(group_id);
                                response_message = " you left the group. The group has been deleted.";
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
                                synchronize_with_peer("SYNC delete_group " + group_id);
                            }
                            else
                            {
                                response_message = " you left the group.";
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
            else if (command_parts.size() < 5)
            {
                response_message = " error: wrong format, need path group size hashes";
            }
            else
            {
                string file_path = command_parts[1];
                string group_id = command_parts[2];
                long long file_size = stoll(command_parts[3]);
                string piece_hashes = command_parts[4];

                lock_guard<mutex> lock(global_data_lock);

                auto it = group_to_members.find(group_id);
                if (it == group_to_members.end() ||
                    find(it->second.begin(), it->second.end(), current_user) == it->second.end())
                {
                    response_message = " error: group not found or you are not a member";
                }
                else
                {
                    file_metadata[file_path] = {file_size, piece_hashes};

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
                response_message = " error: missing file/destination arguments";
            }
            else
            {
                string group_id = command_parts[1];
                string file_name = command_parts[2];

                lock_guard<mutex> lock(global_data_lock);

                auto group_it = group_to_members.find(group_id);
                if (group_it == group_to_members.end() ||
                    find(group_it->second.begin(), group_it->second.end(), current_user) == group_it->second.end())
                {
                    response_message = " error: group does not exist or you are not a member";
                }
                else if (file_metadata.find(file_name) == file_metadata.end())
                {
                    response_message = " error: file metadata not found";
                }
                else
                {
                    long long file_size = file_metadata[file_name].first;
                    string piece_hashes = file_metadata[file_name].second;

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
                response_message = " error: please login first";
            }
            else if (command_parts.size() < 3)
            {
                response_message = " error: wrong format, need group_id file_name";
            }
            else
            {
                string group_id = command_parts[1];
                string file_name = command_parts[2];

                lock_guard<mutex> lock(global_data_lock);

                if (file_metadata.find(file_name) == file_metadata.end())
                {
                    response_message = " error: file metadata not found on tracker.";
                }
                else
                {
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
                            owners_list.erase(it);
                            response_message = " successfully stopped sharing file " + file_name;

                            if (owners_list.empty())
                            {
                                file_owners.erase(it_owners);
                                file_metadata.erase(file_name);

                                auto &group_file_list = group_files[group_id];
                                auto file_it = find(group_file_list.begin(), group_file_list.end(), file_name);
                                if (file_it != group_file_list.end())
                                {
                                    group_file_list.erase(file_it);
                                }

                                response_message += " (file deleted from system because no one is sharing it)";

                                synchronize_with_peer("SYNC delete_file " + group_id + " " + file_name);
                            }
                            else
                            {
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
                (void)group_id;
                lock_guard<mutex> lock(global_data_lock);

                if (file_owners.find(file_name) == file_owners.end())
                {
                    response_message = " error: file not tracked. promotion failed.";
                }
                else
                {
                    auto &owners = file_owners[file_name];
                    auto it = find(owners.begin(), owners.end(), current_user);

                    if (it == owners.end())
                    {
                        owners.push_back(current_user);
                        response_message = " successfully promoted to seeder for " + file_name + ".";

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

                logged_in_clients.erase(user_to_logout);

                for (auto &pair : file_owners)
                {
                    vector<string> &owners = pair.second;
                    auto it = find(owners.begin(), owners.end(), user_to_logout);
                    if (it != owners.end())
                    {
                        owners.erase(it);
                        synchronize_with_peer("SYNC stop_share_user " + pair.first + " " + user_to_logout);
                    }
                }

                current_user = "";
                response_message = " you have logged out and stopped sharing all files.";

                synchronize_with_peer("SYNC logout " + user_to_logout);
            }
        }
        else if (command_parts[0] == "list_files")
        {
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

        if (command_parts[0] != "SYNC")
        {
            send(client_socket, &response_message[0], response_message.length() + 1, MSG_NOSIGNAL);
        }
    }
    delete[] incoming_message;
    close(client_socket);
}
