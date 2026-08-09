#include "platform/windows/serial_port_api.h"

#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

class UniqueDeviceInfoSet {
 public:
  explicit UniqueDeviceInfoSet(HDEVINFO value) : value_(value) {}
  ~UniqueDeviceInfoSet() {
    if (Valid()) SetupDiDestroyDeviceInfoList(value_);
  }

  UniqueDeviceInfoSet(const UniqueDeviceInfoSet&) = delete;
  UniqueDeviceInfoSet& operator=(const UniqueDeviceInfoSet&) = delete;

  [[nodiscard]] bool Valid() const {
    return value_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HDEVINFO Get() const { return value_; }

 private:
  HDEVINFO value_{INVALID_HANDLE_VALUE};
};

class UniqueRegistryKey {
 public:
  explicit UniqueRegistryKey(HKEY value) : value_(value) {}
  ~UniqueRegistryKey() {
    if (Valid()) RegCloseKey(value_);
  }

  UniqueRegistryKey(const UniqueRegistryKey&) = delete;
  UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;

  [[nodiscard]] bool Valid() const {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HKEY Get() const { return value_; }

 private:
  HKEY value_{};
};

std::wstring DeviceProperty(HDEVINFO devices, SP_DEVINFO_DATA* device,
                            DWORD property) {
  DWORD type = 0;
  DWORD bytes = 0;
  SetLastError(ERROR_SUCCESS);
  SetupDiGetDeviceRegistryPropertyW(devices, device, property, &type, nullptr,
                                    0, &bytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t) ||
      (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return {};
  }
  std::vector<BYTE> buffer(bytes + sizeof(wchar_t), 0);
  if (!SetupDiGetDeviceRegistryPropertyW(
          devices, device, property, &type, buffer.data(),
          static_cast<DWORD>(buffer.size()), &bytes) ||
      (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return {};
  }
  return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring PortName(HDEVINFO devices, SP_DEVINFO_DATA* device) {
  UniqueRegistryKey key(SetupDiOpenDevRegKey(
      devices, device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE));
  if (!key.Valid()) return {};
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key.Get(), L"PortName", nullptr, &type, nullptr,
                       &bytes) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    return {};
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  if (RegQueryValueExW(key.Get(), L"PortName", nullptr, &type,
                       reinterpret_cast<LPBYTE>(buffer.data()), &bytes) !=
      ERROR_SUCCESS) {
    return {};
  }
  return buffer.data();
}

std::optional<std::uint32_t> ComNumber(const std::string& value) {
  if (value.size() < 4 ||
      std::toupper(static_cast<unsigned char>(value[0])) != 'C' ||
      std::toupper(static_cast<unsigned char>(value[1])) != 'O' ||
      std::toupper(static_cast<unsigned char>(value[2])) != 'M') {
    return std::nullopt;
  }
  std::uint64_t number = 0;
  for (std::size_t i = 3; i < value.size(); ++i) {
    if (value[i] < '0' || value[i] > '9') return std::nullopt;
    number = number * 10 + static_cast<unsigned int>(value[i] - '0');
    if (number > UINT32_MAX) return std::nullopt;
  }
  if (number == 0) return std::nullopt;
  return static_cast<std::uint32_t>(number);
}

}  // namespace

SerialPortQueryResult SerialPortApi::QueryPresent() {
  SerialPortQueryResult result;
  UniqueDeviceInfoSet devices(SetupDiGetClassDevsW(
      &GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT));
  if (!devices.Valid()) {
    result.error = LastError("Enumerate present serial port devices");
    return result;
  }

  for (DWORD index = 0;; ++index) {
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiEnumDeviceInfo(devices.Get(), index, &device)) {
      const DWORD code = GetLastError();
      if (code == ERROR_NO_MORE_ITEMS) break;
      result.error = LastError("Read serial port device", code);
      return result;
    }
    const std::string port = WideToUtf8(PortName(devices.Get(), &device));
    if (!ComNumber(port)) continue;
    std::wstring friendly =
        DeviceProperty(devices.Get(), &device, SPDRP_FRIENDLYNAME);
    if (friendly.empty()) {
      friendly = DeviceProperty(devices.Get(), &device, SPDRP_DEVICEDESC);
    }
    result.ports.push_back(
        SerialPortSnapshot{port, WideToUtf8(friendly),
                           WideToUtf8(DeviceProperty(
                               devices.Get(), &device, SPDRP_MFG))});
  }

  std::sort(result.ports.begin(), result.ports.end(),
            [](const SerialPortSnapshot& left,
               const SerialPortSnapshot& right) {
              return *ComNumber(left.port_name) < *ComNumber(right.port_name);
            });
  result.success = true;
  return result;
}

}  // namespace workboost::windows
