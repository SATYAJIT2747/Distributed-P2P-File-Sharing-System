#ifndef TRACKER_SYNC_H
#define TRACKER_SYNC_H

#include <string>

void log_update_to_file(const std::string &message);
void recover_from_log();
void heartbeat_thread();
void synchronize_with_peer(const std::string &message);

#endif // TRACKER_SYNC_H
