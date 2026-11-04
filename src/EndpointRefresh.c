#include "Limelight-internal.h"
#include "EndpointRefresh.h"
#include <stdatomic.h>

bool EndpointRefreshRequested;
static atomic_bool enabled;

bool LiIsEndpointAutoRefreshEnabled(void) {
    return atomic_load_explicit(&enabled, memory_order_acquire);
}

void LiSetEndpointAutoRefreshEnabled(bool value) {
    atomic_store_explicit(&enabled, value, memory_order_release);
}

bool LiErClientInitialize(LI_ENDPOINT_REFRESH* state, SOCKET socket, uint8_t stream,
                          const char session[16], uint16_t remotePort, int qos, int bufferSize) {
    memset(state, 0, sizeof(*state));
    if (!EndpointRefreshRequested) return true;
    if (PltCreateMutex(&state->mutex)) return false;
    state->active = socket;
    state->candidate = INVALID_SOCKET;
    state->lastProbeSocket = INVALID_SOCKET;
    state->activeResponseAt = state->candidateResponseAt = UINT64_MAX;
    state->stream = stream;
    state->qos = qos;
    state->bufferSize = bufferSize;
    memcpy(state->session, session, 16);
    if (!LiErDeriveKey((const unsigned char*)StreamConfig.remoteInputAesKey, state->session, state->key)) {
        PltDeleteMutex(&state->mutex);
        return false;
    }
    memcpy(&state->remote, &RemoteAddr, sizeof(state->remote));
#ifdef AF_INET6
    SET_PORT((struct sockaddr_in6*)&state->remote, remotePort);
#else
    SET_PORT((struct sockaddr_in*)&state->remote, remotePort);
#endif
    state->initialized = true;
    return true;
}

void LiErClientDestroy(LI_ENDPOINT_REFRESH* state) {
    // The existing ping and receive workers have both stopped before cleanup.
    if (!state->initialized) return;
    if (state->candidate != INVALID_SOCKET) closeSocket(state->candidate);
    PltDeleteMutex(&state->mutex);
    LiErWipe(state, sizeof(*state));
}

// Called with the ping-worker mutex held. Socket changes may probe immediately;
// repeated callers cannot turn normal keepalive into an unbounded send loop.
static void sendProbe(LI_ENDPOINT_REFRESH* state, SOCKET socket, uint64_t now) {
    unsigned char packet[LI_ER_PACKET_SIZE];
    if (socket == state->lastProbeSocket && now - state->lastProbeAt < 500) return;
    if (state->sequence != UINT64_MAX && LiErEncode(packet, LI_ER_PROBE, state->stream,
            state->session, state->key, ++state->sequence, NULL)) {
        state->lastProbeSocket = socket;
        state->lastProbeAt = now;
        sendto(socket, (char*)packet, sizeof(packet), 0, (struct sockaddr*)&state->remote, AddrLen);
    }
}

bool LiErClientPing(LI_ENDPOINT_REFRESH* state) {
    if (!state->initialized) return false;
    // Audio's ping worker starts during SETUP; wait for authenticated ANNOUNCE.
    if (!LiIsEndpointAutoRefreshEnabled()) return true;
    PltLockMutex(&state->mutex);
    SOCKET socket = state->candidate != INVALID_SOCKET ? state->candidate : state->active;
    if (!state->failed) sendProbe(state, socket, PltGetMillis());
    PltUnlockMutex(&state->mutex);
    return true;
}

static void handle(LI_ENDPOINT_REFRESH* state, SOCKET socket, const void* packet, size_t size, uint64_t now) {
    LI_ER_MESSAGE message;
    unsigned char response[LI_ER_PACKET_SIZE];
    if (!LiErDecode(packet, size, state->stream, state->session, state->key, &message) ||
        message.sequence > state->sequence || state->sequence - message.sequence > 8) return;
    if (message.type == LI_ER_CHALLENGE &&
        (message.sequence > state->ackSequence ||
         (socket == state->candidate && message.sequence >= state->candidateFirstSequence))) {
        uint64_t* responseAt = socket == state->candidate ? &state->candidateResponseAt : &state->activeResponseAt;
        if (*responseAt != UINT64_MAX && now - *responseAt < 250) return;
        if (socket == state->candidate) {
            state->challengeSequence = message.sequence;
            memcpy(state->nonce, message.nonce, sizeof(state->nonce));
            state->challenged = true;
        }
        if (LiErEncode(response, LI_ER_RESPONSE, state->stream, state->session, state->key,
                message.sequence, message.nonce)) {
            *responseAt = now;
            sendto(socket, (char*)response, sizeof(response), 0, (struct sockaddr*)&state->remote, AddrLen);
        }
    }
    else if (message.type == LI_ER_ACK &&
             (message.sequence > state->ackSequence || socket == state->candidate)) {
        if (socket == state->candidate && (!state->challenged ||
                memcmp(state->nonce, message.nonce, sizeof(state->nonce)) ||
                message.sequence < state->challengeSequence)) return;
        if (message.sequence > state->ackSequence) state->ackSequence = message.sequence;
        state->lastAck = now;
        if (socket == state->candidate) state->candidateConfirmed = true;
    }
}

void LiErClientMedia(LI_ENDPOINT_REFRESH* state) {
    // Only the receive worker reads/writes this field; never lock for RTP data.
    if (!state->initialized) return;
    state->lastMedia = state->receiveTime;
}

static bool maintain(LI_ENDPOINT_REFRESH* state, uint64_t now) {
    // Reuse the previous receive timestamp, after the caller has accepted or
    // rejected its RTP packet. No extra per-packet clock read or worker needed.
    if (now - state->lastPoll < 100) return true;
    state->lastPoll = now;
    PltLockMutex(&state->mutex);
    if (now - state->lastAck < 2000 && now - state->lastMedia < 2000) {
        if (!state->stableSince) state->stableSince = now;
        if (now - state->stableSince >= 30000) state->attempts = 0;
    }
    else state->stableSince = 0;
    if (state->candidate != INVALID_SOCKET) {
        if (now - state->candidateStarted >= 2000) {
            closeSocket(state->candidate);
            state->candidate = INVALID_SOCKET;
        }
    }
    if (state->candidate == INVALID_SOCKET && state->attempts < 3 &&
        now - state->lastAck >= 2000 && now - state->lastMedia >= 2000) {
        state->candidate = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, state->bufferSize, state->qos);
        state->attempts++;
        if (state->candidate != INVALID_SOCKET) {
            if (setNonFatalRecvTimeoutMs(state->candidate, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
                closeSocket(state->candidate);
                state->candidate = INVALID_SOCKET;
            }
            else {
                state->candidateStarted = now;
                state->challenged = false;
                state->candidateConfirmed = false;
                state->challengeSequence = 0;
                state->candidateFirstSequence = state->sequence + 1;
                state->candidateResponseAt = UINT64_MAX;
                state->candidateNext = true;
                sendProbe(state, state->candidate, now);
                Limelog("endpoint_auto_refresh: probing a replacement %s UDP socket\n", state->stream == LI_ER_VIDEO ? "video" : "audio");
            }
        }
    }
    {
        bool exhausted = state->candidate == INVALID_SOCKET && state->attempts >= 3 &&
            now - state->lastAck >= 8000 && now - state->lastMedia >= 8000;
        state->failed = exhausted;
        PltUnlockMutex(&state->mutex);
        return !exhausted;
    }
}

int LiErClientRecv(LI_ENDPOINT_REFRESH* state, SOCKET* socket, char* packet, int size, bool useSelect) {
    if (!state->initialized || !LiIsEndpointAutoRefreshEnabled()) {
        return recvUdpSocket(*socket, packet, size, useSelect);
    }
    if (state->failed) return LI_ER_RECV_FAILED;
    if (state->receiving && !maintain(state, state->receiveTime)) return LI_ER_RECV_FAILED;

    SOCKET selected = state->active;
    int length;
    if (state->candidate == INVALID_SOCKET) {
        // Preserve the existing single-socket receive fast path when healthy.
        length = recvUdpSocket(selected, packet, size, useSelect);
    }
    else {
        struct pollfd reads[2] = {
            { .fd = state->active, .events = POLLIN },
            { .fd = state->candidate, .events = POLLIN }
        };
        length = pollSockets(reads, 2, UDP_RECV_POLL_TIMEOUT_MS);
        if (length > 0) {
            bool oldReady = (reads[0].revents & (POLLIN | POLLERR | POLLNVAL)) != 0;
            bool newReady = (reads[1].revents & (POLLIN | POLLERR | POLLNVAL)) != 0;
            if (newReady && (!oldReady || state->candidateNext)) {
                selected = state->candidate;
                state->candidateNext = false;
            }
            else if (oldReady) {
                state->candidateNext = true;
            }
            else length = 0;
            if (length > 0) length = recvUdpSocket(selected, packet, size, selected == state->active && useSelect);
        }
    }

    // One clock read per receive, shared with ACK handling and RTP acceptance.
    state->receiveTime = PltGetMillis();
    if (!state->receiving) {
        // Audio SETUP can precede its receiver by seconds. Startup time must
        // not consume the recovery budget before this worker can receive ACKs.
        state->lastAck = state->lastMedia = state->lastPoll = state->receiveTime;
        state->receiving = true;
    }
    if (length <= 0) {
        if (length < 0 && selected == state->candidate) {
            PltLockMutex(&state->mutex);
            closeSocket(state->candidate);
            state->candidate = INVALID_SOCKET;
            PltUnlockMutex(&state->mutex);
            return LI_ER_RECV_HANDLED;
        }
        return length;
    }
    if (length == LI_ER_PACKET_SIZE && !memcmp(packet, "EAPR", 4)) {
        PltLockMutex(&state->mutex);
        handle(state, selected, packet, (size_t)length, state->receiveTime);
        if (selected == state->candidate && state->candidateConfirmed) {
            SOCKET previous = state->active;
            *socket = state->active = state->candidate;
            state->candidate = INVALID_SOCKET;
            state->stableSince = state->receiveTime;
            state->activeResponseAt = state->candidateResponseAt;
            LiReplaceStreamSocket(previous, *socket, state->stream == LI_ER_VIDEO ? STREAM_SOCKET_SLOT_VIDEO : STREAM_SOCKET_SLOT_AUDIO);
            closeSocket(previous);
            Limelog("endpoint_auto_refresh: migrated %s UDP socket\n", state->stream == LI_ER_VIDEO ? "video" : "audio");
        }
        PltUnlockMutex(&state->mutex);
        return LI_ER_RECV_HANDLED;
    }
    // Only authenticated candidates may feed the existing RTP/decryption path.
    return selected == state->candidate ? LI_ER_RECV_HANDLED : length;
}
