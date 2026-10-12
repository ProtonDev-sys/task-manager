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
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace taskmgr {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
inline double milliseconds(Time start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
inline int adaptiveRefreshInterval(double samplingCost, double displayCost) {
  if (!std::isfinite(samplingCost) || !std::isfinite(displayCost) || samplingCost < 0 || displayCost < 0) return 4000;
  return int(std::clamp(std::ceil((samplingCost + displayCost) * 50), 250.0, 4000.0));
}
inline uint64_t ticks(FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
inline std::wstring lower(std::wstring value) { std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return wchar_t(towlower(ch)); }); return value; }
inline bool startsWithInsensitive(std::wstring_view value, std::wstring_view prefix) { return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin(), [](wchar_t first, wchar_t second) { return towlower(first) == towlower(second); }); }
std::wstring number(double value, int precision = 1);
std::wstring hexadecimal(uint64_t value);
std::optional<uint32_t> counterInteger(std::wstring_view text, unsigned base);
std::vector<std::wstring_view> splitFields(std::wstring_view text, wchar_t delimiter);
std::wstring grouped(double value, int precision = 0);
std::wstring bytes(double value);
std::wstring bits(double value);
std::wstring duration(uint64_t seconds);
std::wstring winerror(DWORD code = GetLastError());
std::string utf8(const std::wstring& value);
std::wstring fromUtf8(std::string_view value);
std::wstring executable();
std::wstring windowsDirectory();
std::wstring commandExecutable(const std::wstring& command);
struct RunCommand { std::wstring file, parameters; };
RunCommand parseRunCommand(const std::wstring& command);
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE handle = nullptr) : value(handle) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct GpuEngineUsage { std::wstring key, label; double value = 0; };
struct ProcessGpu { double value = 0; std::wstring engine; std::vector<GpuEngineUsage> engines; };
struct Process {
  DWORD id = 0, parent = 0, session = 0, threads = 0, handles = 0, pageFaults = 0;
  LONG priority = 8;
  uint64_t created = 0, cpuTicks = 0, working = 0, privateWorking = 0, privateBytes = 0, peakWorking = 0, readBytes = 0, writeBytes = 0;
  double cpu = 0, ioRate = 0, gpu = 0, network = -1;
  std::wstring name, gpuEngine;
  // Share immutable engine data between process samples until the next resource refresh.
  std::shared_ptr<const ProcessGpu> gpuUsage;
  bool app = false, suspended = false, hung = false;
};
struct Metadata { std::wstring path, description, publisher, user, commandLine, package, virtualization; int icon = 0; bool resolved = false; };
struct FileInfo { std::wstring description, publisher; int icon = -1; };
struct Identity { DWORD pid; uint64_t created; bool operator==(const Identity&) const = default; };
struct IdentityHash { size_t operator()(const Identity& key) const { return std::hash<uint64_t>{}(key.created) ^ (std::hash<DWORD>{}(key.pid) << 1); } };
using ProcessGpuCache = std::unordered_map<Identity, std::shared_ptr<const ProcessGpu>, IdentityHash>;
struct Entry { std::wstring key; std::vector<std::wstring> cells; DWORD pid = 0, session = 0; bool enabled = false, startupEditable = false; std::wstring path, location, group, description, accountDomain; };
struct StartupInventory { std::vector<Entry> entries; std::vector<std::wstring> warnings; bool complete = true; };
struct ServiceInventory { std::vector<Entry> services, sessions; DWORD servicesError = ERROR_SUCCESS; };
struct CpuInfo { std::wstring name; double baseMhz = 0; unsigned sockets = 0, cores = 0, logical = 1; std::array<uint64_t, 3> caches{}; bool virtualization = false; };
struct MemoryInfo { uint64_t total = 0, available = 0, committed = 0, commitLimit = 0, cached = 0, paged = 0, nonpaged = 0, compressed = 0, installed = 0; unsigned speed = 0, slots = 0, usedSlots = 0; std::wstring formFactor; };
struct DiskInfo { std::wstring key, title, model, type, letters; int index = 0; double active = 0, read = 0, write = 0, response = 0; uint64_t capacity = 0, formatted = 0; bool system = false, pagefile = false; };
struct NetworkInfo { ULONG64 luid = 0; std::wstring type, alias, description, ipv4, ipv6; double send = 0, receive = 0; uint64_t speed = 0; };
struct GpuInfo { std::wstring key, name, driverVersion, driverDate, location; int index = 0; double usage = 0, temperature = -1; std::vector<std::pair<std::wstring, double>> engines; uint64_t dedicated = 0, dedicatedLimit = 0, shared = 0, sharedLimit = 0; };
struct AppWindow { HWND window = nullptr; DWORD pid = 0; std::wstring title; };
struct Sample {
  std::vector<Process> processes;
  std::shared_ptr<const ServiceInventory> inventory;
  std::span<const Entry> services, sessions;
  std::shared_ptr<const StartupInventory> startup;
  std::shared_ptr<const std::unordered_map<DWORD, std::wstring>> users;
  double cpu = 0, cpuSpeed = 0, interrupts = 0, elapsed = 0;
  Time resourcesSampledAt{};
  std::vector<double> cores;
#ifdef TASKMGR_DIAGNOSTICS
  double duration = 0;
  std::array<double, 6> stages{};
  bool cpuTopologyChanged = false;
  Time resourcesCheckedAt{}, inventoryCheckedAt{}, inventorySampledAt{};
#endif
  uint64_t uptime = 0;
  unsigned threads = 0, handles = 0;
  CpuInfo cpuInfo;
  MemoryInfo memory;
  std::vector<DiskInfo> disks;
  std::vector<NetworkInfo> networks;
  std::vector<GpuInfo> gpus;
  std::vector<AppWindow> windows;
  bool networkAttribution = false;
};
class Counter {
  PDH_HQUERY query = nullptr;
  std::unordered_map<std::wstring, PDH_HCOUNTER> counters;
  std::vector<std::byte> valuesBuffer;
public:
  Counter();
  ~Counter();
  void add(const std::wstring& key, const std::wstring& path);
  void collect();
  std::vector<std::pair<std::wstring, double>> values(const std::wstring& key);
};
class NetworkTrace {
  struct State;
  std::unique_ptr<State> state;
public:
  NetworkTrace();
  ~NetworkTrace();
  bool active() const;
  bool sample(std::span<Process> processes);
#ifdef TASKMGR_DIAGNOSTICS
  DWORD status() const;
#endif
};
class Sampler {
  std::vector<std::byte> buffer;
  struct Previous { uint64_t created = 0, cpuTicks = 0, readBytes = 0, writeBytes = 0, seen = 0; Time sampledAt{}; };
  std::unordered_map<DWORD, Previous> previous;
  uint64_t generation = 0;
  Time last{};
  Sample cachedResources;
  ProcessGpuCache cachedProcessGpu;
  Time resourcesAt{};
  uint64_t idle = 0, total = 0;
  double cpuUsage = 0;
  std::vector<unsigned> processorGroups;
  std::vector<std::array<uint64_t, 3>> corePrevious;
  CpuInfo cpuInfo;
  MemoryInfo memoryHardware;
  Counter counters;
  std::shared_ptr<const ServiceInventory> cachedInventory;
  std::shared_ptr<const StartupInventory> cachedStartup = std::make_shared<StartupInventory>();
  std::mutex startupMutex;
  std::shared_ptr<const StartupInventory> startupProgress;
  std::atomic<bool> startupProgressReady = false;
  std::function<void()> startupNotify;
  std::future<StartupInventory> startupJob;
  Time startupAt{};
  bool startupInvalidated = true;
  std::shared_ptr<const std::unordered_map<DWORD, std::wstring>> cachedUsers;
  std::unordered_map<std::wstring, std::wstring> accounts;
  Time inventoryAt{};
  Time networkAt{};
  std::unordered_map<ULONG64, std::pair<ULONG64, ULONG64>> networkPrevious;
  std::unordered_map<ULONG64, std::array<std::wstring, 3>> adapterAddresses;
  std::unordered_map<int, DiskInfo> diskHardware;
  std::vector<GpuInfo> gpuDescriptors;
  bool hardwareInvalidated = false;
  std::unique_ptr<NetworkTrace> trace;
  void readUsers();
public:
  explicit Sampler(bool attribution = false, std::function<void()> startupNotification = {});
  ~Sampler();
  Sample sample(bool startupVisible = false);
  bool startupReady() const { return startupProgressReady.load() || (startupJob.valid() && startupJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready); }
  bool startupPending() const { return startupJob.valid(); }
  void invalidateInventory() { inventoryAt = {}; resourcesAt = {}; startupInvalidated = hardwareInvalidated = true; }
};
CpuInfo readCpu();
MemoryInfo readMemoryHardware();
DiskInfo readDiskHardware(int index);
std::vector<GpuInfo> readGpus();
double gpuTemperature(const std::wstring& key);
double lastBiosSeconds();
std::wstring serviceGroupCaption(const std::wstring& group);
std::vector<Entry> readServices(DWORD* error = nullptr);
std::vector<Entry> readSessions();
bool sameSessionAccount(const Entry& expected, const Entry& current);
StartupInventory readStartup(const std::function<void(const StartupInventory&)>& progress = {});
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> startupTests();
std::vector<std::pair<std::string, bool>> gpuEngineTests();
std::vector<std::pair<std::string, bool>> networkSamplingTests();
std::vector<std::pair<std::string, bool>> counterSamplingTests();
std::vector<std::pair<std::string, bool>> processSamplingTests();
std::vector<std::pair<std::string, bool>> traceSamplingTests();
std::vector<std::pair<std::string, bool>> traceLifecycleTests();
std::vector<std::pair<std::string, bool>> iconCacheTests();
std::vector<std::pair<std::string, bool>> serviceInventoryTests();
std::vector<std::pair<std::string, bool>> memoryHardwareTests();
std::vector<std::pair<std::string, bool>> processorGroupTests();
std::vector<std::pair<std::string, bool>> accountNameTests();
std::vector<std::pair<std::string, bool>> sessionIdentityTests();
std::vector<std::pair<std::string, bool>> hardwareInventoryTests();
#endif
std::wstring accountName(const std::wstring& user, const std::wstring& domain);
std::wstring fullAccountName(const std::wstring& user, const std::wstring& domain);
class Icons {
  struct Request {
    DWORD pid = 0; uint64_t created = 0; std::wstring path;
#ifdef TASKMGR_DIAGNOSTICS
    Time queued = Clock::now();
#endif
  };
  struct Result {
    DWORD pid = 0; uint64_t created = 0; std::wstring path; Metadata metadata; HICON icon = nullptr;
#ifdef TASKMGR_DIAGNOSTICS
    double latency = 0; Time queued{};
#endif
    bool file = false;
  };
  HWND target;
  int size;
#ifdef TASKMGR_DIAGNOSTICS
  bool measured;
#endif
  mutable std::mutex mutex;
  std::condition_variable signal;
  bool stopping = false;
  std::deque<Request> requests;
  std::deque<Result> results;
  std::deque<Result> enrichment;
  std::unordered_map<std::wstring, HICON> extracted;
  std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> versionCache;
  std::unordered_set<Identity, IdentityHash> requested;
  std::unordered_set<std::wstring> requestedFiles;
  std::array<std::thread, 2> workers;
  std::thread enricher;
  std::unordered_map<Identity, Metadata, IdentityHash> entries;
  std::unordered_map<std::wstring, int> paths;
  std::unordered_map<std::wstring, FileInfo> files;
  void run();
  void enrich();
  HICON extract(const std::wstring& path);
  bool accepts(const Result& result) const { return result.file || requested.contains(Identity{result.pid, result.created}); }
public:
  static constexpr int DefaultIcon = 0, ServiceIcon = 1, ServiceHostIcon = 2, UserIcon = 3;
  HIMAGELIST images = nullptr;
#ifdef TASKMGR_DIAGNOSTICS
  std::vector<double> latencies;
  std::vector<double> displayLatencies;
  void setTestMetadata(const Process& process, Metadata metadata) { entries[{process.id, process.created}] = std::move(metadata); }
  Icons(HWND window, int iconSize, bool measure = false, bool startWorkers = true);
  friend std::vector<std::pair<std::string, bool>> iconCacheTests();
#else
  Icons(HWND window, int iconSize);
#endif
  ~Icons();
  void request(const Process& process, bool urgent = false);
  const FileInfo& file(const std::wstring& path, bool urgent = false);
  void consume();
  const Metadata& get(const Process& process) const;
  bool draw(HDC dc, int index, int left, int top, int pixels) const;
  template<class Live> void prune(const Live& live) {
    std::erase_if(entries, [&](const auto& item) { return !live.contains(item.first); });
    std::lock_guard lock(mutex);
    std::erase_if(requested, [&](const auto& item) { return !live.contains(item); });
    const auto obsolete = [&](const auto& work) { return work.pid != 0 && !live.contains(Identity{work.pid, work.created}); };
    std::erase_if(requests, obsolete);
    std::erase_if(enrichment, obsolete);
    std::erase_if(results, [&](const auto& result) { if (!obsolete(result)) return false; if (result.icon) DestroyIcon(result.icon); return true; });
  }
};
struct GraphPoint { double time = 0, value = 0, secondary = -1; };
class History {
  std::vector<GraphPoint> points;
  size_t first = 0, used = 0;
  GraphPoint& point(size_t index) { return points[(first + index) & (points.size() - 1)]; }
  const GraphPoint& point(size_t index) const { return points[(first + index) & (points.size() - 1)]; }
public:
  History() = default;
  History(const History&) = default;
  History& operator=(const History&) = default;
  History(History&& other) noexcept : points(std::move(other.points)), first(std::exchange(other.first, 0)), used(std::exchange(other.used, 0)) {}
  History& operator=(History&& other) noexcept { if (this != &other) { points = std::move(other.points); first = std::exchange(other.first, 0); used = std::exchange(other.used, 0); } return *this; }
  void add(double time, double value, double secondary = -1);
  std::vector<GraphPoint> window(double now, double duration = 60) const;
  double peak(double now, double duration = 60) const;
  size_t size() const { return used; }
  bool recent(double now) const { return used && point(used - 1).time >= now - 60; }
};
struct GraphStyle { COLORREF color = RGB(17, 125, 187); bool grid = true, scrolling = true, secondary = false; };
void drawGraph(HDC dc, RECT bounds, const History& history, double now, double maximum, const GraphStyle& style);
enum class Action { End, Realtime, High, AboveNormal, Normal, BelowNormal, Idle, Dump, Affinity };
struct PowerPolicy { DWORD priority = 0, control = 0, state = 0; };
PowerPolicy powerPolicy(const Process& process);
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> powerPolicyTests();
#endif
void efficiencyAction(const Process& process, bool enabled);
void processAction(const Process& process, Action action, const std::wstring& argument = L"");
std::vector<Identity> processTree(std::span<const Process> processes, const Process& root);
std::vector<Identity> processTree(std::span<const Process> processes, std::span<const Identity> roots);
std::wstring waitChain(const Process& process);
DWORD serviceActionAccess(int action);
void serviceAction(const Entry& entry, int action);
void startupAction(const Entry& entry);
void sessionAction(const Entry& entry, bool logoff);
constexpr UINT SampleMessage = WM_APP + 1, IconMessage = WM_APP + 2, ActionMessage = WM_APP + 3, TrayMessage = WM_APP + 4;
bool replacementEnabled();
void requestReplacement(HWND owner, bool enabled);
int configureReplacement(bool enabled);
#ifdef TASKMGR_DIAGNOSTICS
constexpr UINT ProbeMessage = WM_APP + 5;
int diagnostics(const std::vector<std::wstring>& arguments);
struct RunOptions { bool hidden = false; int benchmarkSeconds = 0; std::wstring output; int interval = -1, tab = -1, theme = -1; bool minimized = false, idle = false, sortSpam = false, tabSpam = false; int warmup = 0; std::wstring screenshots; };
int runApplication(const RunOptions& options = {});
#else
int runApplication();
#endif
}
