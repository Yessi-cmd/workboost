#pragma once

#include <initializer_list>
#include <string>

namespace workboost {

enum class LocaleId { English, Chinese };

// User-interface language for the dashboard. English strings are the canonical
// keys; the Chinese dictionary maps each key to its translation. English is an
// identity lookup, so any string not yet in the dictionary degrades to English
// instead of breaking. The active locale is process-global (single-threaded UI
// plus a background sampler that reads it; a switch between frames is benign).
class Locale {
 public:
  static LocaleId Current();
  static void Set(LocaleId id);
  [[nodiscard]] static bool IsChinese();

  // Returns the translation of key in the current locale, or key itself when
  // untranslated.
  [[nodiscard]] static std::string Get(const char* key);
  [[nodiscard]] static std::string Get(const std::string& key);

  // Get, then substitutes {0}, {1}, ... with the supplied arguments.
  [[nodiscard]] static std::string Format(
      const char* key, std::initializer_list<std::string> args);
};

}  // namespace workboost
