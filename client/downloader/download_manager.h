#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <string>
#include <unordered_map>
#include <mutex>

enum DownloadStatus
{
    IN_PROGRESS,
    COMPLETED
};

struct DownloadInfo
{
    std::string group_id;
    std::string file_name;
    DownloadStatus status;
};

// Map key = file_name + group_id
extern std::unordered_map<std::string, DownloadInfo> download_status_map;
extern std::mutex download_status_mutex;

// Global lock to make sure only one thread writes to the file at a time
extern std::mutex file_write_mutex;

#endif // DOWNLOAD_MANAGER_H
