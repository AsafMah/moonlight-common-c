#include "Limelight-internal.h"

static int stage = STAGE_NONE;
static ConnListenerConnectionTerminated originalTerminationCallback;
static bool alreadyTerminated;
static PLT_THREAD terminationCallbackThread;
static int terminationCallbackErrorCode;
static bool hostRestartRecoveryEnabledForNextConnection;

// Common globals
char* RemoteAddrString;
struct sockaddr_storage RemoteAddr;
struct sockaddr_storage LocalAddr;
SOCKADDR_LEN AddrLen;
int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
ConnListenerRemoteTextContext RemoteTextContextCallback;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
int NegotiatedVideoFormat;
bool HostPyrowaveOffered;
LI_PYROWAVE_CAPABILITIES HostPyrowaveCapabilities;
atomic_bool ConnectionInterrupted;
bool HighQualitySurroundSupported;
bool HighQualitySurroundEnabled;
OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig;
OPUS_MULTISTREAM_CONFIGURATION HighQualityOpusConfig;
int AudioPacketDuration;
bool AudioEncryptionEnabled;
bool ReferenceFrameInvalidationSupported;
int NegotiatedAudioCodec;
LI_MAT_LIVE_PROFILE MatLiveProfile;
int NegotiatedAudioBitrate;
int NegotiatedDynamicHdrFormat;
int NegotiatedDynamicHdrFallback;
static atomic_bool connectionAttemptPrepared;
static atomic_bool connectionAttemptActive;
static atomic_bool preparedConnectionHasDeadline;
static atomic_bool activeConnectionHasDeadline;
static atomic_uint_fast64_t preparedConnectionDeadlineMs;
static atomic_uint_fast64_t activeConnectionDeadlineMs;

#define CONNECTION_WAIT_SLICE_MS 50
uint16_t RtspPortNumber;
uint16_t ControlPortNumber;
uint16_t AudioPortNumber;
uint16_t VideoPortNumber;
uint16_t MicPortNumber;
SS_PING AudioPingPayload;
SS_PING VideoPingPayload;
SS_PING MicPingPayload;
uint32_t ControlConnectData;
uint32_t SunshineFeatureFlags;
bool HostRestartRecoveryEnabled;
bool HostRestartHostSupported;
bool HostRestartNegotiated;
uint32_t EncryptionFeaturesSupported;
uint32_t EncryptionFeaturesRequested;
uint32_t EncryptionFeaturesEnabled;

// Connection stages
static const char* stageNames[STAGE_MAX] = {
    "none",
    "platform initialization",
    "name resolution",
    "audio stream initialization",
    "RTSP handshake",
    "control stream initialization",
    "video stream initialization",
    "input stream initialization",
    "control stream establishment",
    "video stream establishment",
    "audio stream establishment",
    "input stream establishment",
    "microphone stream initialization"
};

// Get the name of the current stage based on its number
const char* LiGetStageName(int stage) {
    return stageNames[stage];
}

static void beginConnectionAttempt(void) {
    bool prepared = atomic_exchange_explicit(&connectionAttemptPrepared, false, memory_order_acq_rel);

    if (prepared) {
        bool hasDeadline = atomic_load_explicit(&preparedConnectionHasDeadline, memory_order_acquire);
        uint64_t deadlineMs = atomic_load_explicit(&preparedConnectionDeadlineMs, memory_order_acquire);

        atomic_store_explicit(&activeConnectionDeadlineMs, deadlineMs, memory_order_relaxed);
        atomic_store_explicit(&activeConnectionHasDeadline, hasDeadline, memory_order_release);
    }
    else {
        // Preserve the historical behavior for callers that don't opt into
        // explicit per-attempt cancellation and deadline preparation.
        atomic_store_explicit(&ConnectionInterrupted, false, memory_order_release);
        atomic_store_explicit(&activeConnectionHasDeadline, false, memory_order_release);
    }

    atomic_store_explicit(&connectionAttemptActive, true, memory_order_release);
}

static void endConnectionAttempt(void) {
    atomic_store_explicit(&connectionAttemptActive, false, memory_order_release);
    atomic_store_explicit(&activeConnectionHasDeadline, false, memory_order_release);
}

uint32_t liGetConnectionAttemptRemainingTimeMs(void) {
    bool hasDeadline;
    uint64_t deadlineMs;
    uint64_t nowMs;
    uint64_t remainingMs;

    if (atomic_load_explicit(&connectionAttemptActive, memory_order_acquire)) {
        hasDeadline = atomic_load_explicit(&activeConnectionHasDeadline, memory_order_acquire);
        deadlineMs = atomic_load_explicit(&activeConnectionDeadlineMs, memory_order_relaxed);
    }
    else if (atomic_load_explicit(&connectionAttemptPrepared, memory_order_acquire)) {
        hasDeadline = atomic_load_explicit(&preparedConnectionHasDeadline, memory_order_acquire);
        deadlineMs = atomic_load_explicit(&preparedConnectionDeadlineMs, memory_order_relaxed);
    }
    else {
        return UINT32_MAX;
    }

    if (!hasDeadline) {
        return UINT32_MAX;
    }

    nowMs = PltGetMillis();
    if (deadlineMs <= nowMs) {
        return 0;
    }

    remainingMs = deadlineMs - nowMs;
    return remainingMs >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)remainingMs;
}

int liGetConnectionAttemptWaitError(void) {
    if (!atomic_load_explicit(&connectionAttemptActive, memory_order_acquire) &&
            !atomic_load_explicit(&connectionAttemptPrepared, memory_order_acquire)) {
        return 0;
    }

    if (atomic_load_explicit(&ConnectionInterrupted, memory_order_acquire)) {
        return EINTR;
    }

    if (liGetConnectionAttemptRemainingTimeMs() == 0) {
        return ETIMEDOUT;
    }

    return 0;
}

bool liWaitForConnectionDelayMs(uint32_t delayMs) {
    uint64_t operationDeadlineMs = PltGetMillis() + delayMs;

    for (;;) {
        uint64_t nowMs = PltGetMillis();
        int waitError = liGetConnectionAttemptWaitError();

        if (waitError != 0) {
            SetLastSocketError(waitError);
            return false;
        }

        if (nowMs >= operationDeadlineMs) {
            return true;
        }

        uint64_t waitMs = operationDeadlineMs - nowMs;
        uint32_t attemptRemainingMs = liGetConnectionAttemptRemainingTimeMs();
        if (attemptRemainingMs != UINT32_MAX && waitMs > attemptRemainingMs) {
            waitMs = attemptRemainingMs;
        }
        if (waitMs > CONNECTION_WAIT_SLICE_MS) {
            waitMs = CONNECTION_WAIT_SLICE_MS;
        }

        PltSleepMs((int)waitMs);
    }
}

bool liConnectionAttemptHasControl(void) {
    return atomic_load_explicit(&connectionAttemptActive, memory_order_acquire) ||
           atomic_load_explicit(&connectionAttemptPrepared, memory_order_acquire);
}

// Interrupt a pending connection attempt. This interruption happens asynchronously
// so it is not safe to start another connection before LiStartConnection() returns.
void LiInterruptConnection(void) {
    // Signal anyone waiting on the global interrupted flag
    atomic_store_explicit(&ConnectionInterrupted, true, memory_order_release);
}

void LiPrepareConnectionAttempt(uint32_t remainingTimeoutMs) {
    if (stage != STAGE_NONE ||
            atomic_load_explicit(&connectionAttemptActive, memory_order_acquire)) {
        Limelog("Connection attempt preparation must happen before LiStartConnection()\n");
        return;
    }

    atomic_store_explicit(&ConnectionInterrupted, false, memory_order_release);
    atomic_store_explicit(&preparedConnectionHasDeadline, remainingTimeoutMs != UINT32_MAX, memory_order_relaxed);
    if (remainingTimeoutMs != UINT32_MAX) {
        atomic_store_explicit(&preparedConnectionDeadlineMs,
                              PltGetMillis() + remainingTimeoutMs,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&connectionAttemptPrepared, true, memory_order_release);
}

void LiSetHostRestartRecoveryEnabled(bool enabled) {
    if (stage != STAGE_NONE) {
        Limelog("Host restart recovery opt-in must be set before LiStartConnection()\n");
        return;
    }

    hostRestartRecoveryEnabledForNextConnection = enabled;
}

// Stop the connection by undoing the step at the current stage and those before it
void LiStopConnection(void) {
    // Disable termination callbacks now
    alreadyTerminated = true;

    // Drop the stream socket registry before any socket is closed so
    // consumers of LiGetStreamSockets() don't see dead descriptors
    LiClearStreamSockets();

    // Set the interrupted flag
    LiInterruptConnection();

    if (stage == STAGE_MICROPHONE_STREAM_INIT) {
        Limelog("Stopping microphone stream...");
        destroyMicrophoneStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_INPUT_STREAM_START) {
        Limelog("Stopping input stream...");
        stopInputStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_AUDIO_STREAM_START) {
        Limelog("Stopping audio stream...");
        stopAudioStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_VIDEO_STREAM_START) {
        Limelog("Stopping video stream...");
        stopVideoStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_CONTROL_STREAM_START) {
        Limelog("Stopping control stream...");
        stopControlStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_INPUT_STREAM_INIT) {
        Limelog("Cleaning up input stream...");
        destroyInputStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_VIDEO_STREAM_INIT) {
        Limelog("Cleaning up video stream...");
        destroyVideoStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_CONTROL_STREAM_INIT) {
        Limelog("Cleaning up control stream...");
        destroyControlStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_RTSP_HANDSHAKE) {
        // Nothing to do
        stage--;
    }
    if (stage == STAGE_AUDIO_STREAM_INIT) {
        Limelog("Cleaning up audio stream...");
        destroyAudioStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_NAME_RESOLUTION) {
        // Nothing to do
        stage--;
    }
    if (stage == STAGE_PLATFORM_INIT) {
        Limelog("Cleaning up platform...");
        cleanupPlatform();
        stage--;
        Limelog("done\n");
    }
    LC_ASSERT(stage == STAGE_NONE);
    
    if (RemoteAddrString != NULL) {
        free(RemoteAddrString);
        RemoteAddrString = NULL;
    }
}

static void terminationCallbackThreadFunc(void* context)
{
    // Invoke the client's termination callback
    originalTerminationCallback(terminationCallbackErrorCode);
}

// This shim callback runs the client's connectionTerminated() callback on a
// separate thread. This is neccessary because other internal threads directly
// invoke this callback. That can result in a deadlock if the client
// calls LiStopConnection() in the callback when the cleanup code
// attempts to join the thread that the termination callback (and LiStopConnection)
// is running on.
static void ClInternalConnectionTerminated(int errorCode)
{
    int err;

    // Avoid recursion and issuing multiple callbacks
    if (alreadyTerminated || ConnectionInterrupted) {
        return;
    }

    terminationCallbackErrorCode = errorCode;
    alreadyTerminated = true;

    // Invoke the termination callback on a separate thread
    err = PltCreateThread("AsyncTerm", terminationCallbackThreadFunc, NULL, &terminationCallbackThread);
    if (err != 0) {
        // Nothing we can safely do here, so we'll just assert on debug builds
        Limelog("Failed to create termination thread: %d\n", err);
        LC_ASSERT(err == 0);
    }

    // Detach the thread since we never wait on it
    PltDetachThread(&terminationCallbackThread);
}

static bool parseRtspPortNumberFromUrl(const char* rtspSessionUrl, uint16_t* port)
{
    // If the session URL is not present, we will just use the well known port
    if (rtspSessionUrl == NULL) {
        return false;
    }

    // Pick the last colon in the string to match the port number
    char* portNumberStart = strrchr(rtspSessionUrl, ':');
    if (portNumberStart == NULL) {
        return false;
    }

    // Skip the colon
    portNumberStart++;

    // Validate the port number
    long int rawPort = strtol(portNumberStart, NULL, 10);
    if (rawPort <= 0 || rawPort > 65535) {
        return false;
    }

    *port = (uint16_t)rawPort;
    return true;
}

// Starts the connection to the streaming machine
int LiStartConnection(PSERVER_INFORMATION serverInfo, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
    PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, void* renderContext, int drFlags,
    void* audioContext, int arFlags) {
    int err;

    HostRestartRecoveryEnabled = hostRestartRecoveryEnabledForNextConnection;
    hostRestartRecoveryEnabledForNextConnection = false;
    HostRestartHostSupported = false;
    HostRestartNegotiated = false;

    // Start from a clean stream socket registry in case a prior session
    // didn't go through LiStopConnection()
    LiClearStreamSockets();

    beginConnectionAttempt();
    err = liGetConnectionAttemptWaitError();
    if (err != 0) {
        SetLastSocketError(err);
        goto Cleanup;
    }

    if (drCallbacks != NULL && (drCallbacks->capabilities & CAPABILITY_PULL_RENDERER) && drCallbacks->submitDecodeUnit) {
        Limelog("CAPABILITY_PULL_RENDERER cannot be set with a submitDecodeUnit callback\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    if (drCallbacks != NULL && (drCallbacks->capabilities & CAPABILITY_PULL_RENDERER) && (drCallbacks->capabilities & CAPABILITY_DIRECT_SUBMIT)) {
        Limelog("CAPABILITY_PULL_RENDERER and CAPABILITY_DIRECT_SUBMIT cannot be set together\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    if (clCallbacks != NULL && clCallbacks->ds5HapticsPcm != NULL && clCallbacks->ds5HapticsIrV2 != NULL) {
        Limelog("Physical and simulated DualSense haptics modes are mutually exclusive\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    if (serverInfo->serverCodecModeSupport == 0) {
        Limelog("serverCodecModeSupport field in SERVER_INFORMATION must be set!\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    // Extract the appversion from the supplied string
    if (extractVersionQuadFromString(serverInfo->serverInfoAppVersion,
                                     AppVersionQuad) < 0) {
        Limelog("Invalid appversion string: %s\n", serverInfo->serverInfoAppVersion);
        err = -1;
        goto Cleanup;
    }

    // Replace missing callbacks with placeholders
    fixupMissingCallbacks(&drCallbacks, &arCallbacks, &clCallbacks);
    memcpy(&VideoCallbacks, drCallbacks, sizeof(VideoCallbacks));
    memcpy(&AudioCallbacks, arCallbacks, sizeof(AudioCallbacks));

#ifdef LC_DEBUG_RECORD_MODE
    // Install the pass-through recorder callbacks
    setRecorderCallbacks(&VideoCallbacks, &AudioCallbacks);
#endif

    // Hook the termination callback so we can avoid issuing a termination callback
    // after LiStopConnection() is called.
    //
    // Initialize ListenerCallbacks before anything that could call Limelog().
    originalTerminationCallback = clCallbacks->connectionTerminated;
    memcpy(&ListenerCallbacks, clCallbacks, sizeof(ListenerCallbacks));
    ListenerCallbacks.connectionTerminated = ClInternalConnectionTerminated;

    memset(&LocalAddr, 0, sizeof(LocalAddr));
    NegotiatedVideoFormat = 0;
    HostPyrowaveOffered = false;
    memset(&HostPyrowaveCapabilities, 0, sizeof(HostPyrowaveCapabilities));
    VideoFecControlSupported = false;
    memcpy(&StreamConfig, streamConfig, sizeof(StreamConfig));
    RemoteAddrString = strdup(serverInfo->address);
    if (StreamConfig.videoFecPercentage < -2 || StreamConfig.videoFecPercentage > 100) {
        err = -1;
        goto Cleanup;
    }

    // The values in RTSP SETUP will be used to populate these.
    VideoPortNumber = 0;
    ControlPortNumber = 0;
    AudioPortNumber = 0;

    // Parse RTSP port number from RTSP session URL
    if (!parseRtspPortNumberFromUrl(serverInfo->rtspSessionUrl, &RtspPortNumber)) {
        // Use the well known port if parsing fails
        RtspPortNumber = 48010;

        Limelog("RTSP port: %u (RTSP URL parsing failed)\n", RtspPortNumber);
    }
    else {
        Limelog("RTSP port: %u\n", RtspPortNumber);
    }

    alreadyTerminated = false;
    
    // Validate the audio configuration
    if (MAGIC_BYTE_FROM_AUDIO_CONFIG(StreamConfig.audioConfiguration) != 0xCA ||
            CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT) {
        Limelog("Invalid audio configuration specified\n");
        err = -1;
        goto Cleanup;
    }

    // Resolve the audio codec for this session.
    //
    // AC3 / E-AC3 are Sunshine-only extensions; on GFE we silently downgrade to
    // Opus to avoid producing a session with no audio. The renderer can call
    // LiGetNegotiatedAudioCodec() to check what was actually selected.
    NegotiatedAudioCodec = AUDIO_CODEC_OPUS;
    NegotiatedAudioBitrate = 0;
    memset(&MatLiveProfile,0,sizeof(MatLiveProfile));
    switch (StreamConfig.audioCodec) {
        case AUDIO_CODEC_OPUS:
            // Default; nothing to do.
            break;
        case AUDIO_CODEC_AC3:
        case AUDIO_CODEC_EAC3:
            if (!IS_SUNSHINE()) {
                Limelog("Requested audio codec %d is not supported by GFE; falling back to Opus\n",
                        StreamConfig.audioCodec);
            }
            else if (CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) < 2) {
                Limelog("AC3/E-AC3 requires multichannel audio; falling back to Opus\n");
            }
            else if (CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) > 6) {
                Limelog("AC3/E-AC3 supports max 5.1 (6 channels); falling back to Opus for %d-channel config\n",
                        CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration));
            }
            else {
                NegotiatedAudioCodec = StreamConfig.audioCodec;
                NegotiatedAudioBitrate = StreamConfig.audioBitrate > 0
                    ? StreamConfig.audioBitrate
                    : (StreamConfig.audioCodec == AUDIO_CODEC_AC3 ? 640000 : 384000);
            }
            break;
        case AUDIO_CODEC_MAT_AUTO:
            if (!IS_SUNSHINE() || StreamConfig.controlOnly) {
                Limelog("MAT preference unavailable for this session; selecting Opus\n");
                break;
            }
            NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE;
            break;
        case AUDIO_CODEC_MAT_LIVE:
            if (!IS_SUNSHINE() || StreamConfig.controlOnly) {
                Limelog("Live MAT requires a Sunshine audio session\n");
                err = -1; goto Cleanup;
            }
            NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE;
            break;
        case 4: // Retired finite diagnostic profiles; never silently reinterpret.
        case 5:
            Limelog("Finite MAT codecs are not supported; request live MAT or Opus\n");
            err = -1; goto Cleanup;
        case AUDIO_CODEC_PCM_S16:
            if (!IS_SUNSHINE()) {
                Limelog("PCM passthrough is only supported by Sunshine; falling back to Opus\n");
            }
            else if (CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) > 6) {
                Limelog("PCM passthrough capped at 5.1 (6 channels); falling back to Opus for %d-channel config\n",
                        CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration));
            }
            else {
                NegotiatedAudioCodec = AUDIO_CODEC_PCM_S16;
                // PCM bitrate is fully determined by sample rate / channels;
                // expose nominal value for the renderer (48000 Hz * ch * 16 bit).
                NegotiatedAudioBitrate = 48000 * CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) * 16;
            }
            break;
        default:
            Limelog("Unknown audio codec %d requested; falling back to Opus\n",
                    StreamConfig.audioCodec);
            break;
    }
    Limelog("Negotiated audio codec: %d (bitrate %d bps)\n",
            NegotiatedAudioCodec, NegotiatedAudioBitrate);

    // FEC only works in 16 byte chunks, so we must round down
    // the given packet size to the nearest multiple of 16.
    StreamConfig.packetSize -= StreamConfig.packetSize % 16;

    if (StreamConfig.packetSize == 0) {
        Limelog("Invalid packet size specified\n");
        err = -1;
        goto Cleanup;
    }

    // Height must not be odd or NVENC will fail to initialize
    if (StreamConfig.height & 0x1) {
        Limelog("Encoder height must not be odd. Rounding %d to %d\n",
                StreamConfig.height,
                StreamConfig.height & ~0x1);
        StreamConfig.height = StreamConfig.height & ~0x1;
    }

    // Dimensions over 4096 are only supported with HEVC on NVENC. Experimental
    // formats are not part of the legacy codec mask used by this check.
    const int legacyVideoFormats = StreamConfig.supportedVideoFormats & ~VIDEO_FORMAT_PYROWAVE;
    if (!(legacyVideoFormats & ~VIDEO_FORMAT_MASK_H264) &&
            (StreamConfig.width > 4096 || StreamConfig.height > 4096)) {
        Limelog("WARNING: Streaming at resolutions above 4K using H.264 will likely fail! Trying anyway!\n");
    }
    // Dimensions over 8192 aren't supported at all (even on Turing)
    else if (StreamConfig.width > 8192 || StreamConfig.height > 8192) {
        Limelog("WARNING: Streaming at resolutions above 8K will likely fail! Trying anyway!\n");
    }

    // Reference frame invalidation doesn't seem to work with resolutions much
    // higher than 1440p. I haven't figured out a pattern to indicate which
    // resolutions will work and which won't, but we can at least exclude
    // 4K from RFI to avoid significant persistent artifacts after frame loss.
    if (StreamConfig.width == 3840 && StreamConfig.height == 2160 &&
            (VideoCallbacks.capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC) &&
            !IS_SUNSHINE()) {
        Limelog("Disabling reference frame invalidation for 4K streaming with GFE\n");
        VideoCallbacks.capabilities &= ~CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
    }
    
    Limelog("Initializing platform...");
    ListenerCallbacks.stageStarting(STAGE_PLATFORM_INIT);
    err = initializePlatform();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_PLATFORM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_PLATFORM_INIT);
    ListenerCallbacks.stageComplete(STAGE_PLATFORM_INIT);
    Limelog("done\n");

    Limelog("Resolving host name...");
    ListenerCallbacks.stageStarting(STAGE_NAME_RESOLUTION);
    LC_ASSERT(RtspPortNumber != 0);
    if (RtspPortNumber != 48010) {
        // If we have an alternate RTSP port, use that as our test port. The host probably
        // isn't listening on 47989 or 47984 anyway, since they're using alternate ports.
        err = resolveHostName(serverInfo->address, AF_UNSPEC, RtspPortNumber, &RemoteAddr, &AddrLen);
        if (err != 0 && liGetConnectionAttemptWaitError() == 0) {
            // Sleep for a second and try again. It's possible that we've attempt to connect
            // before the host has gotten around to listening on the RTSP port. Give it some
            // time before retrying.
            if (liWaitForConnectionDelayMs(1000)) {
                err = resolveHostName(serverInfo->address, AF_UNSPEC, RtspPortNumber, &RemoteAddr, &AddrLen);
            }
            else {
                err = LastSocketError();
            }
        }
    }
    else {
        // We use TCP 47984 and 47989 first here because we know those should always be listening
        // on hosts using the standard ports.
        //
        // TCP 48010 is a last resort because:
        // a) it's not always listening and there's a race between listen() on the host and our connect()
        // b) it's not used at all by certain host versions which perform RTSP over ENet
        err = resolveHostName(serverInfo->address, AF_UNSPEC, 47984, &RemoteAddr, &AddrLen);
        if (err != 0 && liGetConnectionAttemptWaitError() == 0) {
            err = resolveHostName(serverInfo->address, AF_UNSPEC, 47989, &RemoteAddr, &AddrLen);
        }
        if (err != 0 && liGetConnectionAttemptWaitError() == 0) {
            err = resolveHostName(serverInfo->address, AF_UNSPEC, 48010, &RemoteAddr, &AddrLen);
        }
    }
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
        goto Cleanup;
    }

    err = liGetConnectionAttemptWaitError();
    if (err != 0) {
        SetLastSocketError(err);
        ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
        goto Cleanup;
    }

    // Resolve LocalAddr by RemoteAddr.
    {
        SOCKADDR_LEN localAddrLen;
        err = getLocalAddressByUdpConnect(&RemoteAddr, AddrLen, RtspPortNumber, &LocalAddr, &localAddrLen);
        if (err != 0) {
            Limelog("failed to resolve local addr: %d\n", err);
            ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
            goto Cleanup;
        }
        LC_ASSERT(localAddrLen == AddrLen);
    }

    err = liGetConnectionAttemptWaitError();
    if (err != 0) {
        SetLastSocketError(err);
        ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
        goto Cleanup;
    }

    bool isNat64 = false;
    if (StreamConfig.streamingRemotely == STREAM_CFG_AUTO) {
        isNat64 = isNat64SynthesizedAddress(&RemoteAddr);
        err = liGetConnectionAttemptWaitError();
        if (err != 0) {
            SetLastSocketError(err);
            ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
            goto Cleanup;
        }
    }

    stage++;
    LC_ASSERT(stage == STAGE_NAME_RESOLUTION);
    ListenerCallbacks.stageComplete(STAGE_NAME_RESOLUTION);
    Limelog("done\n");

    // If STREAM_CFG_AUTO was requested, determine the streamingRemotely value
    // now that we have resolved the target address and impose the video packet
    // size cap if required.
    if (StreamConfig.streamingRemotely == STREAM_CFG_AUTO) {
        // It's possible to have a NAT64 prefix on a ULA or other private range,
        // so we must exclude NAT64 addresses from our local address checks.
        if (!isNat64 && isPrivateNetworkAddress(&RemoteAddr)) {
            StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
        }
        else {
            StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;

            if (RemoteAddr.ss_family == AF_INET || isNat64) {
                // Cap packet size at 1024 for remote IPv4 streaming to avoid fragmentation.
                Limelog("Packet size capped at 1024 bytes for remote IPv4 streaming\n");
                StreamConfig.packetSize = 1024;
            }
            else {
                // IPv6 guarantees a minimum MTU of 1280 before fragmentation, so use a higher
                // packet size cap for remote IPv6 streaming (when not using NAT64 which isn't
                // end-to-end IPv6 traffic).
                Limelog("Packet size capped at 1184 bytes for remote IPv6 streaming\n");
                StreamConfig.packetSize = 1184;
            }
        }
    }

    // Skip audio stream initialization in control-only mode
    if (!StreamConfig.controlOnly) {
        Limelog("Initializing audio stream...");
        ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_INIT);
        err = initializeAudioStream();
        if (err != 0) {
            Limelog("failed: %d\n", err);
            ListenerCallbacks.stageFailed(STAGE_AUDIO_STREAM_INIT, err);
            goto Cleanup;
        }
        stage++;
        LC_ASSERT(stage == STAGE_AUDIO_STREAM_INIT);
        ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_INIT);
        Limelog("done\n");
    }
    else {
        Limelog("Control-only mode: skipping audio stream initialization\n");
        stage++;  // Still increment stage to maintain continuity
        LC_ASSERT(stage == STAGE_VIDEO_STREAM_INIT);
    }

    Limelog("Starting RTSP handshake...");
    ListenerCallbacks.stageStarting(STAGE_RTSP_HANDSHAKE);
    err = performRtspHandshake(serverInfo);
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_RTSP_HANDSHAKE, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_RTSP_HANDSHAKE);
    ListenerCallbacks.stageComplete(STAGE_RTSP_HANDSHAKE);
    Limelog("done\n");

    Limelog("Initializing control stream...");
    ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_INIT);
    err = initializeControlStream();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_CONTROL_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_INIT);
    Limelog("done\n");

    // Skip video stream initialization in control-only mode
    if (!StreamConfig.controlOnly) {
        Limelog("Initializing video stream...");
        ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_INIT);
        initializeVideoStream();
        stage++;
        LC_ASSERT(stage == STAGE_VIDEO_STREAM_INIT);
        ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_INIT);
        Limelog("done\n");
    }
    else {
        Limelog("Control-only mode: skipping video stream initialization\n");
        stage++;  // Still increment stage to maintain continuity
        LC_ASSERT(stage == STAGE_VIDEO_STREAM_INIT);
    }

    Limelog("Initializing input stream...");
    ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_INIT);
    initializeInputStream();
    stage++;
    LC_ASSERT(stage == STAGE_INPUT_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_INIT);
    Limelog("done\n");

    Limelog("Starting control stream...");
    ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_START);
    err = startControlStream();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_CONTROL_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_START);
    Limelog("done\n");

    // Skip video and audio stream start in control-only mode
    if (!StreamConfig.controlOnly) {
        Limelog("Starting video stream...");
        ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_START);
        err = startVideoStream(renderContext, drFlags);
        if (err != 0) {
            Limelog("Video stream start failed: %d\n", err);
            ListenerCallbacks.stageFailed(STAGE_VIDEO_STREAM_START, err);
            goto Cleanup;
        }
        stage++;
        LC_ASSERT(stage == STAGE_VIDEO_STREAM_START);
        ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_START);
        Limelog("done\n");
    }
    else {
        Limelog("Control-only mode: skipping video stream start\n");
        stage++;  // Still increment stage to maintain continuity
        LC_ASSERT(stage == STAGE_VIDEO_STREAM_START);
    }

    if (!StreamConfig.controlOnly) {
        Limelog("Starting audio stream...");
        ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_START);
        err = startAudioStream(audioContext, arFlags);
        if (err != 0) {
            Limelog("Audio stream start failed: %d\n", err);
            ListenerCallbacks.stageFailed(STAGE_AUDIO_STREAM_START, err);
            goto Cleanup;
        }
        stage++;
        LC_ASSERT(stage == STAGE_AUDIO_STREAM_START);
        ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_START);
        Limelog("done\n");
    }
    else {
        Limelog("Control-only mode: skipping audio stream start\n");
        stage++;  // Still increment stage to maintain continuity
        LC_ASSERT(stage == STAGE_AUDIO_STREAM_START);
    }

    Limelog("Starting input stream...");
    ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_START);
    err = startInputStream();
    if (err != 0) {
        Limelog("Input stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_INPUT_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_INPUT_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_START);
    Limelog("done\n");

    // Initialize microphone stream if enabled and port was negotiated
    // Skip in control-only mode since there's no audio stream
    if (!StreamConfig.controlOnly && StreamConfig.enableMic && MicPortNumber != 0) {
        Limelog("Initializing microphone stream...");
        ListenerCallbacks.stageStarting(STAGE_MICROPHONE_STREAM_INIT);
        err = initializeMicrophoneStream();
        if (err != 0) {
            Limelog("failed: %d (microphone will be unavailable)\n", err);
            // Don't fail the connection for mic initialization failure
            err = 0;  // Reset error so connection continues
        }
        stage++;
        LC_ASSERT(stage == STAGE_MICROPHONE_STREAM_INIT);
        ListenerCallbacks.stageComplete(STAGE_MICROPHONE_STREAM_INIT);
        Limelog("done\n");
    }
    else {
        // Skip for control-only mode or when microphone is disabled
        stage++;
        LC_ASSERT(stage == STAGE_MICROPHONE_STREAM_INIT);
    }

    // Wiggle the mouse a bit to wake the display up
    LiSendMouseMoveEvent(1, 1);
    PltSleepMs(10);
    LiSendMouseMoveEvent(-1, -1);
    PltSleepMs(10);

    ListenerCallbacks.connectionStarted();

Cleanup:
    if (err != 0) {
        // Undo any work we've done here before failing
        LiStopConnection();
    }
    endConnectionAttempt();
    return err;
}

const char* LiGetLaunchUrlQueryParameters(void) {
    // v0 = Video encryption and control stream encryption v2
    // v1 = RTSP encryption
    return "&corever=1";
}
