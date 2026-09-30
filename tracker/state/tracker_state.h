#ifndef TRACKER_STATE_H
#define TRACKER_STATE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

extern volatile bool is_main_tracker_up;
extern std::string my_ip_port;
extern std::string main_tracker_ip_port;
extern std::string peer_ip_port;
extern int my_tracker_number;

// Data structures for users, groups, files, and clients
extern std::unordered_map<std::string, std::string> registered_users;
extern std::unordered_map<std::string, std::vector<std::string>> group_to_members;
extern std::unordered_map<std::string, std::string> group_to_owner;
extern std::unordered_map<std::string, std::vector<std::string>> group_to_requests;
extern std::unordered_map<std::string, std::string> file_to_hashes;
extern std::unordered_map<std::string, std::vector<std::string>> group_files;
extern std::unordered_map<std::string, std::vector<std::string>> file_owners;
extern std::unordered_map<std::string, std::pair<long long, std::string>> file_metadata;
extern std::unordered_map<std::string, std::string> logged_in_clients;

extern std::mutex global_data_lock;

#endif // TRACKER_STATE_H
