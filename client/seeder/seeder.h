#ifndef SEEDER_H
#define SEEDER_H

extern volatile int global_seeder_port;

void act_as_seeder();
void handle_peer_request(int peer_socket);

#endif // SEEDER_H
