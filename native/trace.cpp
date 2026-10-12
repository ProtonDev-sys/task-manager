#include "core.hpp"
#include <evntrace.h>
#include <evntcons.h>

namespace taskmgr {
// Per-process network bytes from the kernel TCP/IP and UDP/IP events. A private system-logger session needs elevation,
// like Windows' own Task Manager; without it the Network column reports that attribution is unavailable.
static constexpr GUID TcpIp{0x9a280ac0, 0xc8e0, 0x11d1, {0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2}};
static constexpr GUID UdpIp{0xbf3a50c5, 0xa9c9, 0x4988, {0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80}};
struct NetworkEvent { DWORD pid, bytes; uint64_t timestamp; };
static std::optional<NetworkEvent> networkEvent(const EVENT_RECORD& event) {
  const auto& provider = event.EventHeader.ProviderId; const auto opcode = event.EventHeader.EventDescriptor.Opcode;
  if ((provider != TcpIp && provider != UdpIp) || (opcode != 10 && opcode != 11 && opcode != 26 && opcode != 27) || event.EventHeader.EventDescriptor.Version < 1 || !event.UserData || event.UserDataLength < 8 || event.EventHeader.TimeStamp.QuadPart <= 0) return std::nullopt;
  DWORD values[2]{}; memcpy(values, event.UserData, sizeof(values));
  return NetworkEvent{values[0], values[1], uint64_t(event.EventHeader.TimeStamp.QuadPart)};
}
struct TraceCounts {
  struct Counter { uint64_t created = 0, bytes = 0, seen = 0; };
  std::unordered_map<DWORD, Counter> counters;
  Time sampledAt{};
  uint64_t generation = 0;
  void record(const NetworkEvent& event) {
    const auto found = counters.find(event.pid);
    // ProcessTrace converts QPC event timestamps to FILETIME unless RAW_TIMESTAMP is set.
    // Late delivery from an old PID must not count against its new owner's creation time.
    if (found == counters.end() || !found->second.created || event.timestamp < found->second.created) return;
    auto& bytes = found->second.bytes;
    bytes += std::min<uint64_t>(event.bytes, UINT64_MAX - bytes);
  }
  void sample(std::span<Process> processes, Time now) {
    const double elapsed = sampledAt == Time{} ? 0 : std::chrono::duration<double>(now - sampledAt).count();
    ++generation;
    for (auto& process : processes) {
      auto [found, inserted] = counters.try_emplace(process.id);
      const auto& counter = found->second;
      process.network = !inserted && counter.created == process.created && elapsed > 0 ? double(counter.bytes) / elapsed : 0;
      // A new identity establishes a baseline; any bytes mixed with its old owner are discarded.
      found->second = {process.created, 0, generation};
    }
    std::erase_if(counters, [&](const auto& item) { return item.second.seen != generation; });
    sampledAt = now;
  }
};
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> traceSamplingTests() {
  std::vector<std::pair<std::string, bool>> checks;
  {
    EVENT_RECORD event{}; DWORD payload[2]{10, 4096}; event.EventHeader.ProviderId = TcpIp; event.EventHeader.EventDescriptor.Opcode = 10; event.EventHeader.EventDescriptor.Version = 2;
    event.EventHeader.TimeStamp.QuadPart = 1000; event.UserData = payload; event.UserDataLength = sizeof(payload);
    const auto parsed = networkEvent(event);
    checks.emplace_back("trace_reads_payload_process_identity_and_bytes", parsed && parsed->pid == 10 && parsed->bytes == 4096 && parsed->timestamp == 1000);
    bool supported = true;
    for (const auto provider : {TcpIp, UdpIp}) for (const auto opcode : std::array<UCHAR, 4>{10, 11, 26, 27}) { event.EventHeader.ProviderId = provider; event.EventHeader.EventDescriptor.Opcode = opcode; supported = supported && networkEvent(event).has_value(); }
    checks.emplace_back("trace_supports_tcp_udp_ipv4_ipv6", supported);
    event.EventHeader.EventDescriptor.Opcode = 12;
    checks.emplace_back("trace_ignores_connection_events", !networkEvent(event)); event.EventHeader.EventDescriptor.Opcode = 10;
    event.EventHeader.ProviderId = {};
    checks.emplace_back("trace_ignores_unrelated_providers", !networkEvent(event)); event.EventHeader.ProviderId = TcpIp;
    event.EventHeader.EventDescriptor.Version = 0;
    checks.emplace_back("trace_rejects_legacy_incompatible_payload", !networkEvent(event)); event.EventHeader.EventDescriptor.Version = 2;
    event.UserDataLength = 7;
    checks.emplace_back("trace_rejects_truncated_payload", !networkEvent(event)); event.UserDataLength = sizeof(payload);
    event.UserData = nullptr;
    checks.emplace_back("trace_rejects_missing_payload", !networkEvent(event)); event.UserData = payload;
    event.EventHeader.TimeStamp.QuadPart = 0;
    checks.emplace_back("trace_rejects_missing_timestamp", !networkEvent(event));
  }
  {
    TraceCounts trace; std::vector<Process> processes(1); processes[0].id = 10; processes[0].created = 100;
    const Time initial = Time{} + std::chrono::seconds(1);
    trace.record({10, 200, 101}); trace.sample(processes, initial);
    checks.emplace_back("trace_new_identity_starts_with_baseline", processes[0].network == 0 && trace.counters.size() == 1);
    const auto* retained = &trace.counters.at(10);
    trace.record({10, 200, 101}); trace.record({10, 100, 102}); trace.sample(processes, initial + std::chrono::seconds(2));
    checks.emplace_back("trace_rates_use_actual_drain_interval", processes[0].network == 150);
    checks.emplace_back("trace_reuses_counter_nodes_between_samples", &trace.counters.at(10) == retained && trace.counters.at(10).bytes == 0);
    trace.record({10, 200, 103}); processes[0].created = 1000; trace.sample(processes, initial + std::chrono::seconds(3));
    checks.emplace_back("trace_reused_pid_discards_old_owner_traffic", processes[0].network == 0 && trace.counters.at(10).created == 1000);
    trace.record({10, 300, 999}); trace.record({10, 400, 1000}); trace.sample(processes, initial + std::chrono::seconds(4));
    checks.emplace_back("trace_late_old_pid_events_are_rejected", processes[0].network == 400);
    trace.record({20, 9999, 1001});
    checks.emplace_back("trace_unknown_process_events_do_not_allocate", trace.counters.size() == 1);
    trace.sample({}, initial + std::chrono::seconds(5)); trace.record({10, 9999, 1001});
    checks.emplace_back("trace_departed_process_counters_are_pruned", trace.counters.empty());
    trace.sample(processes, initial + std::chrono::seconds(6));
    checks.emplace_back("trace_reappearing_process_reestablishes_baseline", processes[0].network == 0);
    trace.counters.at(10).bytes = UINT64_MAX - 1; trace.record({10, 100, 1001});
    checks.emplace_back("trace_counter_overflow_saturates", trace.counters.at(10).bytes == UINT64_MAX);
    trace.sample(processes, initial);
    checks.emplace_back("trace_nonpositive_elapsed_discards_rate", processes[0].network == 0);
  }
  return checks;
}
#endif
struct NetworkTrace::State {
  std::wstring name;
  std::vector<std::byte> properties;
  TRACEHANDLE session = 0, consumer = INVALID_PROCESSTRACE_HANDLE;
  std::thread worker;
  std::atomic<bool> consuming = false;
  std::atomic<DWORD> status = ERROR_SUCCESS;
  std::mutex gate;
  TraceCounts counts;
  EVENT_TRACE_PROPERTIES* header() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data()); }
  bool initialize() {
    GUID id{}; wchar_t text[40]{};
    const auto created = CoCreateGuid(&id);
    if (FAILED(created)) { status = HRESULT_CODE(created); return false; }
    if (!StringFromGUID2(id, text, int(std::size(text)))) { status = ERROR_INVALID_DATA; return false; }
    name = L"TaskManagerNative-" + std::to_wstring(GetCurrentProcessId()) + L"-" + text;
    properties.assign(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t), std::byte{});
    auto value = header(); value->Wnode.BufferSize = ULONG(properties.size()); value->Wnode.Flags = WNODE_FLAG_TRACED_GUID; value->Wnode.ClientContext = 1; value->Wnode.Guid = id;
    value->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE; value->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP; value->FlushTimer = 1; value->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return true;
  }
  ~State() {
    if (session) ControlTraceW(session, nullptr, header(), EVENT_TRACE_CONTROL_STOP);
    if (consumer != INVALID_PROCESSTRACE_HANDLE) CloseTrace(consumer);
    if (worker.joinable()) worker.join();
  }
  static void WINAPI record(EVENT_RECORD* event) {
    const auto parsed = networkEvent(*event); if (!parsed || !event->UserContext) return;
    auto state = static_cast<State*>(event->UserContext); std::lock_guard lock(state->gate); state->counts.record(*parsed);
  }
};
NetworkTrace::NetworkTrace() : state(std::make_unique<State>()) {
  // Never stop a session by a reusable PID-based name: another live sampler may own it.
  if (!state->initialize()) return;
  const auto started = StartTraceW(&state->session, state->name.c_str(), state->header()); state->status = started;
  if (started != ERROR_SUCCESS) { state->session = 0; return; }
  EVENT_TRACE_LOGFILEW log{}; log.LoggerName = state->name.data(); log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD; log.EventRecordCallback = State::record; log.Context = state.get();
  state->consumer = OpenTraceW(&log);
  if (state->consumer == INVALID_PROCESSTRACE_HANDLE) { state->status = GetLastError(); ControlTraceW(state->session, nullptr, state->header(), EVENT_TRACE_CONTROL_STOP); state->session = 0; return; }
  state->consuming = true;
  state->worker = std::thread([current = state.get()] { TRACEHANDLE handles[] = {current->consumer}; current->status = ProcessTrace(handles, 1, nullptr, nullptr); current->consuming = false; });
}
NetworkTrace::~NetworkTrace() = default;
bool NetworkTrace::active() const { return state->consuming.load(); }
bool NetworkTrace::sample(std::span<Process> processes) {
  if (!active()) return false;
  std::lock_guard lock(state->gate);
  state->counts.sample(processes, Clock::now());
  return true;
}
#ifdef TASKMGR_DIAGNOSTICS
DWORD NetworkTrace::status() const { return state->status.load(); }
std::vector<std::pair<std::string, bool>> traceLifecycleTests() {
  std::vector<std::pair<std::string, bool>> checks;
  NetworkTrace first;
  if (!first.active()) {
    const auto status = first.status();
    if (status == ERROR_ACCESS_DENIED || status == ERROR_PRIVILEGE_NOT_HELD) checks.emplace_back("trace_native_lifecycle_skipped_permission", true);
    else if (status == ERROR_NOT_SUPPORTED || status == ERROR_NO_SYSTEM_RESOURCES) checks.emplace_back("trace_native_lifecycle_skipped_session_unavailable", true);
    else checks.emplace_back("trace_native_session_initializes", false);
    return checks;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool identified = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
  checks.emplace_back("trace_native_owned_process_identity", identified);
  std::array<Process, 1> processes; processes[0].id = GetCurrentProcessId(); processes[0].created = ticks(created);
  checks.emplace_back("trace_native_owned_process_baseline", first.sample(processes) && processes[0].network == 0);
  {
    NetworkTrace second;
    checks.emplace_back("trace_native_starting_second_session_preserves_first", first.sample(processes) && std::isfinite(processes[0].network) && processes[0].network >= 0);
    if (second.active()) checks.emplace_back("trace_native_second_session_samples_independently", second.sample(processes) && processes[0].network == 0);
    else {
      const auto status = second.status();
      checks.emplace_back("trace_native_second_session_unavailable_is_classified", status == ERROR_ACCESS_DENIED || status == ERROR_PRIVILEGE_NOT_HELD || status == ERROR_NOT_SUPPORTED || status == ERROR_NO_SYSTEM_RESOURCES);
    }
  }
  checks.emplace_back("trace_native_stopping_second_session_preserves_first", first.sample(processes) && std::isfinite(processes[0].network) && processes[0].network >= 0);
  return checks;
}
#endif
}
