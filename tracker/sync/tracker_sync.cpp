#include "sync/tracker_sync.h"
#include "state/tracker_state.h"
#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

using namespace std;

void log_update_to_file(const string &message)
{
    ofstream log_file("updates.txt", ios::app);
    if (log_file.is_open())
    {
        log_file << message << endl;
        log_file.close();
    }
}

void recover_from_log()
{
    ifstream log_file("updates.txt");
    if (!log_file.is_open())
        return;

    cout << "Recovering state from log file..." << endl;
    string line;
    while (getline(log_file, line))
    {
        synchronize_with_peer(line);
        usleep(100000);
    }
    log_file.close();

    ofstream clear_file("updates.txt", ios::trunc);
    clear_file.close();
    cout << "recovery complete. log text file is  cleared." << endl;
}

void heartbeat_thread()
{
    bool was_main_tracker_up = true;

    while (true)
    {
        if (my_tracker_number == 2)
        {
            int main_socket;
            struct sockaddr_in main_address;

            main_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (main_socket < 0)
            {
                cerr << "unable to create socket for heartbeat." << endl;
                sleep(5);
                continue;
            }

            string main_ip = main_tracker_ip_port.substr(0, main_tracker_ip_port.find(":"));
            int main_port = stoi(main_tracker_ip_port.substr(main_tracker_ip_port.find(":") + 1));

            bzero(&main_address, sizeof(main_address));
            main_address.sin_family = AF_INET;
            main_address.sin_port = htons(main_port);
            inet_pton(AF_INET, &main_ip[0], &main_address.sin_addr);

            if (connect(main_socket, (struct sockaddr *)&main_address, sizeof(main_address)) < 0)
            {
                is_main_tracker_up = false;
                if (was_main_tracker_up)
                {
                    cout << "Main tracker is down. T2 will take over..." << endl;
                }
            }
            else
            {
                is_main_tracker_up = true;
                if (!was_main_tracker_up)
                {
                    cout << "Main tracker is back online. Initiating recovery..." << endl;
                    recover_from_log();
                }
                close(main_socket);
            }

            was_main_tracker_up = is_main_tracker_up;
        }

        sleep(5);
    }
}

void synchronize_with_peer(const string &message)
{
    if (!is_main_tracker_up && my_tracker_number == 2)
    {
        log_update_to_file(message);
        cout << "Main tracker is down. Logging update: " << message << endl;
        return;
    }

    int peer_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_socket < 0)
    {
        cerr << "Error creating sync socket" << endl;
        return;
    }

    string peer_ip = peer_ip_port.substr(0, peer_ip_port.find(":"));
    int peer_port = stoi(peer_ip_port.substr(peer_ip_port.find(":") + 1));

    struct sockaddr_in peer_address;
    bzero(&peer_address, sizeof(peer_address));
    peer_address.sin_family = AF_INET;
    peer_address.sin_port = htons(peer_port);
    inet_pton(AF_INET, &peer_ip[0], &peer_address.sin_addr);

    if (connect(peer_socket, (struct sockaddr *)&peer_address, sizeof(peer_address)) < 0)
    {
        cerr << "Could not connect to peer for sync, maybe offline" << endl;
        is_main_tracker_up = false;
        close(peer_socket);

        if (my_tracker_number == 2)
        {
            log_update_to_file(message);
            cout << "Sync failed. Logged update for later." << endl;
        }
        return;
    }

    cout << "Syncing with peer now... message: \"" << message << "\"" << endl;

    if (send(peer_socket, &message[0], message.length() + 1, MSG_NOSIGNAL) < 0)
    {
        perror("Send to peer failed");
        close(peer_socket);

        if (my_tracker_number == 2)
        {
            log_update_to_file(message);
            cout << "Send failed. Logged update for later." << endl;
        }
        return;
    }

    close(peer_socket);
}
