#include "core.hpp"
#include <evntrace.h>
#include <evntcons.h>

namespace taskmgr {
// Per-process network bytes from the kernel TCP/IP and UDP/IP events. A private system-logger session needs elevation,
// like Windows' own Task Manager; without it the Network column reports that attribution is unavailable.
static constexpr GUID TcpIp{0x9a280ac0, 0xc8e0, 0x11d1, {0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2}};
static constexpr GUID UdpIp{0xbf3a50c5, 0xa9c9, 0x4988, {0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80}};
struct NetworkTrace::State {
  std::wstring name = L"TaskManagerNative-" + std::to_wstring(GetCurrentProcessId());
  std::vector<std::byte> properties;
  TRACEHANDLE session = 0, consumer = INVALID_PROCESSTRACE_HANDLE;
  std::thread worker;
  std::mutex gate;
  std::unordered_map<DWORD, uint64_t> counts;
  EVENT_TRACE_PROPERTIES* header() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data()); }
  void reset() {
    properties.assign(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t), std::byte{});
    auto value = header(); value->Wnode.BufferSize = ULONG(properties.size()); value->Wnode.Flags = WNODE_FLAG_TRACED_GUID; value->Wnode.ClientContext = 1; CoCreateGuid(&value->Wnode.Guid);
    value->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE; value->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP; value->FlushTimer = 1; value->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  }
  static void WINAPI record(EVENT_RECORD* event) {
    const auto& provider = event->EventHeader.ProviderId; const auto opcode = event->EventHeader.EventDescriptor.Opcode;
    if ((provider != TcpIp && provider != UdpIp) || (opcode != 10 && opcode != 11 && opcode != 26 && opcode != 27) || event->UserDataLength < 8) return;
    DWORD values[2]{}; memcpy(values, event->UserData, sizeof(values));
    auto state = static_cast<State*>(event->UserContext); std::lock_guard lock(state->gate); state->counts[values[0]] += values[1];
  }
};
NetworkTrace::NetworkTrace() : state(std::make_unique<State>()) {
  state->reset(); ControlTraceW(0, state->name.c_str(), state->header(), EVENT_TRACE_CONTROL_STOP); state->reset();
  if (StartTraceW(&state->session, state->name.c_str(), state->header()) != ERROR_SUCCESS) { state->session = 0; return; }
  EVENT_TRACE_LOGFILEW log{}; log.LoggerName = state->name.data(); log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD; log.EventRecordCallback = State::record; log.Context = state.get();
  state->consumer = OpenTraceW(&log);
  if (state->consumer == INVALID_PROCESSTRACE_HANDLE) { ControlTraceW(state->session, nullptr, state->header(), EVENT_TRACE_CONTROL_STOP); state->session = 0; return; }
  state->worker = std::thread([consumer = state->consumer] { TRACEHANDLE handles[] = {consumer}; ProcessTrace(handles, 1, nullptr, nullptr); });
}
NetworkTrace::~NetworkTrace() {
  if (state->session) ControlTraceW(state->session, nullptr, state->header(), EVENT_TRACE_CONTROL_STOP);
  if (state->consumer != INVALID_PROCESSTRACE_HANDLE) CloseTrace(state->consumer);
  if (state->worker.joinable()) state->worker.join();
}
bool NetworkTrace::active() const { return state->session != 0; }
std::unordered_map<DWORD, double> NetworkTrace::rates(double elapsed) {
  std::unordered_map<DWORD, uint64_t> counts; { std::lock_guard lock(state->gate); counts.swap(state->counts); }
  std::unordered_map<DWORD, double> result; if (elapsed <= 0) return result;
  for (const auto& [pid, amount] : counts) result.emplace(pid, double(amount) / elapsed);
  return result;
}
}
