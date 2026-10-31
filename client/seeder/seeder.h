#ifndef SEEDER_H
#define SEEDER_H

#include <string>

extern volatile int global_seeder_port;

void act_as_seeder();
void handle_peer_request(int peer_socket);

// Files this peer is sharing: name known to the tracker -> path on this disk.
// The seeder only serves files in this list.
void add_shared_file(const std::string &file_name, const std::string &local_path);
void remove_shared_file(const std::string &file_name);
void clear_shared_files();

#endif // SEEDER_H
