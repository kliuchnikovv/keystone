// Minimal JSON for the app protocol (docs/app-protocol.md): an allocation-free
// writer into a caller buffer and a strict, bounded reader. Portable (no ESP-IDF),
// so the protocol layer is host-tested.
//
// The reader is hostile-input safe by construction: fixed token budget, fixed
// nesting depth, whole-input validation before any accessor works, no recursion
// beyond kMaxDepth, and every accessor bounds-checks. It accepts standard JSON only
// (no comments, no trailing commas, no NaN/Infinity, no raw control characters in
// strings).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core {

// ---- writer ----------------------------------------------------------------

class JsonWriter {
 public:
  static constexpr int kMaxDepth = 8;

  JsonWriter(char* buf, std::size_t cap) noexcept : buf_(buf), cap_(cap) {
    if (cap_ == 0) ok_ = false;
    else buf_[0] = '\0';
  }

  JsonWriter& begin_object() noexcept { return open('{'); }
  JsonWriter& end_object() noexcept { return close('}'); }
  JsonWriter& begin_array() noexcept { return open('['); }
  JsonWriter& end_array() noexcept { return close(']'); }

  // Object member name; the next call writes its value.
  JsonWriter& key(const char* k) noexcept {
    sep();
    string(k);
    put(':');
    after_key_ = true;
    return *this;
  }

  JsonWriter& str(const char* s) noexcept {
    value_prefix();
    string(s);
    return *this;
  }
  JsonWriter& i64(std::int64_t v) noexcept {
    value_prefix();
    char t[24];
    const int n = std::snprintf(t, sizeof(t), "%lld", static_cast<long long>(v));
    put_n(t, n > 0 ? static_cast<std::size_t>(n) : 0);
    return *this;
  }
  JsonWriter& u64(std::uint64_t v) noexcept {
    value_prefix();
    char t[24];
    const int n = std::snprintf(t, sizeof(t), "%llu", static_cast<unsigned long long>(v));
    put_n(t, n > 0 ? static_cast<std::size_t>(n) : 0);
    return *this;
  }
  // JSON has no NaN/Infinity: those become null.
  JsonWriter& f64(double v) noexcept {
    value_prefix();
    if (!std::isfinite(v)) {
      put_n("null", 4);
      return *this;
    }
    char t[32];
    const int n = std::snprintf(t, sizeof(t), "%.9g", v);
    put_n(t, n > 0 ? static_cast<std::size_t>(n) : 0);
    return *this;
  }
  JsonWriter& boolean(bool v) noexcept {
    value_prefix();
    put_n(v ? "true" : "false", v ? 4 : 5);
    return *this;
  }
  JsonWriter& null() noexcept {
    value_prefix();
    put_n("null", 4);
    return *this;
  }

  // Convenience: "key": value
  JsonWriter& kv(const char* k, const char* s) noexcept { return key(k).str(s); }
  JsonWriter& kv(const char* k, std::int64_t v) noexcept { return key(k).i64(v); }
  JsonWriter& kv(const char* k, int v) noexcept { return key(k).i64(v); }
  JsonWriter& kv(const char* k, std::uint32_t v) noexcept { return key(k).u64(v); }
  JsonWriter& kv(const char* k, bool v) noexcept { return key(k).boolean(v); }

  // True if everything fit and brackets balance (call after the last end_*).
  bool ok() const noexcept { return ok_ && depth_ == 0 && !after_key_; }
  std::size_t size() const noexcept { return len_; }
  const char* c_str() const noexcept { return buf_; }

 private:
  void sep() noexcept {
    if (need_comma_[depth_]) put(',');
    need_comma_[depth_] = true;
  }
  // Before any value: after a key no separator is needed; otherwise a comma if the
  // container already has an item.
  void value_prefix() noexcept {
    if (after_key_) after_key_ = false;
    else sep();
  }
  JsonWriter& open(char c) noexcept {
    value_prefix();
    put(c);
    if (depth_ + 1 >= kMaxDepth) {
      ok_ = false;
      return *this;
    }
    ++depth_;
    need_comma_[depth_] = false;
    return *this;
  }
  JsonWriter& close(char c) noexcept {
    if (depth_ == 0) {
      ok_ = false;
      return *this;
    }
    --depth_;
    put(c);
    return *this;
  }
  void string(const char* s) noexcept {
    put('"');
    for (; s != nullptr && *s != '\0'; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      switch (c) {
        case '"': put_n("\\\"", 2); break;
        case '\\': put_n("\\\\", 2); break;
        case '\n': put_n("\\n", 2); break;
        case '\r': put_n("\\r", 2); break;
        case '\t': put_n("\\t", 2); break;
        case '\b': put_n("\\b", 2); break;
        case '\f': put_n("\\f", 2); break;
        default:
          if (c < 0x20) {
            char t[8];
            std::snprintf(t, sizeof(t), "\\u%04x", static_cast<unsigned>(c));
            put_n(t, 6);
          } else {
            put(static_cast<char>(c));
          }
      }
    }
    put('"');
  }
  void put(char c) noexcept { put_n(&c, 1); }
  void put_n(const char* s, std::size_t n) noexcept {
    if (!ok_) return;
    if (len_ + n + 1 > cap_) {  // keep room for the terminating NUL
      ok_ = false;
      return;
    }
    std::memcpy(buf_ + len_, s, n);
    len_ += n;
    buf_[len_] = '\0';
  }

  char* buf_;
  std::size_t cap_;
  std::size_t len_ = 0;
  bool ok_ = true;
  bool after_key_ = false;
  int depth_ = 0;
  bool need_comma_[kMaxDepth] = {};
};

// ---- reader ----------------------------------------------------------------

template <int MaxTokens = 96>
class BasicJsonReader {
 public:
  static_assert(MaxTokens > 0 && MaxTokens < 65535, "token indices are 16-bit");
  static constexpr int kMaxTokens = MaxTokens;
  static constexpr int kMaxDepth = 8;
  static constexpr std::size_t kMaxInput = 8192;

  enum class Type : std::uint8_t { Object, Array, String, Number, True, False, Null };

  // Validate and tokenize `text` (not necessarily NUL-terminated; must outlive the
  // reader). Returns false on any syntax error or exceeded limit; no accessor is
  // meaningful then.
  bool parse(const char* text, std::size_t len) noexcept {
    text_ = text;
    len_ = len;
    n_ = 0;
    pos_ = 0;
    valid_ = false;
    if (text == nullptr || len == 0 || len > kMaxInput) return false;
    if (!value(0)) return false;
    ws();
    if (pos_ != len_) return false;
    valid_ = true;
    return true;
  }

  bool valid() const noexcept { return valid_; }
  int root() const noexcept { return valid_ ? 0 : -1; }
  Type type(int t) const noexcept { return tok_[t].type; }
  bool is(int t, Type ty) const noexcept { return t >= 0 && t < n_ && tok_[t].type == ty; }

  // Member `key` of object `obj`, or -1.
  int find(int obj, const char* key) const noexcept {
    if (!is(obj, Type::Object)) return -1;
    int i = obj + 1;
    for (int p = 0; p < tok_[obj].count; ++p) {
      const int v = i + 1;
      if (str_eq(i, key)) return v;
      i = tok_[v].next;
    }
    return -1;
  }
  int count(int t) const noexcept { return (t >= 0 && t < n_) ? tok_[t].count : 0; }
  // i-th element of array `arr`, or -1.
  int at(int arr, int i) const noexcept {
    if (!is(arr, Type::Array) || i < 0 || i >= tok_[arr].count) return -1;
    int idx = arr + 1;
    for (int k = 0; k < i; ++k) idx = tok_[idx].next;
    return idx;
  }

  bool string_equals(int t, const char* s) const noexcept { return is(t, Type::String) && str_eq(t, s); }

  // Decode a string token (escapes, \uXXXX) into `out` as UTF-8, NUL-terminated.
  // False if it is not a string, does not fit, or has an unpaired surrogate.
  bool get_string(int t, char* out, std::size_t cap) const noexcept {
    if (!is(t, Type::String) || cap == 0) return false;
    std::size_t o = 0;
    const char* p = text_ + tok_[t].start;
    const char* e = text_ + tok_[t].end;
    auto emit = [&](unsigned char c) {
      if (o + 1 >= cap) return false;
      out[o++] = static_cast<char>(c);
      return true;
    };
    while (p < e) {
      if (*p != '\\') {
        if (!emit(static_cast<unsigned char>(*p++))) return false;
        continue;
      }
      ++p;  // validated at parse time: a valid escape follows
      switch (*p++) {
        case '"': if (!emit('"')) return false; break;
        case '\\': if (!emit('\\')) return false; break;
        case '/': if (!emit('/')) return false; break;
        case 'b': if (!emit('\b')) return false; break;
        case 'f': if (!emit('\f')) return false; break;
        case 'n': if (!emit('\n')) return false; break;
        case 'r': if (!emit('\r')) return false; break;
        case 't': if (!emit('\t')) return false; break;
        case 'u': {
          std::uint32_t cp = hex4(p);
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (e - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
            const std::uint32_t lo = hex4(p + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;  // lone low surrogate
          }
          if (cp < 0x80) {
            if (!emit(static_cast<unsigned char>(cp))) return false;
          } else if (cp < 0x800) {
            if (!emit(static_cast<unsigned char>(0xC0 | (cp >> 6))) ||
                !emit(static_cast<unsigned char>(0x80 | (cp & 0x3F)))) return false;
          } else if (cp < 0x10000) {
            if (!emit(static_cast<unsigned char>(0xE0 | (cp >> 12))) ||
                !emit(static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F))) ||
                !emit(static_cast<unsigned char>(0x80 | (cp & 0x3F)))) return false;
          } else {
            if (!emit(static_cast<unsigned char>(0xF0 | (cp >> 18))) ||
                !emit(static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F))) ||
                !emit(static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F))) ||
                !emit(static_cast<unsigned char>(0x80 | (cp & 0x3F)))) return false;
          }
          break;
        }
        default: return false;
      }
    }
    out[o] = '\0';
    return true;
  }

  // An integer literal (no fraction, no exponent) that fits in int64.
  bool get_i64(int t, std::int64_t& out) const noexcept {
    if (!is(t, Type::Number)) return false;
    const char* p = text_ + tok_[t].start;
    const char* e = text_ + tok_[t].end;
    bool neg = false;
    if (p < e && *p == '-') {
      neg = true;
      ++p;
    }
    if (p == e) return false;
    std::uint64_t v = 0;
    for (; p < e; ++p) {
      if (*p < '0' || *p > '9') return false;  // '.', 'e', 'E': not an integer literal
      const std::uint64_t d = static_cast<std::uint64_t>(*p - '0');
      if (v > (UINT64_MAX - d) / 10) return false;
      v = v * 10 + d;
    }
    if (neg) {
      if (v > static_cast<std::uint64_t>(INT64_MAX) + 1) return false;
      out = static_cast<std::int64_t>(0 - v);
    } else {
      if (v > static_cast<std::uint64_t>(INT64_MAX)) return false;
      out = static_cast<std::int64_t>(v);
    }
    return true;
  }
  bool get_double(int t, double& out) const noexcept {
    if (!is(t, Type::Number)) return false;
    const std::size_t n = tok_[t].end - tok_[t].start;
    if (n == 0 || n >= 40) return false;
    char tmp[40];
    std::memcpy(tmp, text_ + tok_[t].start, n);
    tmp[n] = '\0';
    out = std::strtod(tmp, nullptr);
    return std::isfinite(out);
  }
  bool get_bool(int t, bool& out) const noexcept {
    if (is(t, Type::True)) { out = true; return true; }
    if (is(t, Type::False)) { out = false; return true; }
    return false;
  }

 private:
  struct Token {
    Type type;
    std::uint16_t start, end;  // byte span (strings: inside the quotes)
    std::uint16_t next;        // index of the first token after this subtree
    std::uint16_t count;       // object: pairs, array: items
  };

  bool str_eq(int t, const char* s) const noexcept {
    // Compare the raw span; keys we look up never contain escapes.
    const std::size_t n = tok_[t].end - tok_[t].start;
    return tok_[t].type == Type::String && std::strlen(s) == n &&
           std::memcmp(text_ + tok_[t].start, s, n) == 0;
  }
  static std::uint32_t hex4(const char* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = p[i];
      v = v * 16 + static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
  }
  static bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
  }
  void ws() noexcept {
    while (pos_ < len_ && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_;
  }
  int alloc(Type ty, std::size_t start) noexcept {
    if (n_ >= kMaxTokens) return -1;
    tok_[n_] = Token{ty, static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(start), 0, 0};
    return n_++;
  }

  bool value(int depth) noexcept {
    if (depth > kMaxDepth) return false;
    ws();
    if (pos_ >= len_) return false;
    const char c = text_[pos_];
    if (c == '{') return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') return string_tok();
    if (c == 't') return literal("true", Type::True);
    if (c == 'f') return literal("false", Type::False);
    if (c == 'n') return literal("null", Type::Null);
    return number();
  }

  bool object(int depth) noexcept {
    const int me = alloc(Type::Object, pos_);
    if (me < 0) return false;
    ++pos_;  // '{'
    ws();
    if (pos_ < len_ && text_[pos_] == '}') {
      ++pos_;
      tok_[me].next = static_cast<std::uint16_t>(n_);
      return true;
    }
    for (;;) {
      ws();
      if (pos_ >= len_ || text_[pos_] != '"' || !string_tok()) return false;
      ws();
      if (pos_ >= len_ || text_[pos_] != ':') return false;
      ++pos_;
      if (!value(depth + 1)) return false;
      ++tok_[me].count;
      ws();
      if (pos_ >= len_) return false;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        tok_[me].next = static_cast<std::uint16_t>(n_);
        return true;
      }
      return false;
    }
  }

  bool array(int depth) noexcept {
    const int me = alloc(Type::Array, pos_);
    if (me < 0) return false;
    ++pos_;  // '['
    ws();
    if (pos_ < len_ && text_[pos_] == ']') {
      ++pos_;
      tok_[me].next = static_cast<std::uint16_t>(n_);
      return true;
    }
    for (;;) {
      if (!value(depth + 1)) return false;
      ++tok_[me].count;
      ws();
      if (pos_ >= len_) return false;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        tok_[me].next = static_cast<std::uint16_t>(n_);
        return true;
      }
      return false;
    }
  }

  bool string_tok() noexcept {
    ++pos_;  // opening quote
    const std::size_t start = pos_;
    while (pos_ < len_) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        const int t = alloc(Type::String, start);
        if (t < 0) return false;
        tok_[t].end = static_cast<std::uint16_t>(pos_);
        tok_[t].next = static_cast<std::uint16_t>(n_);
        ++pos_;
        return true;
      }
      if (c < 0x20) return false;
      if (c == '\\') {
        if (pos_ + 1 >= len_) return false;
        const char e = text_[pos_ + 1];
        if (e == 'u') {
          if (pos_ + 5 >= len_) return false;
          for (int i = 2; i < 6; ++i) if (!is_hex(text_[pos_ + static_cast<std::size_t>(i)])) return false;
          pos_ += 6;
          continue;
        }
        if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' && e != 'r' && e != 't') return false;
        pos_ += 2;
        continue;
      }
      ++pos_;
    }
    return false;  // unterminated
  }

  bool literal(const char* word, Type ty) noexcept {
    const std::size_t n = std::strlen(word);
    if (len_ - pos_ < n || std::memcmp(text_ + pos_, word, n) != 0) return false;
    const int t = alloc(ty, pos_);
    if (t < 0) return false;
    pos_ += n;
    tok_[t].end = static_cast<std::uint16_t>(pos_);
    tok_[t].next = static_cast<std::uint16_t>(n_);
    return true;
  }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  bool number() noexcept {
    const std::size_t start = pos_;
    auto digits = [&]() {
      std::size_t n = 0;
      while (pos_ < len_ && text_[pos_] >= '0' && text_[pos_] <= '9') { ++pos_; ++n; }
      return n;
    };
    if (pos_ < len_ && text_[pos_] == '-') ++pos_;
    if (pos_ >= len_) return false;
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      digits();
    } else {
      return false;
    }
    if (pos_ < len_ && text_[pos_] == '.') {
      ++pos_;
      if (digits() == 0) return false;
    }
    if (pos_ < len_ && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < len_ && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (digits() == 0) return false;
    }
    const int t = alloc(Type::Number, start);
    if (t < 0) return false;
    tok_[t].end = static_cast<std::uint16_t>(pos_);
    tok_[t].next = static_cast<std::uint16_t>(n_);
    return true;
  }

  const char* text_ = nullptr;
  std::size_t len_ = 0;
  std::size_t pos_ = 0;
  int n_ = 0;
  bool valid_ = false;
  Token tok_[static_cast<std::size_t>(MaxTokens)] = {};
};

// The size used on the brain for client messages (small). Tests that read the hub's
// own, larger frames use BasicJsonReader<N> with a bigger budget.
using JsonReader = BasicJsonReader<96>;

}  // namespace core
