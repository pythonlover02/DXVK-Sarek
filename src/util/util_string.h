#pragma once

#include <cstring>
#include <string>
#include <sstream>
#include <vector>

#include "./com/com_include.h"

namespace dxvk::str {
  
  std::string fromws(const WCHAR *ws);

  void tows(const char* mbs, WCHAR* wcs, size_t wcsLen);

  template <size_t N>
  void tows(const char* mbs, WCHAR (&wcs)[N]) {
    return tows(mbs, wcs, N);
  }

  std::wstring tows(const char* mbs);

#ifdef _WIN32
  inline std::wstring topath(const char* mbs) { return tows(mbs); }
#else
  inline std::string  topath(const char* mbs) { return std::string(mbs); }
#endif
  
  inline void format1(std::stringstream&) { }

  template<typename... Tx>
  void format1(std::stringstream& str, const WCHAR *arg, const Tx&... args) {
    str << fromws(arg);
    format1(str, args...);
  }

  template<typename T, typename... Tx>
  void format1(std::stringstream& str, const T& arg, const Tx&... args) {
    str << arg;
    format1(str, args...);
  }
  
  template<typename... Args>
  std::string format(const Args&... args) {
    std::stringstream stream;
    format1(stream, args...);
    return stream.str();
  }

  inline void strlcpy(char* dst, const char* src, size_t count) {
    std::strncpy(dst, src, count);
    if (count > 0)
      dst[count - 1] = '\0';
  }
  
  /**
   * \brief Split string at one or more delimiters characters
   * 
   * \param [in] string String to split
   * \param [in] delims Delimiter characters
   * \returns Vector of substring views
  */
  inline std::vector<std::string_view> split(std::string_view string, std::string_view delims = " ") {
    std::vector<std::string_view> tokens;

    for (size_t start = 0; start < string.size(); ) {
      // Find first delimiter
      const auto end = string.find_first_of(delims, start);

      // Add non-empty tokens
      if (start != end)
        tokens.emplace_back(string.substr(start, end-start));

      // Break at the end of string
      if (end == std::string_view::npos)
        break;

      start = end + 1;
    }
    return tokens;
  }
  /** Compares ASCII characters in a case-insensitive way */
  inline bool compareCharsCaseInsensitive(char a, char b) {
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    return a == b;
  }

  /** Compare ASCII string in a case-insensitive way */
  inline bool compareCaseInsensitive(const char* a, const char* b) {
    for (size_t i = 0u; a[i] || b[i]; i++) {
      if (!compareCharsCaseInsensitive(a[i], b[i]))
        return false;
    }

    return true;
  }

}
