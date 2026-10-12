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
  bool record(const NetworkEvent& event) {
    const auto found = counters.find(event.pid);
    // ProcessTrace converts QPC event timestamps to FILETIME unless RAW_TIMESTAMP is set.
    // Late delivery from an old PID must not count against its new owner's creation time.
    if (found == counters.end() || !found->second.created || event.timestamp < found->second.created) return false;
    auto& bytes = found->second.bytes;
    bytes += std::min<uint64_t>(event.bytes, UINT64_MAX - bytes);
    return true;
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
    const bool rejected = !trace.record({10, 300, 999}), accepted = trace.record({10, 400, 1000}); trace.sample(processes, initial + std::chrono::seconds(4));
    checks.emplace_back("trace_late_old_pid_events_are_rejected", rejected && accepted && processes[0].network == 400);
    const bool untracked = !trace.record({20, 9999, 1001});
    checks.emplace_back("trace_unknown_process_events_do_not_allocate", untracked && trace.counters.size() == 1);
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
#ifdef TASKMGR_DIAGNOSTICS
  const DWORD diagnosticPid = GetCurrentProcessId();
  std::atomic<uint64_t> deliveredEvents = 0, decodedEvents = 0, ownUdpEvents = 0;
#endif
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
  template<bool measure>
  static void WINAPI record(EVENT_RECORD* event) {
    if (!event->UserContext) return;
    auto state = static_cast<State*>(event->UserContext);
#ifdef TASKMGR_DIAGNOSTICS
    if constexpr (measure) state->deliveredEvents.fetch_add(1);
#endif
    const auto parsed = networkEvent(*event); if (!parsed) return;
#ifdef TASKMGR_DIAGNOSTICS
    if constexpr (measure) state->decodedEvents.fetch_add(1);
#endif
    std::lock_guard lock(state->gate);
    [[maybe_unused]] const bool attributed = state->counts.record(*parsed);
#ifdef TASKMGR_DIAGNOSTICS
    if constexpr (measure) if (attributed && parsed->bytes && parsed->pid == state->diagnosticPid && event->EventHeader.ProviderId == UdpIp) state->ownUdpEvents.fetch_add(1);
#endif
  }
};
#ifdef TASKMGR_DIAGNOSTICS
NetworkTrace::NetworkTrace() : NetworkTrace(false) {}
NetworkTrace::NetworkTrace(bool measure) : state(std::make_unique<State>()) {
#else
NetworkTrace::NetworkTrace() : state(std::make_unique<State>()) {
#endif
  // Never stop a session by a reusable PID-based name: another live sampler may own it.
  if (!state->initialize()) return;
  const auto started = StartTraceW(&state->session, state->name.c_str(), state->header()); state->status = started;
  if (started != ERROR_SUCCESS) { state->session = 0; return; }
  EVENT_TRACE_LOGFILEW log{}; log.LoggerName = state->name.data(); log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD; log.Context = state.get();
#ifdef TASKMGR_DIAGNOSTICS
  // Compile out the counters from ordinary diagnostic traces used for performance comparisons.
  log.EventRecordCallback = measure ? State::record<true> : State::record<false>;
#else
  log.EventRecordCallback = State::record<false>;
#endif
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
NetworkSmokeResult networkTraceSmoke() {
  // Run separately from portable self-tests: successful sockets without attributed ETW events fail.
  // Nothing is sent outside the two loopback endpoints owned by this process.
  const auto began = Clock::now();
  NetworkSmokeResult result;
  NetworkTrace trace(true);
  auto finish = [&](std::string reason, bool passed = false, bool skipped = false) {
    result.reason = std::move(reason); result.passed = passed; result.skipped = skipped;
    if (result.traceStatus == ERROR_SUCCESS) result.traceStatus = trace.status();
    // Read in reverse pipeline order so these diagnostics remain consistent while callbacks finish.
    result.ownUdpEvents = trace.state->ownUdpEvents.load();
    result.decodedEvents = trace.state->decodedEvents.load();
    result.deliveredEvents = trace.state->deliveredEvents.load();
    if (passed && (result.traceStatus != ERROR_SUCCESS || !trace.active())) { result.passed = false; result.reason = "trace_consumer_stopped"; }
    result.elapsedMilliseconds = milliseconds(began);
    return result;
  };
  auto unavailable = [&] {
    result.traceStatus = trace.status();
    if (result.traceStatus == ERROR_ACCESS_DENIED || result.traceStatus == ERROR_PRIVILEGE_NOT_HELD) return finish("trace_permission", false, true);
    if (result.traceStatus == ERROR_NOT_SUPPORTED || result.traceStatus == ERROR_NO_SYSTEM_RESOURCES) return finish("trace_session_unavailable", false, true);
    return finish("trace_initialization_failed");
  };
  if (!trace.active()) return unavailable();
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return finish("owned_process_identity_failed_" + std::to_string(GetLastError()));
  if (!ticks(created)) return finish("owned_process_identity_missing");
  std::array<Process, 1> processes; processes[0].id = GetCurrentProcessId(); processes[0].created = ticks(created);
  if (!trace.sample(processes)) return unavailable();
  ++result.samples;
  if (processes[0].network != 0) return finish("owned_process_baseline_not_zero");

  WSADATA data{};
  const int initialized = WSAStartup(MAKEWORD(2, 2), &data);
  if (initialized != 0) { result.socketStatus = DWORD(initialized); return finish("winsock_initialization_failed"); }
  struct SocketRuntime { ~SocketRuntime() { WSACleanup(); } } runtime;
  struct Socket {
    SOCKET value = INVALID_SOCKET;
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
  } sender, receiver;
  auto socketFailure = [&](std::string reason) { result.socketStatus = DWORD(WSAGetLastError()); return finish(std::move(reason)); };
  auto bindLoopback = [&](Socket& owned, sockaddr_in& address) {
    owned.value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (owned.value == INVALID_SOCKET) return false;
    const BOOL exclusive = TRUE; u_long nonblocking = 1;
    if (setsockopt(owned.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
        ioctlsocket(owned.value, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(owned.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) return false;
    int size = sizeof(address);
    if (getsockname(owned.value, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR) return false;
    if (size != int(sizeof(address)) || address.sin_family != AF_INET || address.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || !address.sin_port) { WSASetLastError(WSAEINVAL); return false; }
    return true;
  };
  sockaddr_in source{}, destination{};
  if (!bindLoopback(sender, source) || !bindLoopback(receiver, destination)) return socketFailure("udp_loopback_bind_failed");
  // Both sockets bind exclusively to ephemeral loopback ports; connect filters unrelated datagrams.
  if (connect(sender.value, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == SOCKET_ERROR ||
      connect(receiver.value, reinterpret_cast<const sockaddr*>(&source), sizeof(source)) == SOCKET_ERROR) return socketFailure("udp_loopback_connect_failed");
  constexpr unsigned datagrams = 32;
  std::array<char, 1024> expected{}, received{};
  for (size_t index = 0; index < expected.size(); ++index) expected[index] = char(index % 113);
  const auto transferDeadline = Clock::now() + std::chrono::seconds(1);
  for (unsigned sequence = 0; sequence < datagrams; ++sequence) {
    memcpy(expected.data(), &sequence, sizeof(sequence));
    for (;;) {
      if (Clock::now() >= transferDeadline) { result.socketStatus = WSAETIMEDOUT; return finish("udp_loopback_send_timed_out"); }
      const int sent = send(sender.value, expected.data(), int(expected.size()), 0);
      if (sent == int(expected.size())) { ++result.sentDatagrams; result.sentBytes += unsigned(sent); break; }
      if (sent != SOCKET_ERROR) return finish("udp_loopback_partial_datagram");
      const int error = WSAGetLastError();
      if (error != WSAEWOULDBLOCK) { result.socketStatus = DWORD(error); return finish("udp_loopback_send_failed"); }
      Sleep(1);
    }
    for (;;) {
      if (Clock::now() >= transferDeadline) { result.socketStatus = WSAETIMEDOUT; return finish("udp_loopback_receive_timed_out"); }
      const int count = recv(receiver.value, received.data(), int(received.size()), 0);
      if (count >= 0) {
        if (count != int(expected.size()) || received != expected) return finish("udp_loopback_payload_mismatch");
        ++result.receivedDatagrams; result.receivedBytes += unsigned(count); break;
      }
      const int error = WSAGetLastError();
      if (error != WSAEWOULDBLOCK) { result.socketStatus = DWORD(error); return finish("udp_loopback_receive_failed"); }
      Sleep(1);
    }
  }
  // ControlTrace supersedes FlushTrace. Flush only the private session this diagnostic owns.
  result.traceStatus = ControlTraceW(trace.state->session, nullptr, trace.state->header(), EVENT_TRACE_CONTROL_FLUSH);
  if (result.traceStatus != ERROR_SUCCESS) return finish("trace_flush_failed");
  const auto deliveryDeadline = Clock::now() + std::chrono::seconds(3);
  do {
    if (!trace.sample(processes)) return finish("trace_consumer_stopped");
    ++result.samples;
    if (!std::isfinite(processes[0].network) || processes[0].network < 0) return finish("owned_process_rate_invalid");
    result.peakBytesPerSecond = std::max(result.peakBytesPerSecond, processes[0].network);
    if (result.peakBytesPerSecond > 0 && trace.state->ownUdpEvents.load() > 0) {
      if (!trace.active() || trace.status() != ERROR_SUCCESS) return finish("trace_consumer_stopped");
      return finish("", true);
    }
    Sleep(25);
  } while (Clock::now() < deliveryDeadline);
  return finish("no_owned_udp_attribution");
}
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
