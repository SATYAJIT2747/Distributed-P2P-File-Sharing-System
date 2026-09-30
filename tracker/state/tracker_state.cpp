#include "state/tracker_state.h"

using namespace std;

volatile bool is_main_tracker_up = true;
string my_ip_port;
string main_tracker_ip_port;
string peer_ip_port;
int my_tracker_number = 0;

unordered_map<string, string> registered_users;
unordered_map<string, vector<string>> group_to_members;
unordered_map<string, string> group_to_owner;
unordered_map<string, vector<string>> group_to_requests;
unordered_map<string, string> file_to_hashes;
unordered_map<string, vector<string>> group_files;
unordered_map<string, vector<string>> file_owners;
unordered_map<string, pair<long long, string>> file_metadata;
unordered_map<string, string> logged_in_clients;

mutex global_data_lock;
