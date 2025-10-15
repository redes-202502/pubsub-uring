#ifndef PROTO_HPP
#define PROTO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

constexpr uint16_t MAGIC = 0xCAFE;
constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024; // 1 MB
constexpr uint32_t HEADER_SIZE = 7;

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

struct FrameHeader {
  uint16_t magic;
  OpCode opcode;
  uint32_t length;

  static std::optional<FrameHeader> parse(std::span<const std::byte> data);
  std::array<std::byte, HEADER_SIZE> serialize() const;
};

struct DecodedMessage {
  OpCode opcode;
  uint32_t payloadLen;
  std::byte *payload;
};

struct EncodedMessage {
  uint32_t payloadLen;
  std::byte *payload;
};

class MessageEncoder {
public:
  static uint32_t sizeHandshakePub(std::string_view clientId);
  static uint32_t sizeHandshakeSub(std::span<const uint8_t> channels,
                                   std::string_view clientId);
  static uint32_t sizeHandshakeAck();
  static uint32_t sizePublish(std::span<const std::byte> message);
  static uint32_t sizeMessage(std::span<const std::byte> message);
  static uint32_t sizeSubscribe();
  static uint32_t sizeUnsubscribe();
  static uint32_t sizeDisconnect();
  static uint32_t sizePing();
  static uint32_t sizePong();
  static uint32_t sizeError();

  void encodeHandshakePub(std::byte *buffer, uint8_t channel,
                          std::string_view clientId);
  void encodeHandshakeSub(std::byte *buffer, std::span<const uint8_t> channels,
                          std::string_view clientId);
  void encodeHandshakeAck(std::byte *buffer, uint8_t status,
                          uint64_t sessionId);
  void encodePublish(std::byte *buffer, uint8_t channel,
                     std::span<const std::byte> message);
  void encodeMessage(std::byte *buffer, uint8_t channel, uint64_t timestamp,
                     std::span<const std::byte> message);
  void encodeSubscribe(std::byte *buffer, uint8_t channel);
  void encodeUnsubscribe(std::byte *buffer, uint8_t channel);
  void encodeDisconnect(std::byte *buffer);
  void encodePing(std::byte *buffer);
  void encodePong(std::byte *buffer);
  void encodeError(std::byte *buffer, uint8_t errorCode);
};

class MessageDecoder {
public:
  struct ParseResult {
    bool needMoreData = false;
    uint32_t bytesConsumed = 0;
    std::optional<DecodedMessage> message;
  };

  ParseResult decode(std::span<const std::byte> data);
};
#endif