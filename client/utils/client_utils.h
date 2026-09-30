#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include <string>
#include <vector>

#define msg_buff_size 200000

void check(int exp, const char *msg);
std::string get_local_path(const std::string &file_name);
std::string calculate_file_metadata(const std::string &file_path, long long &file_size);
void read_tracker_info(std::string filename, std::vector<std::string> &info);
size_t get_optimal_thread_count();

#endif // CLIENT_UTILS_H
