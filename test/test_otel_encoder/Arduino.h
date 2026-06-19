// Minimal Arduino `String` shim so OtlpEncoder can be compiled and exercised on
// the host (pio test -e native_otel). Only the surface the encoder + tests use
// is implemented; this is NOT a general String replacement.
#pragma once

#include <cstddef>
#include <cstring>
#include <string>

class String {
  public:
    String() = default;
    String(const char *s) : buf(s ? s : "") {}
    String(const std::string &s) : buf(s) {}

    const char *c_str() const { return buf.c_str(); }
    size_t length() const { return buf.size(); }
    bool isEmpty() const { return buf.empty(); }
    void reserve(size_t n) { buf.reserve(n); }

    String &operator+=(char c) {
        buf.push_back(c);
        return *this;
    }
    String &operator+=(const char *s) {
        buf += s;
        return *this;
    }
    String &operator+=(const String &s) {
        buf += s.buf;
        return *this;
    }

  private:
    std::string buf;
};

// macOS/BSD libc provide strlcpy; glibc (Linux CI) does not. Provide it there so
// the same test compiles cross-platform.
#if defined(__linux__)
#include <cstdint>
static inline size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = std::strlen(src);
    if (size != 0) {
        size_t copylen = srclen < (size - 1) ? srclen : (size - 1);
        std::memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}
#endif
