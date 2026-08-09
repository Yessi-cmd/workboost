#include "core/helper/helper_protocol.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace workboost::helper {
namespace {

constexpr std::uint32_t kMagic = 0x31504257;  // "WBP1" in little endian.
constexpr std::uint16_t kRequestKind = 1;
constexpr std::uint16_t kResponseKind = 2;
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kMaximumServiceNameBytes = 256;
constexpr std::size_t kIdentityTokenBytes = 16;
constexpr std::size_t kMaximumResponseTextBytes = 512;

void SetError(std::string* error, const std::string& value) {
  if (error) *error = value;
}

bool IsValidUtf8(const std::string& value, bool service_name) {
  if (value.empty()) return !service_name;
  for (std::size_t i = 0; i < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[i]);
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7f) {
      code_point = first;
      continuation = 0;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      continuation = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      continuation = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      continuation = 3;
    } else {
      return false;
    }
    if (i + continuation >= value.size()) return false;
    for (std::size_t j = 0; j < continuation; ++j) {
      const unsigned char next =
          static_cast<unsigned char>(value[i + j + 1]);
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation == 2 && code_point < 0x800) ||
        (continuation == 3 && code_point < 0x10000) ||
        code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return false;
    }
    if (code_point == 0 || code_point < 0x20 ||
        (service_name && (code_point == '/' || code_point == '\\'))) {
      return false;
    }
    i += continuation + 1;
  }
  return true;
}

bool IsNoncePresent(
    const std::array<std::uint8_t, kNonceBytes>& nonce) {
  return std::any_of(nonce.begin(), nonce.end(),
                     [](std::uint8_t value) { return value != 0; });
}

bool IsIdentityToken(const std::string& value) {
  if (value.size() != kIdentityTokenBytes) return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool IsCommand(Command command) {
  return command == Command::StopServiceTemporary ||
         command == Command::StartServiceRestore;
}

bool IsStatus(Status status) {
  return status == Status::Succeeded || status == Status::Rejected ||
         status == Status::Failed || status == Status::Uncertain;
}

std::optional<std::uint16_t> ServiceStateToWire(ServiceState state) {
  switch (state) {
    case ServiceState::Unknown: return 0;
    case ServiceState::Stopped: return 1;
    case ServiceState::StartPending: return 2;
    case ServiceState::StopPending: return 3;
    case ServiceState::Running: return 4;
    case ServiceState::ContinuePending: return 5;
    case ServiceState::PausePending: return 6;
    case ServiceState::Paused: return 7;
  }
  return std::nullopt;
}

std::optional<ServiceState> ServiceStateFromWire(std::uint16_t state) {
  switch (state) {
    case 0: return ServiceState::Unknown;
    case 1: return ServiceState::Stopped;
    case 2: return ServiceState::StartPending;
    case 3: return ServiceState::StopPending;
    case 4: return ServiceState::Running;
    case 5: return ServiceState::ContinuePending;
    case 6: return ServiceState::PausePending;
    case 7: return ServiceState::Paused;
    default: return std::nullopt;
  }
}

void Append16(std::vector<std::uint8_t>* output, std::uint16_t value) {
  output->push_back(static_cast<std::uint8_t>(value & 0xff));
  output->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void Append32(std::vector<std::uint8_t>* output, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
}

void Append64(std::vector<std::uint8_t>* output, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    output->push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
}

void AppendString(std::vector<std::uint8_t>* output,
                  const std::string& value) {
  Append16(output, static_cast<std::uint16_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
}

void WriteHeader(std::vector<std::uint8_t>* output, std::uint16_t kind) {
  Append32(output, kMagic);
  Append16(output, kProtocolVersion);
  Append16(output, kind);
  Append32(output, 0);
}

void FinishHeader(std::vector<std::uint8_t>* output) {
  const std::uint32_t payload =
      static_cast<std::uint32_t>(output->size() - kHeaderBytes);
  for (int i = 0; i < 4; ++i) {
    (*output)[8 + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((payload >> (i * 8)) & 0xff);
  }
}

class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

  bool Read16(std::uint16_t* value) {
    if (!CanRead(2)) return false;
    *value = static_cast<std::uint16_t>(bytes_[position_]) |
             static_cast<std::uint16_t>(bytes_[position_ + 1] << 8);
    position_ += 2;
    return true;
  }

  bool Read32(std::uint32_t* value) {
    if (!CanRead(4)) return false;
    *value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      *value |= static_cast<std::uint32_t>(bytes_[position_++]) << shift;
    }
    return true;
  }

  bool Read64(std::uint64_t* value) {
    if (!CanRead(8)) return false;
    *value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      *value |= static_cast<std::uint64_t>(bytes_[position_++]) << shift;
    }
    return true;
  }

  template <std::size_t Size>
  bool ReadArray(std::array<std::uint8_t, Size>* value) {
    if (!CanRead(Size)) return false;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), Size,
                value->begin());
    position_ += Size;
    return true;
  }

  bool ReadString(std::string* value) {
    std::uint16_t size = 0;
    if (!Read16(&size) || !CanRead(size)) return false;
    value->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                  bytes_.begin() +
                      static_cast<std::ptrdiff_t>(position_ + size));
    position_ += size;
    return true;
  }

  [[nodiscard]] bool Finished() const { return position_ == bytes_.size(); }

 private:
  [[nodiscard]] bool CanRead(std::size_t size) const {
    return size <= bytes_.size() - std::min(position_, bytes_.size());
  }

  const std::vector<std::uint8_t>& bytes_;
  std::size_t position_{};
};

bool ReadHeader(Reader* reader, std::uint16_t expected_kind,
                std::uint32_t total_size, std::string* error) {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t kind = 0;
  std::uint32_t payload_size = 0;
  if (!reader->Read32(&magic) || !reader->Read16(&version) ||
      !reader->Read16(&kind) || !reader->Read32(&payload_size)) {
    SetError(error, "helper message header is truncated");
    return false;
  }
  if (magic != kMagic || version != kProtocolVersion ||
      kind != expected_kind || payload_size != total_size - kHeaderBytes) {
    SetError(error, "helper message header is invalid");
    return false;
  }
  return true;
}

bool CheckMessageSize(const std::vector<std::uint8_t>& bytes,
                      std::string* error) {
  if (bytes.size() < kHeaderBytes || bytes.size() > kMaximumMessageBytes ||
      bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    SetError(error, "helper message size is outside the protocol limit");
    return false;
  }
  return true;
}

}  // namespace

bool Validate(const Request& request, std::string* error) {
  if (request.request_id == 0) {
    SetError(error, "helper request id must be nonzero");
    return false;
  }
  if (!IsCommand(request.command)) {
    SetError(error, "helper command is not in the action allowlist");
    return false;
  }
  if (request.timeout_ms < 1000 || request.timeout_ms > 30000) {
    SetError(error, "helper timeout is outside the allowlist");
    return false;
  }
  if (!IsNoncePresent(request.nonce)) {
    SetError(error, "helper nonce must be nonzero");
    return false;
  }
  if (request.service_name.empty() ||
      request.service_name.size() > kMaximumServiceNameBytes ||
      !IsValidUtf8(request.service_name, true)) {
    SetError(error, "helper service name is invalid");
    return false;
  }
  if (!IsIdentityToken(request.expected_identity_token)) {
    SetError(error, "helper service identity token is invalid");
    return false;
  }
  return true;
}

bool Validate(const Response& response, std::string* error) {
  if (response.request_id == 0 || !IsStatus(response.status) ||
      !IsNoncePresent(response.nonce) ||
      !ServiceStateToWire(response.state_before) ||
      !ServiceStateToWire(response.state_after) ||
      response.message.size() > kMaximumResponseTextBytes ||
      !IsValidUtf8(response.message, false)) {
    SetError(error, "helper response contains an invalid field");
    return false;
  }
  if (response.status == Status::Succeeded && response.error_code != 0) {
    SetError(error, "successful helper response cannot contain an error code");
    return false;
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> Encode(const Request& request,
                                                std::string* error) {
  if (!Validate(request, error)) return std::nullopt;
  std::vector<std::uint8_t> output;
  output.reserve(128 + request.service_name.size());
  WriteHeader(&output, kRequestKind);
  Append64(&output, request.request_id);
  Append16(&output, static_cast<std::uint16_t>(request.command));
  Append16(&output, 0);
  Append32(&output, request.timeout_ms);
  output.insert(output.end(), request.nonce.begin(), request.nonce.end());
  AppendString(&output, request.service_name);
  AppendString(&output, request.expected_identity_token);
  FinishHeader(&output);
  if (!CheckMessageSize(output, error)) return std::nullopt;
  return output;
}

std::optional<std::vector<std::uint8_t>> Encode(const Response& response,
                                                std::string* error) {
  if (!Validate(response, error)) return std::nullopt;
  const auto before = ServiceStateToWire(response.state_before);
  const auto after = ServiceStateToWire(response.state_after);
  std::vector<std::uint8_t> output;
  output.reserve(96 + response.message.size());
  WriteHeader(&output, kResponseKind);
  Append64(&output, response.request_id);
  Append16(&output, static_cast<std::uint16_t>(response.status));
  Append16(&output, *before);
  Append16(&output, *after);
  Append16(&output, 0);
  Append32(&output, response.error_code);
  output.insert(output.end(), response.nonce.begin(), response.nonce.end());
  AppendString(&output, response.message);
  FinishHeader(&output);
  if (!CheckMessageSize(output, error)) return std::nullopt;
  return output;
}

std::optional<Request> DecodeRequest(const std::vector<std::uint8_t>& bytes,
                                     std::string* error) {
  if (!CheckMessageSize(bytes, error)) return std::nullopt;
  Reader reader(bytes);
  if (!ReadHeader(&reader, kRequestKind,
                  static_cast<std::uint32_t>(bytes.size()), error)) {
    return std::nullopt;
  }
  Request request;
  std::uint16_t command = 0;
  std::uint16_t reserved = 0;
  if (!reader.Read64(&request.request_id) || !reader.Read16(&command) ||
      !reader.Read16(&reserved) || !reader.Read32(&request.timeout_ms) ||
      !reader.ReadArray(&request.nonce) ||
      !reader.ReadString(&request.service_name) ||
      !reader.ReadString(&request.expected_identity_token) ||
      !reader.Finished() || reserved != 0) {
    SetError(error, "helper request payload is invalid");
    return std::nullopt;
  }
  request.command = static_cast<Command>(command);
  if (!Validate(request, error)) return std::nullopt;
  return request;
}

std::optional<Response> DecodeResponse(
    const std::vector<std::uint8_t>& bytes, std::string* error) {
  if (!CheckMessageSize(bytes, error)) return std::nullopt;
  Reader reader(bytes);
  if (!ReadHeader(&reader, kResponseKind,
                  static_cast<std::uint32_t>(bytes.size()), error)) {
    return std::nullopt;
  }
  Response response;
  std::uint16_t status = 0;
  std::uint16_t before = 0;
  std::uint16_t after = 0;
  std::uint16_t reserved = 0;
  if (!reader.Read64(&response.request_id) || !reader.Read16(&status) ||
      !reader.Read16(&before) || !reader.Read16(&after) ||
      !reader.Read16(&reserved) || !reader.Read32(&response.error_code) ||
      !reader.ReadArray(&response.nonce) ||
      !reader.ReadString(&response.message) || !reader.Finished() ||
      reserved != 0) {
    SetError(error, "helper response payload is invalid");
    return std::nullopt;
  }
  const auto state_before = ServiceStateFromWire(before);
  const auto state_after = ServiceStateFromWire(after);
  response.status = static_cast<Status>(status);
  if (!state_before || !state_after) {
    SetError(error, "helper response contains an unknown service state");
    return std::nullopt;
  }
  response.state_before = *state_before;
  response.state_after = *state_after;
  if (!Validate(response, error)) return std::nullopt;
  return response;
}

}  // namespace workboost::helper
