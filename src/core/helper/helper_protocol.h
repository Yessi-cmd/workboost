#pragma once

#include "core/model/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workboost::helper {

constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kNonceBytes = 32;
constexpr std::size_t kMaximumMessageBytes = 2048;

enum class Command : std::uint16_t {
  StopServiceTemporary = 1,
  StartServiceRestore = 2,
};

enum class Status : std::uint16_t {
  Succeeded = 1,
  Rejected = 2,
  Failed = 3,
  Uncertain = 4,
};

struct Request {
  std::uint64_t request_id{};
  Command command{Command::StopServiceTemporary};
  std::uint32_t timeout_ms{15000};
  std::array<std::uint8_t, kNonceBytes> nonce{};
  std::string service_name;
  std::string expected_identity_token;
};

struct Response {
  std::uint64_t request_id{};
  Status status{Status::Failed};
  ServiceState state_before{ServiceState::Unknown};
  ServiceState state_after{ServiceState::Unknown};
  std::uint32_t error_code{};
  std::array<std::uint8_t, kNonceBytes> nonce{};
  std::string message;
};

bool Validate(const Request& request, std::string* error = nullptr);
bool Validate(const Response& response, std::string* error = nullptr);

std::optional<std::vector<std::uint8_t>> Encode(
    const Request& request, std::string* error = nullptr);
std::optional<std::vector<std::uint8_t>> Encode(
    const Response& response, std::string* error = nullptr);
std::optional<Request> DecodeRequest(const std::vector<std::uint8_t>& bytes,
                                     std::string* error = nullptr);
std::optional<Response> DecodeResponse(const std::vector<std::uint8_t>& bytes,
                                       std::string* error = nullptr);

}  // namespace workboost::helper
