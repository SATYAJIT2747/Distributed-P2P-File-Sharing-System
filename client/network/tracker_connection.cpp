#include "network/tracker_connection.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <mutex>

using namespace std;

static vector<string> trackers;
static int current_tracker_index = -1;
static int tracker_socket = -1;
static mutex tracker_mutex;

// Remembered so we can log in again after switching to the other tracker
static string last_login_message = "";

static int connect_to_address(const string &address)
{
    string ip = address.substr(0, address.find(":"));
    int port = stoi(address.substr(address.find(":") + 1));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    sockaddr_in server_address;
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &server_address.sin_addr);

    if (connect(sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}

// Tries every tracker once, starting at start_index. Caller holds tracker_mutex.
static bool connect_to_any_tracker(int start_index)
{
    for (size_t i = 0; i < trackers.size(); i++)
    {
        int index = (start_index + i) % trackers.size();
        int sock = connect_to_address(trackers[index]);
        if (sock >= 0)
        {
            tracker_socket = sock;
            current_tracker_index = index;
            cout << "connected to tracker " << index + 1 << " (" << trackers[index] << ")\n";
            return true;
        }
    }
    return false;
}

static bool send_all(int sock, const string &message)
{
    // the tracker uses '\0' to know where a message ends
    const char *data = message.c_str();
    size_t total = message.length() + 1;
    size_t sent = 0;
    while (sent < total)
    {
        ssize_t n = send(sock, data + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        sent += n;
    }
    return true;
}

// TCP is a stream, so keep reading until the '\0' that ends the reply
static bool receive_reply(int sock, string &reply)
{
    reply.clear();
    char buffer[4096];
    while (true)
    {
        ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0)
            return false;
        reply.append(buffer, n);
        if (reply.back() == '\0')
            break;
    }
    reply.resize(strlen(reply.c_str()));
    return true;
}

static bool send_and_receive(const string &message, string &reply)
{
    if (tracker_socket < 0)
        return false;
    return send_all(tracker_socket, message) && receive_reply(tracker_socket, reply);
}

static bool fail_over()
{
    close(tracker_socket);
    tracker_socket = -1;

    cout << "lost connection to tracker " << current_tracker_index + 1 << ", trying the other tracker...\n";
    if (!connect_to_any_tracker(current_tracker_index + 1))
        return false;

    if (!last_login_message.empty())
    {
        string login_reply;
        if (send_and_receive(last_login_message, login_reply) &&
            login_reply.find("login successful") != string::npos)
            cout << "logged in again on the new tracker\n";
        else
            cout << "warning: could not log in again on the new tracker, please login manually\n";
    }
    return true;
}

bool init_tracker_connection(const vector<string> &tracker_addresses)
{
    lock_guard<mutex> lock(tracker_mutex);
    trackers = tracker_addresses;
    return connect_to_any_tracker(0);
}

bool tracker_request(const string &message, string &reply)
{
    lock_guard<mutex> lock(tracker_mutex);

    if (!send_and_receive(message, reply))
    {
        if (!fail_over() || !send_and_receive(message, reply))
        {
            cerr << "error: no tracker is reachable right now\n";
            return false;
        }
    }

    if (message.rfind("login ", 0) == 0 && reply.find("login successful") != string::npos)
        last_login_message = message;
    else if (message == "logout" && reply.find("you have logged out") != string::npos)
        last_login_message = "";

    return true;
}

void close_tracker_connection()
{
    lock_guard<mutex> lock(tracker_mutex);
    if (tracker_socket >= 0)
        close(tracker_socket);
    tracker_socket = -1;
}
