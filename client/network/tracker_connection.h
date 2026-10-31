#ifndef TRACKER_CONNECTION_H
#define TRACKER_CONNECTION_H

#include <string>
#include <vector>

// Connects to the first tracker that is up (T1 first, then T2).
// Returns false if no tracker could be reached.
bool init_tracker_connection(const std::vector<std::string> &tracker_addresses);

// Sends one command to the tracker and waits for its full reply.
// Thread-safe: the main loop and download threads share the same session.
// If the current tracker is down, switches to the other one, logs in again
// with the last successful login, and resends the command once.
bool tracker_request(const std::string &message, std::string &reply);

void close_tracker_connection();

#endif // TRACKER_CONNECTION_H
