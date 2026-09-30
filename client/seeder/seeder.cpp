#include "seeder/seeder.h"
#include "utils/client_utils.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <thread>
#include <sstream>
#include <vector>
#include <cerrno>
#include <cstdio>

using namespace std;

volatile int global_seeder_port = 0;

void handle_peer_request(int peer_socket)
{
    char request_buffer[msg_buff_size];
    bzero(request_buffer, msg_buff_size);
    if (recv(peer_socket, request_buffer, msg_buff_size, 0) <= 0)
    {
        close(peer_socket);
        return;
    }

    vector<string> parts;
    stringstream ss(request_buffer);
    string token;
    while (ss >> token)
    {
        parts.push_back(token);
    }

    if (parts.size() < 3 || parts[0] != "REQUEST_PIECE")
    {
        close(peer_socket);
        return;
    }

    string file_name = parts[1];
    int piece_index = stoi(parts[2]);
    const int PIECE_SIZE = 512 * 1024;
    string local_path = "./" + file_name;

    int fd = open(local_path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        cerr << "Cannot open " << local_path << endl;
        close(peer_socket);
        return;
    }

    if (lseek(fd, (off_t)piece_index * PIECE_SIZE, SEEK_SET) == (off_t)-1)
    {
        cerr << "lseek failed\n";
        close(fd);
        close(peer_socket);
        return;
    }

    char piece_data[PIECE_SIZE];
    ssize_t bytes_read = read(fd, piece_data, PIECE_SIZE);
    close(fd);

    if (bytes_read > 0)
    {
        ssize_t total_sent = 0;
        while (total_sent < bytes_read)
        {
            ssize_t n = send(peer_socket,
                             piece_data + total_sent,
                             bytes_read - total_sent,
                             MSG_NOSIGNAL);

            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("send");
                break;
            }
            if (n == 0)
                break;
            total_sent += n;
        }

        if (total_sent != bytes_read)
        {
            cerr << "Warning: only " << total_sent
                 << " of " << bytes_read << " bytes sent for piece "
                 << piece_index << endl;
        }
    }
    close(peer_socket);
}

void act_as_seeder()
{
    int seeder_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (seeder_socket == -1)
    {
        cerr << "couldn't create seeder socket\n";
        return;
    }

    sockaddr_in seeder_address;
    bzero(&seeder_address, sizeof(seeder_address));
    seeder_address.sin_family = AF_INET;
    seeder_address.sin_addr.s_addr = INADDR_ANY;
    seeder_address.sin_port = 0;

    if (bind(seeder_socket, (struct sockaddr *)&seeder_address, sizeof(seeder_address)) == -1)
    {
        perror("bind failed");
        close(seeder_socket);
        return;
    }

    socklen_t len = sizeof(seeder_address);
    if (getsockname(seeder_socket, (struct sockaddr *)&seeder_address, &len) == -1)
    {
        perror("getsockname failed");
        close(seeder_socket);
        return;
    }
    int seeder_port = ntohs(seeder_address.sin_port);
    global_seeder_port = seeder_port;

    if (listen(seeder_socket, 5) == -1)
    {
        cerr << "listen failed\n";
        close(seeder_socket);
        return;
    }

    cout << "Seeder listening on port " << seeder_port << endl;

    while (true)
    {
        sockaddr_in peer_addr;
        socklen_t addr_size = sizeof(peer_addr);
        int peer_socket = accept(seeder_socket, (struct sockaddr *)&peer_addr, &addr_size);
        if (peer_socket < 0)
        {
            perror("accept failed");
            continue;
        }
        cout << "Seeder got a peer connection\n";
        thread(handle_peer_request, peer_socket).detach();
    }
    close(seeder_socket);
}
