#include "platform/windows/system_hardware.h"

#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace workboost::windows {
namespace {

// GetSystemFirmwareTable provider signature for the raw SMBIOS table. This is
// the integer value of the multi-character literal 'RSMB' (0x52534D42, 'R' in
// the most significant byte), written without the -Wmultichar literal.
constexpr DWORD kRawSmbiosSignature =
    static_cast<DWORD>('B') | (static_cast<DWORD>('M') << 8) |
    (static_cast<DWORD>('S') << 16) | (static_cast<DWORD>('R') << 24);

void Trim(std::string* value) {
  const auto is_space = [](unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };
  const auto first =
      std::find_if_not(value->begin(), value->end(),
                       [&](char ch) { return is_space(ch); });
  value->erase(value->begin(), first);
  const auto last = std::find_if_not(
      value->rbegin(), value->rend(), [&](char ch) { return is_space(ch); });
  value->erase(last.base(), value->end());
}

class UniqueRegistryKey {
 public:
  UniqueRegistryKey() = default;
  explicit UniqueRegistryKey(HKEY key) : key_(key) {}
  ~UniqueRegistryKey() {
    if (key_ != nullptr) RegCloseKey(key_);
  }

  UniqueRegistryKey(const UniqueRegistryKey&) = delete;
  UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;

  [[nodiscard]] HKEY Get() const { return key_; }
  // Mutable access for out-parameters (e.g. RegOpenKeyExW); the key is closed
  // by the destructor.
  [[nodiscard]] HKEY* Out() { return &key_; }

 private:
  HKEY key_{};
};

bool ReadCpuName(std::string* name) {
  UniqueRegistryKey key;
  constexpr wchar_t kKeyPath[] =
      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
  const LONG status =
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKeyPath, 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.Out());
  if (status != ERROR_SUCCESS) return false;

  wchar_t buffer[512]{};
  DWORD size = sizeof(buffer);
  const LONG query =
      RegQueryValueExW(key.Get(), L"ProcessorNameString", nullptr, nullptr,
                       reinterpret_cast<LPBYTE>(buffer), &size);
  if (query != ERROR_SUCCESS) return false;
  *name = WideToUtf8(std::wstring(buffer));
  Trim(name);
  return !name->empty();
}

std::uint16_t ReadU16(const unsigned char* data) {
  std::uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

const char* MemoryTypeName(unsigned char type) {
  switch (type) {
    case 0x15: return "DDR";
    case 0x16: return "DDR2";
    case 0x17: return "DDR2 FB-DIMM";
    case 0x18: return "DDR3";
    case 0x19: return "DDR4";
    case 0x1A: return "DDR5";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x1F: return "LPDDR5";
    case 0x20: return "LPDDR5X";
    case 0x22: return "DDR4 ECC";
    case 0x23: return "LPDDR4X";
    case 0x24: return "LPDDR5X";
    case 0x25: return "DDR5 ECC";
    default: return "";
  }
}

// Some AM5 firmware reports DDR5 modules with the DDR4 ECC type code; JEDEC
// DDR5 starts at 4800 MT/s (DDR4 tops out below that), so a DDR5-class speed
// wins over the code for labeling.
const char* ResolveMemoryType(unsigned char type_code, std::uint16_t speed_mt_s) {
  if ((type_code == 0x22 || type_code == 0x25) && speed_mt_s >= 4800) {
    return "DDR5";
  }
  return MemoryTypeName(type_code);
}

std::string FormatGigabytes(std::uint64_t total_mb) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1)
         << static_cast<double>(total_mb) / 1024.0 << " GB";
  return output.str();
}

// True when walking structures from `start` reaches the end-of-table marker
// (type 127) entirely in bounds. Used to locate the structure table, which a
// few firmware dumps prefix with a short non-structure header.
bool WalkIsClean(const std::vector<unsigned char>& table, std::size_t start) {
  const auto* data = table.data();
  const auto* end = data + table.size();
  const auto* cursor = data + start;
  while (cursor + 4 <= end) {
    const unsigned char type = cursor[0];
    const unsigned char length = cursor[1];
    if (length < 4 || cursor + length > end) return false;
    if (type == 127) return true;
    const auto* strings = cursor + length;
    while (strings + 1 < end && !(strings[0] == 0 && strings[1] == 0)) {
      ++strings;
    }
    cursor = strings + 2;
  }
  return false;
}

// Locates the start of the SMBIOS structure table. Most dumps begin directly
// with a Type 0 (BIOS Information) structure; some begin with a short prefix
// (e.g. a Type 0 whose length field is too small to be real).
std::optional<std::size_t> FindStructureTableStart(
    const std::vector<unsigned char>& table) {
  const auto* data = table.data();
  for (std::size_t i = 0; i < table.size() && i < 256; ++i) {
    if (i + 4 <= table.size() && data[i] == 0x00 && data[i + 1] >= 0x10 &&
        data[i + 1] <= 0x1A && WalkIsClean(table, i)) {
      return i;
    }
  }
  for (std::size_t i = 0; i < table.size() && i < 256; ++i) {
    if (WalkIsClean(table, i)) return i;
  }
  return std::nullopt;
}

// Walks the raw SMBIOS structure table, summing Type 17 (Memory Device)
// capacities and remembering the fastest module speed and memory type.
bool SummarizeMemory(const std::vector<unsigned char>& table,
                     std::string* model) {
  const auto start = FindStructureTableStart(table);
  if (!start) return false;
  const auto* data = table.data();
  const auto* end = data + table.size();
  std::uint64_t total_mb = 0;
  std::uint16_t max_speed = 0;
  int type_code = 0;
  bool found = false;
  const auto* cursor = data + *start;
  while (cursor + 4 <= end) {
    const unsigned char type = cursor[0];
    const unsigned char length = cursor[1];
    if (length < 4 || cursor + length > end) break;
    if (type == 127) break;  // end-of-table marker
    if (type == 17 && length >= 0x16) {
      // Offsets are relative to the structure start; the length field includes
      // the 4-byte header. Capacity at 0x0C, memory type at 0x12, speed at
      // 0x15.
      const std::uint16_t size_raw = ReadU16(cursor + 0x0C);
      if (size_raw != 0 && size_raw != 0xFFFF) {
        if (size_raw & 0x8000) {
          // Units bit set: value is in GB (>= 32 GB modules).
          total_mb += static_cast<std::uint64_t>(size_raw & 0x7FFF) * 1024ULL;
        } else {
          total_mb += size_raw;
        }
      }
      const std::uint16_t speed = ReadU16(cursor + 0x15);
      max_speed = std::max(max_speed, speed);
      if (cursor[0x12] != 0) type_code = cursor[0x12];
      found = true;
    }
    // Unformatted string area: a sequence of NUL-terminated strings ended by
    // an empty string (two NUL bytes).
    const auto* strings = cursor + length;
    while (strings + 1 < end && !(strings[0] == 0 && strings[1] == 0)) {
      ++strings;
    }
    cursor = strings + 2;
  }
  if (!found) return false;

  const char* type_name = ResolveMemoryType(
      static_cast<unsigned char>(type_code), max_speed);
  std::ostringstream output;
  if (type_name != nullptr && *type_name != '\0') output << type_name;
  if (max_speed != 0) {
    if (output.tellp() != 0) output << " · ";
    output << max_speed << " MT/s";
  }
  if (total_mb != 0) {
    if (output.tellp() != 0) output << " · ";
    output << FormatGigabytes(total_mb);
  }
  *model = output.str();
  return !model->empty();
}

}  // namespace

bool QueryCpuModel(std::string* model, std::string* error) {
  std::string name;
  if (!ReadCpuName(&name)) {
    if (error) *error = "CPU model registry value is unavailable";
    return false;
  }
  *model = std::move(name);
  return true;
}

bool QueryMemoryModel(std::string* model, std::string* error) {
  const UINT size = GetSystemFirmwareTable(kRawSmbiosSignature, 0, nullptr, 0);
  if (size == 0) {
    if (error) *error = "SMBIOS firmware table is unavailable";
    return false;
  }
  std::vector<unsigned char> table(size);
  const UINT actual =
      GetSystemFirmwareTable(kRawSmbiosSignature, 0, table.data(), table.size());
  if (actual == 0 || actual > size) {
    if (error) *error = "SMBIOS firmware table read failed";
    return false;
  }
  table.resize(actual);
  if (!SummarizeMemory(table, model)) {
    if (error) *error = "no SMBIOS memory devices found";
    return false;
  }
  return true;
}

}  // namespace workboost::windows
