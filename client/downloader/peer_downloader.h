#ifndef PEER_DOWNLOADER_H
#define PEER_DOWNLOADER_H

#include <string>
#include <vector>

int open_peer_connection(const std::string &seeder_address);
int select_next_piece_index(const std::vector<std::string> &all_piece_hashes, const std::vector<bool> &local_bitfield);
bool download_piece_from_peer(
    const std::string &file_name,
    int piece_index,
    const std::string &seeder_address,
    const std::string &expected_hash,
    int dest_fd);
void download_file_pieces(
    const std::string &group_id,
    const std::string &file_name,
    const std::string &dest_path,
    const std::vector<std::string> &seeders,
    const std::vector<std::string> &expected_hashes);

#endif // PEER_DOWNLOADER_H
