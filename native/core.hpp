#pragma once
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace taskmgr {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
inline double milliseconds(Time start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
inline uint64_t ticks(FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
inline std::wstring lower(std::wstring value) { std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return wchar_t(towlower(ch)); }); return value; }
inline bool contains(const std::wstring& value, const std::wstring& query) { return query.empty() || lower(value).find(query) != std::wstring::npos; }
std::wstring number(double value, int precision = 1);
std::wstring bytes(double value);
std::wstring winerror(DWORD code = GetLastError());
std::string utf8(const std::wstring& value);
std::wstring executable();
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE handle = nullptr) : value(handle) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct Process {
  DWORD id = 0, parent = 0, session = 0, threads = 0, handles = 0;
  uint64_t created = 0, cpuTicks = 0, working = 0, privateBytes = 0, ioBytes = 0;
  double cpu = 0, ioRate = 0, gpu = 0;
  std::wstring name;
  bool app = false;
};
struct Metadata { std::wstring path, description, publisher, user; int icon = 0; };
struct Identity { DWORD pid; uint64_t created; bool operator==(const Identity&) const = default; };
struct IdentityHash { size_t operator()(const Identity& key) const { return std::hash<uint64_t>{}(key.created) ^ (std::hash<DWORD>{}(key.pid) << 1); } };
struct Resource { std::wstring key, name, detail, unit; double value = 0, maximum = 100, secondary = 0; COLORREF color = RGB(0, 120, 215); };
struct Entry { std::wstring key; std::vector<std::wstring> cells; DWORD pid = 0, session = 0; bool enabled = false; std::wstring path, location; };
struct Sample {
  Time timestamp = Clock::now();
  std::vector<Process> processes;
  std::vector<Resource> resources;
  std::vector<Entry> services, sessions, startup;
  double cpu = 0, elapsed = 0, duration = 0;
  std::array<double, 6> stages{};
  uint64_t memoryTotal = 0, memoryUsed = 0;
  PERFORMANCE_INFORMATION memory{};
  unsigned logical = 1;
  std::wstring cpuName;
};
class Counter {
  PDH_HQUERY query = nullptr;
  std::unordered_map<std::wstring, PDH_HCOUNTER> counters;
public:
  Counter();
  ~Counter();
  void add(const std::wstring& key, const std::wstring& path);
  void collect();
  std::vector<std::pair<std::wstring, double>> values(const std::wstring& key) const;
};
class Sampler {
  std::vector<std::byte> buffer;
  std::unordered_map<DWORD, Process> previous;
  Time last{};
  uint64_t idle = 0, total = 0;
  unsigned logical = 1;
  Counter counters;
  std::vector<Entry> cachedServices, cachedSessions, cachedStartup;
  Time inventoryAt{};
  std::wstring cpuName;
  std::unordered_map<ULONG64, std::pair<ULONG64, ULONG64>> networkPrevious;
  std::vector<Resource> gpuDescriptors;
public:
  Sampler();
  Sample sample();
  void invalidateInventory() { inventoryAt = {}; }
};
std::vector<Entry> readServices();
std::vector<Entry> readSessions();
std::vector<Entry> readStartup();
bool matches(const Process& process, const Metadata& metadata, const std::wstring& query);
int findNext(std::span<const std::wstring> names, std::wstring prefix, int start);
class Icons {
  struct Request { DWORD pid; uint64_t created; std::wstring name; Time queued = Clock::now(); };
  struct Result { DWORD pid; uint64_t created; Metadata metadata; HICON icon = nullptr; double latency = 0; Time queued{}; };
  HWND target;
  mutable std::mutex mutex;
  std::condition_variable signal;
  bool stopping = false;
  std::deque<Request> requests;
  std::deque<Result> results;
  std::deque<Result> enrichment;
  std::unordered_map<std::wstring, HICON> extracted;
  std::unordered_map<std::wstring, Metadata> versionCache;
  std::unordered_set<Identity, IdentityHash> requested;
  std::array<std::thread, 2> workers;
  std::thread enricher;
  std::unordered_map<Identity, Metadata, IdentityHash> entries;
  std::unordered_map<std::wstring, int> paths;
  void run();
  void enrich();
public:
  HIMAGELIST images = nullptr;
  std::vector<double> latencies;
  std::vector<double> displayLatencies;
  explicit Icons(HWND window);
  ~Icons();
  void request(const Process& process, bool urgent = false);
  void consume();
  const Metadata& get(const Process& process) const;
  size_t count() const { return paths.size(); }
  void prune(std::span<const Process> live);
};
struct GraphPoint { double time = 0, value = 0; };
class History {
  std::deque<GraphPoint> points;
public:
  void add(double time, double value);
  std::vector<GraphPoint> window(double now, double duration = 60) const;
  size_t size() const { return points.size(); }
};
void drawGraph(HDC dc, RECT bounds, const History& history, double now, double maximum, COLORREF color, bool grid = true);
enum class Action { End, Idle, Normal, AboveNormal, High, EfficiencyOn, EfficiencyOff, Dump, WaitChain, Affinity };
void processAction(const Process& process, Action action, const std::wstring& argument = L"");
std::wstring waitChain(const Process& process);
void serviceAction(const Entry& entry, int action);
void startupAction(const Entry& entry);
void sessionAction(const Entry& entry, bool logoff);
constexpr UINT SampleMessage = WM_APP + 1, IconMessage = WM_APP + 2, ActionMessage = WM_APP + 3;
int diagnostics(const std::vector<std::wstring>& arguments);
int runApplication(bool hidden = false, int benchmarkSeconds = 0, const std::wstring& output = L"", int intervalOverride = -1, int tab = -1, bool minimized = false, bool idle = false, int warmup = 0);
}
