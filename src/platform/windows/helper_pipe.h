#pragma once

#include "core/helper/helper_protocol.h"
#include "platform/windows/windows_utils.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workboost::windows {

class HelperPipeConnection {
 public:
  HelperPipeConnection() = default;
  HelperPipeConnection(HelperPipeConnection&&) noexcept = default;
  HelperPipeConnection& operator=(HelperPipeConnection&&) noexcept = default;

  HelperPipeConnection(const HelperPipeConnection&) = delete;
  HelperPipeConnection& operator=(const HelperPipeConnection&) = delete;

  [[nodiscard]] bool Valid() const { return pipe_.Valid(); }
  std::optional<std::uint32_t> PeerProcessId(
      WindowsError* error = nullptr) const;
  bool WriteMessage(const std::vector<std::uint8_t>& message,
                    std::uint32_t timeout_ms,
                    WindowsError* error = nullptr) const;
  std::optional<std::vector<std::uint8_t>> ReadMessage(
      std::uint32_t timeout_ms, WindowsError* error = nullptr) const;

 private:
  friend class HelperPipeServer;
  friend std::optional<HelperPipeConnection> ConnectToHelperPipe(
      const std::wstring&, std::uint32_t, WindowsError*);

  HelperPipeConnection(UniqueHandle pipe, bool server_side)
      : pipe_(std::move(pipe)), server_side_(server_side) {}

  UniqueHandle pipe_;
  bool server_side_{};
};

class HelperPipeServer {
 public:
  HelperPipeServer() = default;
  HelperPipeServer(HelperPipeServer&&) noexcept = default;
  HelperPipeServer& operator=(HelperPipeServer&&) noexcept = default;

  HelperPipeServer(const HelperPipeServer&) = delete;
  HelperPipeServer& operator=(const HelperPipeServer&) = delete;

  static std::optional<HelperPipeServer> Create(
      WindowsError* error = nullptr);

  [[nodiscard]] const std::wstring& Name() const { return name_; }
  [[nodiscard]] const std::array<std::uint8_t, helper::kNonceBytes>& Nonce()
      const {
    return nonce_;
  }
  std::optional<HelperPipeConnection> Accept(
      std::uint32_t timeout_ms, WindowsError* error = nullptr);

 private:
  UniqueHandle pipe_;
  std::wstring name_;
  std::array<std::uint8_t, helper::kNonceBytes> nonce_{};
};

std::optional<HelperPipeConnection> ConnectToHelperPipe(
    const std::wstring& pipe_name, std::uint32_t timeout_ms,
    WindowsError* error = nullptr);

std::string HexNonce(
    const std::array<std::uint8_t, helper::kNonceBytes>& nonce);
std::optional<std::array<std::uint8_t, helper::kNonceBytes>> ParseHexNonce(
    const std::string& value);

}  // namespace workboost::windows
