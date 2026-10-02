#include "core.hpp"
#include <dxgi.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <devguid.h>

namespace taskmgr {
template <typename Visit> static void logicalProcessors(LOGICAL_PROCESSOR_RELATIONSHIP relationship, Visit visit) {
  DWORD size = 0; GetLogicalProcessorInformationEx(relationship, nullptr, &size);
  if (!size || size > 1024 * 1024) return;
  std::vector<std::byte> data(size);
  if (!GetLogicalProcessorInformationEx(relationship, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(data.data()), &size)) return;
  for (DWORD offset = 0; offset + sizeof(DWORD) * 2 <= size;) {
    const auto record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(data.data() + offset);
    if (record->Size < sizeof(DWORD) * 2 || record->Size > size - offset) break;
    visit(*record); offset += record->Size;
  }
}
CpuInfo readCpu() {
  CpuInfo cpu; SYSTEM_INFO system{}; GetNativeSystemInfo(&system); cpu.logical = std::max(1UL, system.dwNumberOfProcessors);
  wchar_t name[512]{}; DWORD size = sizeof(name), mhz = 0, mhzSize = sizeof(mhz);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) cpu.name = name;
  while (!cpu.name.empty() && iswspace(cpu.name.back())) cpu.name.pop_back();
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &mhzSize) == ERROR_SUCCESS) cpu.baseMhz = mhz;
  logicalProcessors(RelationProcessorCore, [&](const auto&) { ++cpu.cores; });
  logicalProcessors(RelationProcessorPackage, [&](const auto&) { ++cpu.sockets; });
  logicalProcessors(RelationCache, [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& record) { if (record.Cache.Level >= 1 && record.Cache.Level <= 3) cpu.caches[record.Cache.Level - 1] += record.Cache.CacheSize; });
  cpu.virtualization = IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != FALSE;
  return cpu;
}
// Memory speed, slots and form factor from the SMBIOS type 17 (memory device) records.
MemoryInfo readMemoryHardware() {
  MemoryInfo memory; ULONGLONG installed = 0; if (GetPhysicallyInstalledSystemMemory(&installed)) memory.installed = installed * 1024;
  const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
  if (size < 8 || size > 1024 * 1024) return memory;
  std::vector<BYTE> data(size);
  if (GetSystemFirmwareTable('RSMB', 0, data.data(), size) != size) return memory;
  const size_t end = std::min<size_t>(size, size_t(*reinterpret_cast<const DWORD*>(data.data() + 4)) + 8);
  for (size_t offset = 8; offset + 4 <= end;) {
    const BYTE type = data[offset], length = data[offset + 1];
    if (length < 4 || offset + length > end) break;
    if (type == 17 && length >= 21) {
      ++memory.slots; const WORD capacity = *reinterpret_cast<const WORD*>(&data[offset + 12]);
      if (capacity != 0 && capacity != 0xffff) {
        ++memory.usedSlots;
        const WORD configured = length >= 34 ? *reinterpret_cast<const WORD*>(&data[offset + 32]) : 0, nominal = length >= 23 ? *reinterpret_cast<const WORD*>(&data[offset + 21]) : 0;
        const WORD speed = configured > 0 && configured < 0xffff ? configured : nominal;
        if (speed > 0 && speed < 0xffff) memory.speed = memory.speed ? std::min<unsigned>(memory.speed, speed) : speed;
        memory.formFactor = data[offset + 14] == 9 ? L"DIMM" : data[offset + 14] == 13 ? L"SODIMM" : memory.formFactor;
      }
    }
    offset += length;
    while (offset + 1 < end && (data[offset] || data[offset + 1])) ++offset;
    offset += 2;
    if (type == 127) break;
  }
  return memory;
}
DiskInfo readDiskHardware(int index) {
  DiskInfo disk; disk.index = index; disk.model = L"Physical disk";
  Handle device(CreateFileW((L"\\\\.\\PhysicalDrive" + std::to_wstring(index)).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  if (device.value == INVALID_HANDLE_VALUE) return disk;
  STORAGE_PROPERTY_QUERY query{StorageDeviceProperty, PropertyStandardQuery}; std::array<BYTE, 4096> output{}; DWORD returned = 0;
  if (DeviceIoControl(device.value, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), output.data(), DWORD(output.size()), &returned, nullptr) && returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
    const auto descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(output.data());
    if (descriptor->ProductIdOffset && descriptor->ProductIdOffset < returned) {
      std::string model(reinterpret_cast<const char*>(output.data() + descriptor->ProductIdOffset), strnlen(reinterpret_cast<const char*>(output.data() + descriptor->ProductIdOffset), returned - descriptor->ProductIdOffset));
      while (!model.empty() && isspace(static_cast<unsigned char>(model.back()))) model.pop_back();
      while (!model.empty() && isspace(static_cast<unsigned char>(model.front()))) model.erase(model.begin());
      if (!model.empty()) disk.model.assign(model.begin(), model.end());
    }
  }
  query.PropertyId = StorageDeviceSeekPenaltyProperty; DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
  if (DeviceIoControl(device.value, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &returned, nullptr) && returned >= sizeof(seek)) disk.type = seek.IncursSeekPenalty ? L"HDD" : L"SSD";
  DISK_GEOMETRY_EX geometry{};
  if (DeviceIoControl(device.value, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry), &returned, nullptr)) disk.capacity = uint64_t(geometry.DiskSize.QuadPart);
  return disk;
}
static std::wstring deviceText(HDEVINFO devices, SP_DEVINFO_DATA& device, const DEVPROPKEY& key) {
  DEVPROPTYPE type = 0; std::array<BYTE, 2048> value{}; DWORD size = 0;
  if (!SetupDiGetDevicePropertyW(devices, &device, &key, &type, value.data(), DWORD(value.size() - 2), &size, 0)) return L"";
  if (type == DEVPROP_TYPE_STRING) return reinterpret_cast<const wchar_t*>(value.data());
  if (type == DEVPROP_TYPE_FILETIME) { SYSTEMTIME time{}; FileTimeToSystemTime(reinterpret_cast<const FILETIME*>(value.data()), &time); wchar_t text[64]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr, text, 64, nullptr); return text; }
  return L"";
}
std::vector<GpuInfo> readGpus() {
  std::vector<GpuInfo> result; IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return result;
  for (UINT index = 0; index < 32; ++index) {
    IDXGIAdapter1* adapter = nullptr; if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
    DXGI_ADAPTER_DESC1 description{};
    if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && std::none_of(result.begin(), result.end(), [&](const GpuInfo& gpu) { wchar_t key[64]{}; swprintf_s(key, L"luid_0x%08x_0x%08x", unsigned(description.AdapterLuid.HighPart), description.AdapterLuid.LowPart); return gpu.key == key; })) {
      GpuInfo gpu; wchar_t key[64]{}; swprintf_s(key, L"luid_0x%08x_0x%08x", unsigned(description.AdapterLuid.HighPart), description.AdapterLuid.LowPart);
      gpu.key = key; gpu.index = int(result.size()); gpu.name = description.Description; gpu.dedicatedLimit = description.DedicatedVideoMemory; gpu.sharedLimit = description.SharedSystemMemory;
      LARGE_INTEGER version{};
      if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) gpu.driverVersion = std::to_wstring(HIWORD(version.HighPart)) + L"." + std::to_wstring(LOWORD(version.HighPart)) + L"." + std::to_wstring(HIWORD(version.LowPart)) + L"." + std::to_wstring(LOWORD(version.LowPart));
      result.push_back(std::move(gpu));
    }
    adapter->Release();
  }
  factory->Release();
  const HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
  if (devices != INVALID_HANDLE_VALUE) {
    SP_DEVINFO_DATA device{sizeof(device)};
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device); ++index) {
      const auto name = deviceText(devices, device, DEVPKEY_Device_DeviceDesc);
      for (auto& gpu : result) if (gpu.location.empty() && lower(gpu.name) == lower(name)) { gpu.driverDate = deviceText(devices, device, DEVPKEY_Device_DriverDate); gpu.location = deviceText(devices, device, DEVPKEY_Device_LocationInfo); const auto version = deviceText(devices, device, DEVPKEY_Device_DriverVersion); if (!version.empty()) gpu.driverVersion = version; break; }
    }
    SetupDiDestroyDeviceInfoList(devices);
  }
  return result;
}
// Adapter temperature through the display kernel interface; -1 where the driver does not report one.
double gpuTemperature(const std::wstring& key) {
  struct Open { LUID luid; UINT adapter; };
  struct Query { UINT adapter; UINT type; void* data; UINT size; };
  struct Close { UINT adapter; };
  struct alignas(8) Performance { UINT32 physical; ULONGLONG memoryFrequency, maximumMemoryFrequency, overclockFrequency, memoryBandwidth, pcieBandwidth; ULONG fan, power, temperature; UCHAR overrideState; };
  using OpenAdapter = LONG (APIENTRY*)(Open*); using QueryAdapter = LONG (APIENTRY*)(const Query*); using CloseAdapter = LONG (APIENTRY*)(const Close*);
  static const auto gdi = GetModuleHandleW(L"gdi32.dll");
  static const auto open = reinterpret_cast<OpenAdapter>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")); static const auto query = reinterpret_cast<QueryAdapter>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")); static const auto close = reinterpret_cast<CloseAdapter>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
  if (!open || !query || !close || !key.starts_with(L"luid_0x")) return -1;
  const auto separator = key.find(L"_0x", 7); if (separator == key.npos) return -1;
  const auto high = counterInteger(std::wstring_view(key).substr(7, separator - 7), 16), low = counterInteger(std::wstring_view(key).substr(separator + 3), 16);
  if (!high || !low) return -1;
  Open adapter{{*low, LONG(*high)}, 0}; if (open(&adapter) < 0) return -1;
  Performance performance{}; const Query request{adapter.adapter, 62, &performance, sizeof(performance)};
  const bool read = query(&request) >= 0; const Close closing{adapter.adapter}; close(&closing);
  return read && performance.temperature > 0 && performance.temperature < 2000 ? performance.temperature / 10.0 : -1;
}
// Firmware POST time recorded by Windows, as shown on the Startup tab.
double lastBiosSeconds() {
  static const double seconds = [] { DWORD value = 0, size = sizeof(value); return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"FwPOSTTime", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value ? value / 1000.0 : 0; }();
  return seconds;
}
}
