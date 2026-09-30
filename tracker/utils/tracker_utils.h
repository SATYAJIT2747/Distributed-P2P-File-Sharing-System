#ifndef TRACKER_UTILS_H
#define TRACKER_UTILS_H

#include <string>
#include <vector>

#define msg_buff_size 524288

void ensure_success(int result, const char *error_message);
void read_tracker_info(std::string filename, std::vector<std::string> &info);

#endif // TRACKER_UTILS_H
