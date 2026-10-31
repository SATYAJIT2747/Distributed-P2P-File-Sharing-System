#include "sync/tracker_sync.h"
#include "state/tracker_state.h"
#include <iostream>
#include <fstream>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

using namespace std;

static deque<string> sync_queue;
static mutex sync_queue_mutex;
static condition_variable sync_queue_cv;

const int SYNC_MAX_ATTEMPTS = 3;

// Each tracker keeps its own log, so both can run from the same folder
static string log_file_name()
{
    return "updates_t" + to_string(my_tracker_number) + ".txt";
}

static int connect_to_peer_tracker()
{
    int peer_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_socket < 0)
        return -1;

    // don't wait forever on a tracker that is hung (on Linux this also limits connect)
    timeval timeout = {2, 0};
    setsockopt(peer_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(peer_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    string peer_ip = peer_ip_port.substr(0, peer_ip_port.find(":"));
    int peer_port = stoi(peer_ip_port.substr(peer_ip_port.find(":") + 1));

    struct sockaddr_in peer_address;
    bzero(&peer_address, sizeof(peer_address));
    peer_address.sin_family = AF_INET;
    peer_address.sin_port = htons(peer_port);
    inet_pton(AF_INET, &peer_ip[0], &peer_address.sin_addr);

    if (connect(peer_socket, (struct sockaddr *)&peer_address, sizeof(peer_address)) < 0)
    {
        close(peer_socket);
        return -1;
    }
    return peer_socket;
}

// Sends one update and waits for the other tracker to confirm it applied it
static bool send_with_ack(const string &message)
{
    int peer_socket = connect_to_peer_tracker();
    if (peer_socket < 0)
        return false;

    size_t total = message.length() + 1;
    size_t sent = 0;
    while (sent < total)
    {
        ssize_t n = send(peer_socket, message.c_str() + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            close(peer_socket);
            return false;
        }
        sent += n;
    }

    char ack[32];
    bzero(ack, sizeof(ack));
    ssize_t n = recv(peer_socket, ack, sizeof(ack) - 1, 0);
    close(peer_socket);

    return n > 0 && strncmp(ack, "SYNC_ACK", 8) == 0;
}

static bool deliver(const string &message)
{
    for (int attempt = 1; attempt <= SYNC_MAX_ATTEMPTS; attempt++)
    {
        if (send_with_ack(message))
        {
            cout << "Synced with peer tracker: \"" << message.substr(0, 80) << "\"" << endl;
            return true;
        }
        usleep(200000);
    }
    return false;
}

static void append_to_log(const deque<string> &messages)
{
    if (messages.empty())
        return;

    ofstream log_file(log_file_name(), ios::app);
    for (const string &message : messages)
    {
        log_file << message << endl;
        cout << "Peer tracker is down. Logging update: " << message.substr(0, 80) << endl;
    }
}

// Sends logged updates oldest first. Whatever could not be sent stays in the log.
static bool replay_log()
{
    vector<string> lines;
    {
        ifstream log_file(log_file_name());
        string line;
        while (getline(log_file, line))
            if (!line.empty())
                lines.push_back(line);
    }
    if (lines.empty())
        return true;

    cout << "Replaying " << lines.size() << " logged update(s) to peer tracker..." << endl;

    size_t delivered = 0;
    while (delivered < lines.size() && deliver(lines[delivered]))
        delivered++;

    ofstream log_file(log_file_name(), ios::trunc);
    for (size_t i = delivered; i < lines.size(); i++)
        log_file << lines[i] << endl;

    if (delivered == lines.size())
        cout << "Recovery complete. Log file cleared." << endl;
    return delivered == lines.size();
}

void synchronize_with_peer(const string &message)
{
    {
        lock_guard<mutex> lock(sync_queue_mutex);
        sync_queue.push_back(message);
    }
    sync_queue_cv.notify_one();
}

void sync_worker_thread()
{
    while (true)
    {
        deque<string> batch;
        {
            unique_lock<mutex> lock(sync_queue_mutex);
            sync_queue_cv.wait_for(lock, chrono::seconds(1), []
                                   { return !sync_queue.empty(); });
            batch.swap(sync_queue);
        }

        // older logged updates must reach the peer before newer ones
        bool delivered_all = is_peer_tracker_up && replay_log();
        while (delivered_all && !batch.empty())
        {
            if (!deliver(batch.front()))
                delivered_all = false;
            else
                batch.pop_front();
        }

        if (!delivered_all)
        {
            is_peer_tracker_up = false;
            append_to_log(batch);
        }
    }
}

void heartbeat_thread()
{
    bool was_peer_up = true;

    while (true)
    {
        int peer_socket = connect_to_peer_tracker();
        bool peer_up = peer_socket >= 0;
        if (peer_up)
            close(peer_socket);

        is_peer_tracker_up = peer_up;
        if (my_tracker_number == 2)
            is_main_tracker_up = peer_up;

        if (!peer_up && was_peer_up)
            cout << "Peer tracker " << peer_ip_port << " is down. Updates will be logged until it is back." << endl;
        else if (peer_up && !was_peer_up)
        {
            cout << "Peer tracker " << peer_ip_port << " is back online. Initiating recovery..." << endl;
            sync_queue_cv.notify_one();
        }

        was_peer_up = peer_up;
        sleep(5);
    }
}
