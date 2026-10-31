#include "state/tracker_state.h"
#include "sync/tracker_sync.h"
#include "utils/tracker_utils.h"
#include "handler/client_handler.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <signal.h>

using namespace std;

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc < 3)
    {
        cerr << "the command should look like : ./tracker <tracker_info_file> <tracker_number>" << endl;
        return -1;
    }

    vector<string> tracker_info;
    string info_filename = argv[1];
    read_tracker_info(info_filename, tracker_info);

    int tracker_number = stoi(argv[2]);
    my_tracker_number = tracker_number;

    if (tracker_number == 1)
    {
        my_ip_port = tracker_info[0];
        main_tracker_ip_port = tracker_info[0];
        peer_ip_port = tracker_info[1];
    }
    else if (tracker_number == 2)
    {
        my_ip_port = tracker_info[1];
        main_tracker_ip_port = tracker_info[0];
        peer_ip_port = tracker_info[0];
    }
    else
    {
        cerr << "err0r: not a valid tracker number. use eithe 1 or 2." << endl;
        return -1;
    }

    int my_port = stoi(my_ip_port.substr(my_ip_port.find(":") + 1));
    int main_tracker_port = stoi(main_tracker_ip_port.substr(main_tracker_ip_port.find(":") + 1));

    cout << "my address: " << my_ip_port << endl;
    cout << "peer address: " << peer_ip_port << endl;

    // both trackers watch each other, so either one can log updates while the other is down
    thread hb_thread(heartbeat_thread);
    hb_thread.detach();

    thread sync_thread(sync_worker_thread);
    sync_thread.detach();

    int tracker_socket;
    struct sockaddr_in tracker_address, client_address;

    ensure_success((tracker_socket = socket(AF_INET, SOCK_STREAM, 0)), "socket creation is failed!");

    // lets a crashed tracker restart on the same port right away instead of waiting for TIME_WAIT
    int reuse = 1;
    setsockopt(tracker_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    bzero(&tracker_address, sizeof(tracker_address));
    tracker_address.sin_family = AF_INET;
    tracker_address.sin_addr.s_addr = INADDR_ANY;

    int listening_port = my_port;
    if (tracker_number == 2 && !is_main_tracker_up)
    {
        listening_port = main_tracker_port;
    }
    tracker_address.sin_port = htons(listening_port);

    ensure_success((bind(tracker_socket, (struct sockaddr *)&tracker_address, sizeof(tracker_address))), "bind failed!");
    ensure_success((listen(tracker_socket, 10)), "listen failed!");

    cout << "tracker is listening on the port ..:" << listening_port << endl;

    while (true)
    {
        socklen_t client_address_length = sizeof(client_address);

        int client_socket = accept(tracker_socket, (struct sockaddr *)&client_address, &client_address_length);
        ensure_success(client_socket, "accept failed!");

        thread(client_handler, client_socket).detach();
    }

    close(tracker_socket);
    return 0;
}
