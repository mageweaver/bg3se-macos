/**
 * BG3SE-macOS - Peer Manager Implementation
 *
 * Tracks connected peers with rate limiting and protocol version
 * negotiation support.
 *
 * Issue #6: NetChannel API (Phase 4A)
 */

#include "peer_manager.h"
#include "../core/logging.h"
#include <string.h>
#include <time.h>

// ============================================================================
// Static State
// ============================================================================

static PeerInfo s_peers[MAX_PEERS];
static bool s_initialized = false;

// ============================================================================
// Time Utilities
// ============================================================================

static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ============================================================================
// Internal Helpers
// ============================================================================

static int find_slot_by_user_id(int32_t user_id) {
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].active && s_peers[i].user_id == user_id) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!s_peers[i].active) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Public API
// ============================================================================

void peer_manager_init(void) {
    if (s_initialized) return;

    memset(s_peers, 0, sizeof(s_peers));
    s_initialized = true;

    // Register local host peer for single-player
    peer_manager_add_peer(1, NULL, true);

    LOG_NET_DEBUG("Peer manager initialized (local host peer registered)");
}

int peer_manager_add_peer(int32_t user_id, const char *guid, bool is_host) {
    if (!s_initialized && user_id != 1) {
        // Allow the init call for user_id=1 without recursion
        peer_manager_init();
    }

    // Check for duplicate
    int existing = find_slot_by_user_id(user_id);
    if (existing >= 0) {
        LOG_NET_WARN("Peer already exists: user_id=%d, updating", user_id);
        if (guid) {
            strncpy(s_peers[existing].character_guid, guid,
                    sizeof(s_peers[existing].character_guid) - 1);
            s_peers[existing].character_guid[
                sizeof(s_peers[existing].character_guid) - 1] = '\0';
        }
        s_peers[existing].is_host = is_host;
        return existing;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        LOG_NET_ERROR("Peer manager full (%d peers)", MAX_PEERS);
        return -1;
    }

    memset(&s_peers[slot], 0, sizeof(PeerInfo));
    s_peers[slot].user_id = user_id;
    s_peers[slot].is_host = is_host;
    s_peers[slot].active = true;
    s_peers[slot].proto_version = 0;  // Not yet negotiated
    s_peers[slot].rate_limit = PEER_RATE_LIMIT_DEFAULT;
    s_peers[slot].rate_window_start = get_current_time_ms();

    if (guid) {
        strncpy(s_peers[slot].character_guid, guid,
                sizeof(s_peers[slot].character_guid) - 1);
        s_peers[slot].character_guid[
            sizeof(s_peers[slot].character_guid) - 1] = '\0';
    }

    LOG_NET_DEBUG("Added peer: user_id=%d, slot=%d, is_host=%d",
                  user_id, slot, is_host);

    return slot;
}

bool peer_manager_remove_peer(int32_t user_id) {
    int slot = find_slot_by_user_id(user_id);
    if (slot < 0) return false;

    LOG_NET_DEBUG("Removed peer: user_id=%d, slot=%d", user_id, slot);

    memset(&s_peers[slot], 0, sizeof(PeerInfo));
    return true;
}

PeerInfo *peer_manager_get_peer(int32_t user_id) {
    if (!s_initialized) {
        peer_manager_init();
    }

    int slot = find_slot_by_user_id(user_id);
    if (slot < 0) return NULL;
    return &s_peers[slot];
}

PeerInfo *peer_manager_get_host(void) {
    if (!s_initialized) {
        peer_manager_init();
    }

    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].active && s_peers[i].is_host) {
            return &s_peers[i];
        }
    }
    return NULL;
}

int peer_manager_get_peer_count(void) {
    if (!s_initialized) {
        peer_manager_init();
    }

    int count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].active) count++;
    }
    return count;
}

bool peer_manager_set_proto_version(int32_t user_id, uint32_t proto_version) {
    if (!s_initialized) {
        peer_manager_init();
    }

    int slot = find_slot_by_user_id(user_id);
    if (slot < 0) return false;

    s_peers[slot].proto_version = proto_version;
    LOG_NET_DEBUG("Set proto_version=%u for user_id=%d",
                  proto_version, user_id);
    return true;
}

bool peer_manager_check_rate_limit(int32_t user_id) {
    if (!s_initialized) {
        peer_manager_init();
    }

    int slot = find_slot_by_user_id(user_id);
    if (slot < 0) return false;

    PeerInfo *peer = &s_peers[slot];

    // No rate limit configured
    if (peer->rate_limit == 0) return true;

    uint64_t now = get_current_time_ms();
    uint64_t elapsed = now - peer->rate_window_start;

    // New window
    if (elapsed >= PEER_RATE_WINDOW_MS) {
        peer->rate_window_start = now;
        peer->rate_message_count = 1;
        return true;
    }

    // Within window - check count
    if (peer->rate_message_count >= peer->rate_limit) {
        LOG_NET_WARN("Rate limit exceeded for user_id=%d (%u msgs in %llums)",
                     user_id, peer->rate_message_count,
                     (unsigned long long)elapsed);
        return false;
    }

    peer->rate_message_count++;
    return true;
}

bool peer_manager_set_rate_limit(int32_t user_id, uint32_t max_per_sec) {
    if (!s_initialized) {
        peer_manager_init();
    }

    int slot = find_slot_by_user_id(user_id);
    if (slot < 0) return false;

    s_peers[slot].rate_limit = max_per_sec;
    LOG_NET_DEBUG("Set rate_limit=%u for user_id=%d", max_per_sec, user_id);
    return true;
}

void peer_manager_clear(void) {
    memset(s_peers, 0, sizeof(s_peers));
    s_initialized = false;
    LOG_NET_DEBUG("Peer manager cleared");
}

// ============================================================================
// Iteration and Lookup (Phase 4H)
// ============================================================================

int peer_manager_iterate(PeerIterator fn, void *user_data) {
    if (!fn) return 0;
    if (!s_initialized) {
        peer_manager_init();
    }

    int visited = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].active) {
            visited++;
            if (!fn(&s_peers[i], user_data)) {
                break;
            }
        }
    }
    return visited;
}

int32_t peer_manager_find_by_guid(const char *guid) {
    if (!guid || !guid[0]) return -1;
    if (!s_initialized) {
        peer_manager_init();
    }

    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].active && s_peers[i].character_guid[0] &&
            strcmp(s_peers[i].character_guid, guid) == 0) {
            return s_peers[i].user_id;
        }
    }
    return -1;
}

// ============================================================================
// Handshake Gating (Phase 4I)
// ============================================================================

bool peer_manager_can_send_extender(int32_t user_id) {
    if (!s_initialized) {
        peer_manager_init();
    }

    int slot = find_slot_by_user_id(user_id);
    if (slot < 0) return false;

    return s_peers[slot].proto_version > 0;
}

/*
 * Enumerate active peers.
 *
 * Needed by entity:Replicate: the engine's ReplicateToPeer takes a peer id, so
 * replicating "to everyone" requires knowing who is connected. Copies out to
 * avoid handing callers a pointer into the live table.
 *
 * Returns the number written.
 */
int peer_manager_list_peers(PeerInfo *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    int n = 0;
    for (int i = 0; i < MAX_PEERS && n < max_out; i++) {
        if (s_peers[i].active) {
            out[n++] = s_peers[i];
        }
    }
    return n;
}
