# Protocol extension allocation registry

This document is the allocation contract for protocol extensions maintained
in this `moonlight-common-c` fork. It is the source of truth for identifiers
shared by the host and client projects. Keep the client flags, host flags, and
control opcodes in their own registries.

## Client feature flags

These values are sent by the client in `x-ml-general.featureFlags`.

### Upstream assignments

| Identifier | Value | Used for |
| --- | ---: | --- |
| `ML_FF_FEC_STATUS` | `0x01` | Tells the host that the client sends frame-loss FEC status feedback. |
| `ML_FF_SESSION_ID_V1` | `0x02` | Enables the session ID, ping payload, and connection-data extensions. |

### Frozen fork assignments

These values are already deployed in the fork ecosystem. They must not be
reused or renumbered.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `ML_FF_DS5_HAPTICS_PCM` | `0x04` | Tells the host that the client accepts authored DualSense PCM haptics. |
| `ML_FF_DS5_HAPTICS_IR_V2` | `0x08` | Tells the host that the client selected simulated DualSense and accepts IR v2. |
| `ML_FF_REMOTE_TEXT_CONTEXT` | `0x10` | Tells the host that the client accepts remote editable-text focus context updates. |
| `ML_FF_PYROWAVE` | `0x20` | Tells the host that the client accepts the experimental PyroWave video contract. |
| `ML_FF_DS5_HAPTICS_STATE` | `0x40` | Tells the host that the client accepts absolute DualSense actuator state updates on opcode `0x550D`. |
| `ML_FF_DS5_HAPTICS_IR_EXTENDED` | `0x80` | Tells the host that the client accepts continuous-state and source-event DualSense IR v2 updates on opcode `0x550B`. |
| `ML_FF_HOST_SESSION_STATE` | `0x100` | Tells the host that the client accepts host lock-state notifications on opcode `0x550E`. |

### New fork assignments

New fork-only client flags are allocated from `0x8000` downward. Reserve the
next unused high bit before implementation and add it to this table.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `ML_FF_ENDPOINT_AUTO_REFRESH` | `0x8000` | Tells the host that the client supports authenticated UDP endpoint migration v1. Activation also requires encrypted RTSP and `x-ml-general.endpointAutoRefreshVersion:1`. |
| `ML_FF_BITRATE_CALLBACK` | `0x4000` | Tells the host that the client accepts host-to-client runtime bitrate updates on opcode `0x5506` (subtype `2`, see below). It is set only for a host that advertises `LI_FF_BITRATE_CALLBACK`. Reserved: `common-c` does not offer it yet. |

## Host feature flags

These values are advertised by the host in `x-ss-general.featureFlags`.

### Upstream assignments

| Identifier | Value | Used for |
| --- | ---: | --- |
| `LI_FF_PEN_TOUCH_EVENTS` | `0x01` | Advertises pen and touch event injection support. |
| `LI_FF_CONTROLLER_TOUCH_EVENTS` | `0x02` | Advertises controller touch event injection support. |

### Frozen fork assignments

These values are already deployed in the fork ecosystem. They must not be
reused or renumbered.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `LI_FF_CLIPBOARD_TEXT` | `0x04` | Advertises clipboard text synchronization over opcode `0x5508`. The host sets it only while its GUI agent is alive. |
| `LI_FF_CLIPBOARD_IMAGE` | `0x08` | Advertises clipboard image synchronization over opcode `0x5508`. The host sets it only while its GUI agent is alive. |
| `LI_FF_TOUCHPAD_EVENTS` | `0x10` | Advertises native precision touchpad event support. |
| `LI_FF_TOUCHPAD_FRAME_EVENTS` | `0x20` | Advertises precision touchpad frame event support. |
| `LI_FF_CURSOR_SHAPE` | `0x40` | Advertises client-rendered cursor shape updates. |
| `LI_FF_DS5_HAPTICS_PCM` | `0x80` | Advertises authored DualSense stereo PCM haptics from the host. |
| `LI_FF_DYNAMIC_SDR_WHITE` | `0x100` | Advertises runtime client SDR reference-white updates. |
| `LI_FF_REMOTE_TEXT_CONTEXT` | `0x200` | Advertises host support for remote editable-text focus context updates. |
| `LI_FF_HOST_SESSION_STATE` | `0x800` | Advertises host lock-state notifications so the client can react to a locked desktop. |

### New fork assignments

New fork-only host flags are allocated from `0x8000` downward. Reserve the
next unused high bit before implementation and add it to this table.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `LI_FF_ENDPOINT_AUTO_REFRESH` | `0x8000` | Advertises authenticated UDP endpoint migration v1. The host enables it only after the client requests version 1 over encrypted RTSP. |
| `LI_FF_BITRATE_CALLBACK` | `0x4000` | Advertises host-to-client runtime bitrate updates on opcode `0x5506` (subtype `2`). The host sends them only to a client that set `ML_FF_BITRATE_CALLBACK`. Reserved: `0x400` is not used for this because the unmerged `LI_FF_CONTROLLER_HAPTICS` already holds it. |

## Assigned control opcodes

These opcodes are already assigned in the current `mic` protocol baseline.
The values are separate from both feature-flag namespaces.

| Opcode | Used for |
| --- | --- |
| `0x5500` | Rumble triggers. |
| `0x5501` | Controller motion events. |
| `0x5502` | RGB LED updates. |
| `0x5503` | Adaptive trigger updates. |
| `0x5504` | Microphone data. |
| `0x5505` | Microphone configuration. |
| `0x5506` | Dynamic parameter changes. |
| `0x5507` | Stream resolution change notifications. |
| `0x5508` | Opaque clipboard synchronization payloads. |
| `0x5509` | Local cursor mode and cursor shape updates. |
| `0x550A` | Authored DualSense PCM haptics. |
| `0x550B` | Device-independent DualSense IR v2 haptics. |
| `0x550C` | Remote text context updates. |
| `0x550D` | Absolute DualSense actuator state updates. |
| `0x550E` | Host interactive session lock-state notifications. |
| `0x5510` | Client-to-host FEC aggregate summary (`SS_FEC_SUMMARY` in `Video.h`: three big-endian 32-bit words, sequence, data packets and missing data packets). |

`0x550F` and `0x5511`–`0x5514` are not assigned. Do not use them without adding
a row here that documents the payload.

### Reserved opcodes: DualSense control extensions v1

These two opcodes are reserved here ahead of the runtime code so no other
extension takes them. They are not in the current `mic` baseline.

| Opcode | Direction | Body | Used for |
| --- | --- | --- | --- |
| `0x5515` | Host to client | 16 bytes, little endian | Output extension: player LEDs, mute LED, speaker, headphone and microphone volume, microphone mute, audio routing, lightbar brightness. |
| `0x5516` | Client to host | 12 bytes, little endian | Input status: headset present, microphone present, microphone muted. |

Both bodies start with `version:u8 = 1`, `reserved:u8 = 0`,
`controllerNumber:u16`, `validMask:u32`. The remaining bytes are the fields.
A field whose bit is not set in `validMask` must be zero. A receiver rejects the
whole body for a wrong length, a version other than `1`, a non-zero reserved
byte, an unknown `validMask` bit, or a field out of range.

`validMask` bits of `0x5515`:

| Bit | Field |
| ---: | --- |
| `0x01` | Player LED mask (`0` to `0x1f`). |
| `0x02` | Mute LED (`0` or `1`). |
| `0x04` | Speaker volume. |
| `0x08` | Headphone volume. |
| `0x10` | Microphone volume. |
| `0x20` | Microphone mute (`0` or `1`). |
| `0x40` | Audio routing. |
| `0x80` | Lightbar brightness. |

`validMask` bits of `0x5516`: `0x01` headset, `0x02` microphone, `0x04`
microphone muted. Each value is `0` or `1`; the last body byte is reserved and
zero.

A peer sends only the direction the other side declared in the SDP namespace
below. They do not use a feature-flag bit.

## Dynamic parameter subtypes (opcode `0x5506`)

The first little-endian 32-bit word of a `0x5506` payload is the subtype. The
values are separate from the feature-flag namespaces. Subtype ordinals are part
of the wire protocol; never reuse or renumber one. They follow
`dynamic_param_type_e` in the Foundation Sunshine host (`src/video.h`), which
applies and range-checks them in `src/stream.cpp`. Every payload word is a
little-endian 32-bit value; the ranges below are the ones the host enforces.

| Subtype | Name | Payload after the subtype word | Direction |
| ---: | --- | --- | --- |
| `0` | Resolution | `width:u32`, `height:u32` | Client to host request (`LiRequestResolutionChange`); the host answers with `0x5507`. |
| `1` | Frame rate | `fps:f32`, greater than 0 and at most 1000 | Client to host request. |
| `2` | Bitrate | `kbps:i32`, 1 to 800000 | Client to host request. Also host to client (8-byte payload: subtype and value) once `LI_FF_BITRATE_CALLBACK` and `ML_FF_BITRATE_CALLBACK` are both set. |
| `3` | QP | `i32`, 0 to 51 | Client to host request. |
| `4` | FEC percentage | `i32`, 0 to 100 | Client to host request. |
| `5` | Encoder preset | `i32` | Client to host request. |
| `6` | Adaptive quantization | `i32`, non-zero enables | Client to host request. |
| `7` | Multi-pass | `i32`, 0 to 2 | Client to host request. |
| `8` | VBV buffer size | `i32` kbps, greater than 0 | Client to host request. |
| `9` | Reserved legacy | none | Retired client SDR-white subtype. Hosts ignore it. Never reuse. |

## SDP namespaces

These attributes carry a comma separated list of `name=version` tokens. Only
version `1` is defined. A receiver ignores an unknown name, a version other
than `1`, and a malformed token.

| Attribute | Sender | Tokens |
| --- | --- | --- |
| `x-ml-general.ds5Extensions` | Client offer | `output=1`: the client accepts `0x5515`. `input-status=1`: the client accepts `0x5516`. |
| `x-ss-general.ds5Extensions` | Host answer | The same two tokens, for the directions the host accepts. |

`pad-audio=1` and `pad-speaker=1` are reserved names in the same attribute for
a four-channel DualSense PCM packet on opcode `0x550A`. Their payloads are not
defined here yet, and no host answers them today.

## Reserved private opcodes

The range `0x55F0`–`0x55FF` is reserved for future fork-private control or
protocol opcodes. Reserve one value per opcode and document its payload,
direction, version, and lifecycle in the common-c headers and this registry.
These values must not be used for feature flags.

## Compatibility contract

Feature bits are hints and must not be the only activation condition for a new
protocol. New features must carry an explicit version or capability attribute,
validate the complete wire payload, and reject unsupported versions without
breaking the base stream.

Unknown optional flags and opcodes must be ignored safely. Once an identifier
is published, its meaning and wire format are immutable; incompatible changes
require a new identifier or protocol version.
