# Protocol Specification

## Overview

This document specifies the binary protocol used for communication between publishers, subscribers, and the broker. The protocol is designed for efficiency, using a compact binary format with fixed-size headers and variable-length payloads.�

## Protocol Characteristics

- **Transport**: TCP/UDP
- **Encoding**: Binary
- **Byte Order**: Little-endian (platform-dependent)
- **Maximum Payload**: 1 MB (1,048,576 bytes)
- **Header Size**: 7 bytes

## Frame Structure

Every message follows this structure:

```text
+----------------+----------------+----------------------+
|  Magic (2B)    |  OpCode (1B)   |  Length (4B)         |
+----------------+----------------+----------------------+
|  Payload (variable, 0 to MAX_PAYLOAD_SIZE bytes)       |
+--------------------------------------------------------+
```

### Header Fields

| Field    | Size    | Type     | Description                           |
|----------|---------|----------|---------------------------------------|
| Magic    | 2 bytes | uint16_t | Protocol identifier (0xCAFE)          |
| OpCode   | 1 byte  | uint8_t  | Operation code (see OpCode table)     |
| Length   | 4 bytes | uint32_t | Payload length in bytes (0-1048576)   |

### Validation Rules

- Magic number must be `0xCAFE`
- Payload length must not exceed `MAX_PAYLOAD_SIZE` (1 MB)
- Invalid frames should be rejected immediately

## Operation Codes (OpCodes)

### Connection Lifecycle (0x01 - 0x0F)

| OpCode | Value | Name           | Direction     | Description                    |
|--------|-------|----------------|---------------|--------------------------------|
| 0x01   | 1     | HANDSHAKE_PUB  | Client→Broker | Publisher handshake            |
| 0x02   | 2     | HANDSHAKE_SUB  | Client→Broker | Subscriber handshake           |
| 0x03   | 3     | HANDSHAKE_ACK  | Broker→Client | Handshake acknowledgment       |
| 0x04   | 4     | DISCONNECT     | Bidirectional | Graceful connection close      |

### Pub/Sub Operations (0x10 - 0x1F)

| OpCode | Value | Name         | Direction        | Description                        |
|--------|-------|--------------|------------------|------------------------------------|
| 0x10   | 16    | PUBLISH      | Publisher→Broker | Publish message to channel         |
| 0x11   | 17    | SUBSCRIBE    | Subscriber→Broker| Subscribe to a channel             |
| 0x12   | 18    | UNSUBSCRIBE  | Subscriber→Broker| Unsubscribe from a channel         |
| 0x13   | 19    | MESSAGE      | Broker→Subscriber| Deliver message to subscriber      |

### Control Messages (0x20 - 0x2F)

| OpCode | Value | Name | Direction     | Description                    |
|--------|-------|------|---------------|--------------------------------|
| 0x20   | 32    | PING | Bidirectional | Keep-alive ping                |
| 0x21   | 33    | PONG | Bidirectional | Keep-alive pong response       |

### Error Handling (0xFF)

| OpCode | Value | Name  | Direction     | Description                   |
|--------|-------|-------|---------------|-------------------------------|
| 0xFF   | 255   | ERROR | Broker→Client | Error notification            |

## Error Codes

| Code | Value | Name                        | Description                              |
|------|-------|-----------------------------|------------------------------------------|
| 0x01 | 1     | INVALID_HANDSHAKE           | Handshake message is malformed           |
| 0x02 | 2     | CHANNEL_NOT_FOUND           | Requested channel does not exist         |
| 0x03 | 3     | MESSAGE_TOO_LARGE           | Payload exceeds maximum size             |
| 0x04 | 4     | RATE_LIMIT_EXCEEDED         | Client exceeded rate limits              |
| 0x05 | 5     | PROTOCOL_VERSION_MISMATCH   | Incompatible protocol versions           |
| 0x06 | 6     | INVALID_OPCODE              | Unknown or invalid operation code        |
| 0x07 | 7     | MALFORMED_MESSAGE           | Message structure is invalid             |
| 0x08 | 8     | UNAUTHORIZED                | Client lacks required permissions        |

## Message Formats

### HANDSHAKE_PUB (0x01)

**Publisher → Broker**: Initial handshake from publisher

**Payload Structure**:

```text
+----------------+-------------------+----------------------------+
| Channel (1B)   | ClientID Len (1B) | ClientID (variable)        |
+----------------+-------------------+----------------------------+
```

| Field         | Size     | Type     | Description                           |
|---------------|----------|----------|---------------------------------------|
| Channel       | 1 byte   | uint8_t  | Channel ID to publish to              |
| ClientID Len  | 1 byte   | uint8_t  | Length of client ID string            |
| ClientID      | Variable | string   | Client identifier (up to 255 bytes)   |

**Total Size**: `7 + 2 + clientId.length` bytes

---

### HANDSHAKE_SUB (0x02)

**Subscriber → Broker**: Initial handshake from subscriber

**Payload Structure**:

```text
+-------------------+----------------------+-------------------+----------------------------+
| Channel Count (1B)| Channels (variable)  | ClientID Len (1B) | ClientID (variable)        |
+-------------------+----------------------+-------------------+----------------------------+
```

| Field          | Size     | Type       | Description                              |
|----------------|----------|------------|------------------------------------------|
| Channel Count  | 1 byte   | uint8_t    | Number of channels to subscribe to       |
| Channels       | Variable | uint8_t[]  | Array of channel IDs                     |
| ClientID Len   | 1 byte   | uint8_t    | Length of client ID string               |
| ClientID       | Variable | string     | Client identifier (up to 255 bytes)      |

**Total Size**: `7 + 1 + channelCount + 1 + clientId.length` bytes

---

### HANDSHAKE_ACK (0x03)

**Broker → Client**: Acknowledgment of successful handshake

**Payload Structure**:

```text
+----------------+-------------------------+
| Status (1B)    | SessionID (8B)          |
+----------------+-------------------------+
```

| Field      | Size    | Type     | Description                              |
|------------|---------|----------|------------------------------------------|
| Status     | 1 byte  | uint8_t  | 0 = success, non-zero = error code       |
| SessionID  | 8 bytes | uint64_t | Unique session identifier                |

**Total Size**: `7 + 9 = 16` bytes

---

### DISCONNECT (0x04)

**Bidirectional**: Graceful connection termination

**Payload Structure**: None (empty payload)

**Total Size**: `7` bytes

---

### PUBLISH (0x10)

**Publisher → Broker**: Publish a message to a channel

**Payload Structure**:

```text
+----------------+----------------------------+
| Channel (1B)   | Message (variable)         |
+----------------+----------------------------+
```

| Field    | Size     | Type   | Description                              |
|----------|----------|--------|------------------------------------------|
| Channel  | 1 byte   | uint8_t| Channel ID to publish to                 |
| Message  | Variable | bytes  | Message data (up to MAX_PAYLOAD_SIZE-1)  |

**Total Size**: `7 + 1 + message.length` bytes

---

### SUBSCRIBE (0x11)

**Subscriber → Broker**: Subscribe to a channel

**Payload Structure**:

```text
+----------------+
| Channel (1B)   |
+----------------+
```

| Field    | Size   | Type    | Description                    |
|----------|--------|---------|--------------------------------|
| Channel  | 1 byte | uint8_t | Channel ID to subscribe to     |

**Total Size**: `7 + 1 = 8` bytes

---

### UNSUBSCRIBE (0x12)

**Subscriber → Broker**: Unsubscribe from a channel

**Payload Structure**:

```text
+----------------+
| Channel (1B)   |
+----------------+
```

| Field    | Size   | Type    | Description                      |
|----------|--------|---------|----------------------------------|
| Channel  | 1 byte | uint8_t | Channel ID to unsubscribe from   |

**Total Size**: `7 + 1 = 8` bytes

---

### MESSAGE (0x13)

**Broker → Subscriber**: Deliver a message from a publisher

**Payload Structure**:

```text
+----------------+----------------------+----------------------------+
| Channel (1B)   | Timestamp (8B)       | Message (variable)         |
+----------------+----------------------+----------------------------+
```

| Field      | Size     | Type     | Description                              |
|------------|----------|----------|------------------------------------------|
| Channel    | 1 byte   | uint8_t  | Channel ID of the message                |
| Timestamp  | 8 bytes  | uint64_t | Message timestamp (broker-assigned)      |
| Message    | Variable | bytes    | Message data                             |

**Total Size**: `7 + 1 + 8 + message.length` bytes

---

### PING (0x20)

**Bidirectional**: Keep-alive request

**Payload Structure**: None (empty payload)

**Total Size**: `7` bytes

---

### PONG (0x21)

**Bidirectional**: Keep-alive response

**Payload Structure**: None (empty payload)

**Total Size**: `7` bytes

---

### ERROR (0xFF)

**Broker → Client**: Error notification

**Payload Structure**:

```text
+------------------+
| ErrorCode (1B)   |
+------------------+
```

| Field      | Size   | Type    | Description                        |
|------------|--------|---------|------------------------------------|
| ErrorCode  | 1 byte | uint8_t | Error code (see Error Codes table) |

**Total Size**: `7 + 1 = 8` bytes

## Connection Flow

### Publisher Connection

```text
Publisher                    Broker
   |                            |
   |------ HANDSHAKE_PUB ------>|
   |                            |
   |<----- HANDSHAKE_ACK -------|
   |                            |
   |------ PUBLISH ------------>|
   |------ PUBLISH ------------>|
   |         ...                |
   |------ DISCONNECT --------->|
   |                            |
```

### Subscriber Connection

```text
Subscriber                   Broker
   |                            |
   |------ HANDSHAKE_SUB ------>|
   |                            |
   |<----- HANDSHAKE_ACK -------|
   |                            |
   |------ SUBSCRIBE ---------->|
   |                            |
   |<------ MESSAGE ------------|
   |<------ MESSAGE ------------|
   |         ...                |
   |------ UNSUBSCRIBE -------->|
   |------ DISCONNECT --------->|
   |                            |
```

### Keep-Alive Flow

```text
Client                       Broker
   |                            |
   |------ PING --------------->|
   |                            |
   |<------ PONG ---------------|
   |                            |
```

## Encoding/Decoding

### MessageEncoder

The `MessageEncoder` class provides static methods to calculate message sizes and encode messages into byte buffers.

**Size Calculation Methods**:

- `sizeHandshakePub(clientId)` → `7 + 2 + clientId.length`
- `sizeHandshakeSub(channels, clientId)` → `7 + 1 + channels.length + 1 + clientId.length`
- `sizeHandshakeAck()` → `16`
- `sizePublish(message)` → `7 + 1 + message.length`
- `sizeMessage(message)` → `7 + 1 + 8 + message.length`
- `sizeSubscribe()` → `8`
- `sizeUnsubscribe()` → `8`
- `sizeDisconnect()` → `7`
- `sizePing()` → `7`
- `sizePong()` → `7`
- `sizeError()` → `8`

**Encoding Methods**:

- All `encode*()` methods write directly to a pre-allocated buffer
- Caller must ensure buffer is large enough (use size methods first)

### MessageDecoder

The `MessageDecoder` class handles incremental parsing of binary data.

**ParseResult Structure**:

- `needMoreData`: `true` if more data is required to complete the frame
- `bytesConsumed`: Number of bytes consumed from the input buffer
- `message`: Optional decoded message (present when complete frame received)

**Usage Pattern**:

```cpp
MessageDecoder decoder;
auto result = decoder.decode(dataSpan);

if (result.needMoreData) {
    // Wait for more data
} else if (result.message) {
    // Process complete message
    // Advance buffer by result.bytesConsumed
} else {
    // Invalid frame, close connection
}
```

## Implementation Notes

### Memory Management

- All encoding methods write to caller-provided buffers
- Decoders return pointers into the original data span (zero-copy)
- Payload pointers in `DecodedMessage` are **not owned** by the message

### Partial Frame Handling

The decoder handles partial frames gracefully:

1. If less than 7 bytes available → `needMoreData = true`
2. If header valid but payload incomplete → `needMoreData = true`
3. If complete frame available → returns message and bytes consumed
4. If invalid magic/size → returns error (no message, `needMoreData = false`)

### Channel Management

- Channels are identified by single byte values (0-255)
- Subscribers can subscribe to multiple channels in initial handshake
- Additional subscriptions/unsubscriptions use SUBSCRIBE/UNSUBSCRIBE opcodes

### Session Management

- Broker assigns unique 64-bit session IDs during handshake
- Session IDs can be used for reconnection logic (implementation-defined)

## Security Considerations

1. **Message Size Validation**: Always enforce MAX_PAYLOAD_SIZE (1 MB) limit
2. **Magic Number Verification**: Reject frames with incorrect magic number
3. **OpCode Validation**: Reject unknown opcodes with INVALID_OPCODE error
4. **Rate Limiting**: Implement RATE_LIMIT_EXCEEDED for abusive clients
5. **Client ID Validation**: Validate client ID lengths and formats
6. **Channel Validation**: Verify channel existence before operations

## Future Extensions

- Protocol version negotiation
- Compression support
- Encryption/TLS integration
- Message acknowledgments
- Quality of Service (QoS) levels
- Wildcard channel subscriptions
- Message prioritization

## References

- Implementation: [src/Proto.hpp](../src/Proto.hpp)
- Implementation: [src/Proto.cpp](../src/Proto.cpp)
