#include "downloader/download_manager.h"

std::unordered_map<std::string, DownloadInfo> download_status_map;
std::mutex download_status_mutex;
std::mutex file_write_mutex;
