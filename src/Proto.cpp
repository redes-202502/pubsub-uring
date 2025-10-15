#include "Proto.hpp"

using namespace std;

optional<FrameHeader> FrameHeader::parse(span<const byte> data) {
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

array<byte, HEADER_SIZE> FrameHeader::serialize() const {
  array<byte, HEADER_SIZE> buffer;
  memcpy(buffer.data(), &magic, 2);
  memcpy(buffer.data() + 2, &opcode, 1);
  memcpy(buffer.data() + 3, &length, 4);
  return buffer;
}

uint32_t MessageEncoder::sizeHandshakePub(string_view clientId) {
  return HEADER_SIZE + 2 + clientId.size();
}

uint32_t MessageEncoder::sizeHandshakeSub(span<const uint8_t> channels,
                                          string_view clientId) {
  return HEADER_SIZE + 1 + channels.size() + 1 + clientId.size();
}

uint32_t MessageEncoder::sizeHandshakeAck() {
  return HEADER_SIZE + 9; // status(1) + sessionId(8)
}

uint32_t MessageEncoder::sizePublish(span<const byte> message) {
  return HEADER_SIZE + 1 + message.size(); // channel(1) + message
}

uint32_t MessageEncoder::sizeMessage(span<const byte> message) {
  return HEADER_SIZE + 1 + 8 +
         message.size(); // channel(1) + timestamp(8) + message
}

uint32_t MessageEncoder::sizeSubscribe() {
  return HEADER_SIZE + 1; // channel(1)
}

uint32_t MessageEncoder::sizeUnsubscribe() {
  return HEADER_SIZE + 1; // channel(1)
}

uint32_t MessageEncoder::sizeDisconnect() { return HEADER_SIZE; }

uint32_t MessageEncoder::sizePing() { return HEADER_SIZE; }

uint32_t MessageEncoder::sizePong() { return HEADER_SIZE; }

uint32_t MessageEncoder::sizeError() {
  return HEADER_SIZE + 1; // errorCode(1)
}

void MessageEncoder::encodeHandshakePub(byte *buffer, uint8_t channel,
                                        string_view clientId) {
  uint32_t payloadSize = 2 + clientId.size();
  FrameHeader header{MAGIC, OpCode::HANDSHAKE_PUB, payloadSize};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);

  uint32_t offset = HEADER_SIZE;
  buffer[offset++] = static_cast<byte>(channel);
  buffer[offset++] = static_cast<byte>(clientId.size());
  memcpy(buffer + offset, clientId.data(), clientId.size());
}

void MessageEncoder::encodeHandshakeSub(byte *buffer,
                                        span<const uint8_t> channels,
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

void MessageEncoder::encodeHandshakeAck(byte *buffer, uint8_t status,
                                        uint64_t sessionId) {
  uint32_t payloadSize = 9;
  FrameHeader header{MAGIC, OpCode::HANDSHAKE_ACK, payloadSize};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);

  uint32_t offset = HEADER_SIZE;
  buffer[offset++] = static_cast<byte>(status);
  memcpy(buffer + offset, &sessionId, 8);
}

void MessageEncoder::encodePublish(byte *buffer, uint8_t channel,
                                   span<const byte> message) {
  uint32_t payloadSize = 1 + message.size();
  FrameHeader header{MAGIC, OpCode::PUBLISH, payloadSize};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);

  uint32_t offset = HEADER_SIZE;
  buffer[offset++] = static_cast<byte>(channel);
  memcpy(buffer + offset, message.data(), message.size());
}

void MessageEncoder::encodeMessage(byte *buffer, uint8_t channel,
                                   uint64_t timestamp,
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

void MessageEncoder::encodeSubscribe(byte *buffer, uint8_t channel) {
  uint32_t payloadSize = 1;
  FrameHeader header{MAGIC, OpCode::SUBSCRIBE, payloadSize};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);

  buffer[HEADER_SIZE] = static_cast<byte>(channel);
}

void MessageEncoder::encodeUnsubscribe(byte *buffer, uint8_t channel) {
  uint32_t payloadSize = 1;
  FrameHeader header{MAGIC, OpCode::UNSUBSCRIBE, payloadSize};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);

  buffer[HEADER_SIZE] = static_cast<byte>(channel);
}

void MessageEncoder::encodeDisconnect(byte *buffer) {
  FrameHeader header{MAGIC, OpCode::DISCONNECT, 0};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);
}

void MessageEncoder::encodePing(byte *buffer) {
  FrameHeader header{MAGIC, OpCode::PING, 0};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);
}

void MessageEncoder::encodePong(byte *buffer) {
  FrameHeader header{MAGIC, OpCode::PONG, 0};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);
}

void MessageEncoder::encodeError(byte *buffer, uint8_t errorCode) {
  uint32_t payloadSize = 1;
  FrameHeader header{MAGIC, OpCode::ERROR, payloadSize};
  auto headerBytes = header.serialize();
  memcpy(buffer, headerBytes.data(), HEADER_SIZE);

  buffer[HEADER_SIZE] = static_cast<byte>(errorCode);
}

MessageDecoder::ParseResult MessageDecoder::decode(span<const byte> data) {
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