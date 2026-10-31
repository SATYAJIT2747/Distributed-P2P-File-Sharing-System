#ifndef TRACKER_SYNC_H
#define TRACKER_SYNC_H

#include <string>

// Queues an update for the other tracker and returns right away.
// Safe to call while holding global_data_lock: no network work happens here.
void synchronize_with_peer(const std::string &message);

// Checks every few seconds if the other tracker is up.
void heartbeat_thread();

// Sends queued updates in order and waits for an ACK for each one.
// While the other tracker is down, updates go to updates_t<N>.txt and are
// replayed (oldest first) as soon as it comes back.
void sync_worker_thread();

#endif // TRACKER_SYNC_H
