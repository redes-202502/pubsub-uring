module;

export module herald.proto;

import std;

using namespace std;

export namespace herald {

enum class OpCode : uint8_t {
  // Connection lifecycle
  HANDSHAKE_PUB = 0x01,
  HANDSHAKE_SUB = 0x02,
  HANDSHAKE_ACK = 0x03,
  DISCONNECT = 0x04,
  // Pub/Sub operations
  PUBLISH = 0x10,
  SUBSCRIBE = 0x11,
  UNSUBSCRIBE = 0x12,
  MESSAGE = 0x13,
  // Control messages
  PING = 0x20,
  PONG = 0x21,
  // Error handling
  ERROR = 0xFF,
};

// Error codes
enum class ErrorCode : uint8_t {
  INVALID_HANDSHAKE = 0x01,
  CHANNEL_NOT_FOUND = 0x02,
  MESSAGE_TOO_LARGE = 0x03,
  RATE_LIMIT_EXCEEDED = 0x04,
  PROTOCOL_VERSION_MISMATCH = 0x05,
  INVALID_OPCODE = 0x06,
  MALFORMED_MESSAGE = 0x07,
  UNAUTHORIZED = 0x08,
};

constexpr uint16_t MAGIC = 0xCAFE;
constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024; // 1 MB
constexpr uint32_t HEADER_SIZE = 7;

struct FrameHeader {
  uint16_t magic;
  OpCode opcode;
  uint32_t length;

  static optional<FrameHeader> parse(span<const byte> data) {
    if (data.size() < HEADER_SIZE) {
      return {};
    }

    FrameHeader header;
    memcpy(&header.magic, data.data(), 2);
    memcpy(&header.opcode, data.data() + 2, 1);
    memcpy(&header.length, data.data() + 3, 4);

    if (header.magic != MAGIC) {
      return {};
    }

    if (header.length > MAX_PAYLOAD_SIZE) {
      return {};
    }

    return header;
  }

  array<byte, HEADER_SIZE> serialize() const {
    array<byte, HEADER_SIZE> buffer;
    memcpy(buffer.data(), &magic, 2);
    memcpy(buffer.data() + 2, &opcode, 1);
    memcpy(buffer.data() + 3, &length, 4);
    return buffer;
  }
};

struct DecodedMessage {
  OpCode opcode;
  uint32_t payloadLen;
  byte *payload;
};

struct EncodedMessage {
  uint32_t payloadLen;
  byte *payload;
};

class MessageEncoder {
public:
  static uint32_t sizeHandshakePub(string_view clientId) {
    return HEADER_SIZE + 2 + clientId.size();
  }

  static uint32_t sizeHandshakeSub(span<const uint8_t> channels,
                                   string_view clientId) {
    return HEADER_SIZE + 1 + channels.size() + 1 + clientId.size();
  }

  static uint32_t sizeHandshakeAck() {
    return HEADER_SIZE + 9; // status(1) + sessionId(8)
  }

  static uint32_t sizePublish(span<const byte> message) {
    return HEADER_SIZE + 1 + message.size(); // channel(1) + message
  }

  static uint32_t sizeMessage(span<const byte> message) {
    return HEADER_SIZE + 1 + 8 +
           message.size(); // channel(1) + timestamp(8) + message
  }

  static uint32_t sizeSubscribe() {
    return HEADER_SIZE + 1; // channel(1)
  }

  static uint32_t sizeUnsubscribe() {
    return HEADER_SIZE + 1; // channel(1)
  }

  static uint32_t sizeDisconnect() { return HEADER_SIZE; }

  static uint32_t sizePing() { return HEADER_SIZE; }

  static uint32_t sizePong() { return HEADER_SIZE; }

  static uint32_t sizeError() {
    return HEADER_SIZE + 1; // errorCode(1)
  }

  void encodeHandshakePub(byte *buffer, uint8_t channel, string_view clientId) {
    uint32_t payloadSize = 2 + clientId.size();
    FrameHeader header{MAGIC, OpCode::HANDSHAKE_PUB, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    uint32_t offset = HEADER_SIZE;
    buffer[offset++] = static_cast<byte>(channel);
    buffer[offset++] = static_cast<byte>(clientId.size());
    memcpy(buffer + offset, clientId.data(), clientId.size());
  }

  void encodeHandshakeSub(byte *buffer, span<const uint8_t> channels,
                          string_view clientId) {
    uint32_t payloadSize = 1 + channels.size() + 1 + clientId.size();
    FrameHeader header{MAGIC, OpCode::HANDSHAKE_SUB, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    uint32_t offset = HEADER_SIZE;
    buffer[offset++] = static_cast<byte>(channels.size());
    memcpy(buffer + offset, channels.data(), channels.size());
    offset += channels.size();
    buffer[offset++] = static_cast<byte>(clientId.size());
    memcpy(buffer + offset, clientId.data(), clientId.size());
  }

  void encodeHandshakeAck(byte *buffer, uint8_t status, uint64_t sessionId) {
    uint32_t payloadSize = 9;
    FrameHeader header{MAGIC, OpCode::HANDSHAKE_ACK, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    uint32_t offset = HEADER_SIZE;
    buffer[offset++] = static_cast<byte>(status);
    memcpy(buffer + offset, &sessionId, 8);
  }

  void encodePublish(byte *buffer, uint8_t channel, span<const byte> message) {
    uint32_t payloadSize = 1 + message.size();
    FrameHeader header{MAGIC, OpCode::PUBLISH, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    uint32_t offset = HEADER_SIZE;
    buffer[offset++] = static_cast<byte>(channel);
    memcpy(buffer + offset, message.data(), message.size());
  }

  void encodeMessage(byte *buffer, uint8_t channel, uint64_t timestamp,
                     span<const byte> message) {
    uint32_t payloadSize = 1 + 8 + message.size();
    FrameHeader header{MAGIC, OpCode::MESSAGE, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    uint32_t offset = HEADER_SIZE;
    buffer[offset++] = static_cast<byte>(channel);
    memcpy(buffer + offset, &timestamp, 8);
    offset += 8;
    memcpy(buffer + offset, message.data(), message.size());
  }

  void encodeSubscribe(byte *buffer, uint8_t channel) {
    uint32_t payloadSize = 1;
    FrameHeader header{MAGIC, OpCode::SUBSCRIBE, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    buffer[HEADER_SIZE] = static_cast<byte>(channel);
  }

  void encodeUnsubscribe(byte *buffer, uint8_t channel) {
    uint32_t payloadSize = 1;
    FrameHeader header{MAGIC, OpCode::UNSUBSCRIBE, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    buffer[HEADER_SIZE] = static_cast<byte>(channel);
  }

  void encodeDisconnect(byte *buffer) {
    FrameHeader header{MAGIC, OpCode::DISCONNECT, 0};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);
  }

  void encodePing(byte *buffer) {
    FrameHeader header{MAGIC, OpCode::PING, 0};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);
  }

  void encodePong(byte *buffer) {
    FrameHeader header{MAGIC, OpCode::PONG, 0};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);
  }

  void encodeError(byte *buffer, uint8_t errorCode) {
    uint32_t payloadSize = 1;
    FrameHeader header{MAGIC, OpCode::ERROR, payloadSize};
    auto headerBytes = header.serialize();
    memcpy(buffer, headerBytes.data(), HEADER_SIZE);

    buffer[HEADER_SIZE] = static_cast<byte>(errorCode);
  }
};

class MessageDecoder {
public:
  struct ParseResult {
    bool needMoreData = false;
    uint32_t bytesConsumed = 0;
    optional<DecodedMessage> message;
  };

  ParseResult decode(span<const byte> data) {
    if (data.size() < HEADER_SIZE) {
      return ParseResult{.needMoreData = true, .bytesConsumed = 0};
    }

    auto headerOpt = FrameHeader::parse(data);
    if (!headerOpt) {
      return ParseResult{.needMoreData = false, .bytesConsumed = 0};
    }

    const auto &header = *headerOpt;
    uint32_t totalSize = HEADER_SIZE + header.length;

    if (data.size() < totalSize) {
      return ParseResult{.needMoreData = true, .bytesConsumed = 0};
    }

    byte *payload = header.length > 0
                        ? const_cast<byte *>(data.data() + HEADER_SIZE)
                        : nullptr;

    DecodedMessage message{header.opcode, header.length, payload};

    return ParseResult{
        .needMoreData = false, .bytesConsumed = totalSize, .message = message};
  }
};

} // namespace herald