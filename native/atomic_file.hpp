#pragma once
#include "core.hpp"

namespace taskmgr {
// Stage a complete replacement beside its destination. Member destruction
// closes the handle before removing a temporary file that this writer owns.
class AtomicFile {
  struct Temporary {
    std::wstring path;
    bool owned = false;
    ~Temporary() { if (owned) DeleteFileW(path.c_str()); }
  };
  std::wstring destination;
  Temporary temporary;
  Handle file;
  std::array<char, 64 * 1024> buffer;
  size_t buffered = 0;
  DWORD failure = ERROR_SUCCESS;

  static std::wstring temporaryName(const std::wstring& path) {
    GUID guid{}; wchar_t identifier[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, identifier, int(std::size(identifier))))
      throw std::runtime_error("Unable to create a temporary file name.");
    // A staging suffix must not push a valid destination filename past the
    // filesystem's component limit. Keep the GUID basename independent of it.
    return (std::filesystem::path(path).parent_path() / (std::wstring(L".taskmgr-") + identifier + L".tmp")).wstring();
  }
  [[noreturn]] void fail(DWORD error) {
    if (!failure) failure = error ? error : ERROR_WRITE_FAULT;
    throw std::runtime_error(utf8(winerror(failure)));
  }
  void writable() {
    if (failure) fail(failure);
    if (!file.value || file.value == INVALID_HANDLE_VALUE) fail(ERROR_INVALID_HANDLE);
  }
  void write(std::string_view contents) {
    while (!contents.empty()) {
      const DWORD count = DWORD(std::min<size_t>(contents.size(), 1024 * 1024)); DWORD written = 0;
      if (!WriteFile(file.value, contents.data(), count, &written, nullptr)) fail(GetLastError());
      if (!written || written > count) fail(ERROR_WRITE_FAULT);
      contents.remove_prefix(written);
    }
  }
  void flush() {
    if (buffered) { write({buffer.data(), buffered}); buffered = 0; }
  }
public:
  explicit AtomicFile(const std::wstring& path)
    : destination(path), temporary{temporaryName(path)},
      file(CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) {
    if (file.value == INVALID_HANDLE_VALUE) fail(GetLastError());
    // CREATE_NEW failure, including a name collision, never grants ownership.
    temporary.owned = true;
  }
  AtomicFile(const AtomicFile&) = delete;
  AtomicFile& operator=(const AtomicFile&) = delete;
  void append(std::string_view contents) {
    writable();
    if (contents.empty()) return;
    // Large CSV snapshots and metadata fields bypass the small-write buffer.
    if (contents.size() >= buffer.size()) { flush(); write(contents); return; }
    if (contents.size() > buffer.size() - buffered) flush();
    std::memcpy(buffer.data() + buffered, contents.data(), contents.size()); buffered += contents.size();
  }
  void commit() {
    writable(); flush();
    if (!FlushFileBuffers(file.value)) fail(GetLastError());
    if (!CloseHandle(file.value)) fail(GetLastError());
    file.value = nullptr;
    if (!MoveFileExW(temporary.path.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) fail(GetLastError());
    temporary.owned = false;
  }
};
}
