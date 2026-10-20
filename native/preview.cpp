#include "app.hpp"
#include "report.hpp"

namespace taskmgr {
namespace {
// This file is linked only into developer builds. No live inventories, process
// handles, paths, accounts or network addresses are used by these previews.
constexpr uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB;
struct PreviewProcess { const wchar_t* name; const wchar_t* title; double cpu; unsigned memory; bool app; int parent = -1; };
constexpr PreviewProcess previewProcesses[] = {
  {L"browser.exe", L"Browser", 4.8, 428, true},
  {L"browser.exe", L"Browser", 1.2, 184, false, 0},
  {L"browser.exe", L"Browser", .7, 96, false, 0},
  {L"editor.exe", L"Code editor", 2.6, 312, true},
  {L"explorer.exe", L"File Explorer", .3, 84, true},
  {L"music.exe", L"Music", .1, 122, true},
  {L"TaskManager.exe", L"Task Manager", .2, 24, true},
  {L"clouddrive.exe", L"Cloud Drive", .2, 64, false},
  {L"searchhost.exe", L"Search", 0, 42, false},
  {L"updater.exe", L"Software updater", 0, 18, false},
  {L"dwm.exe", L"Desktop Window Manager", .7, 56, false},
  {L"svchost.exe", L"Service Host", .1, 28, false},
  {L"services.exe", L"Services and Controller app", 0, 12, false},
};
constexpr DWORD PreviewPid = 4000000000UL;
Sample previewSample(unsigned frame) {
  const double phase = double(frame) * .31;
  const double wave = std::sin(phase) * 6 + std::sin(phase * 2.3) * 3;
  Sample sample;
  sample.cpu = 24 + wave; sample.cpuSpeed = 4.12; sample.interrupts = .1;
  sample.elapsed = 1; sample.resourcesSampledAt = Clock::now(); sample.uptime = 93784;
  sample.threads = 2468; sample.handles = 82340;
  sample.cpuInfo.name = L"Example 8-core processor"; sample.cpuInfo.baseMhz = 3600;
  sample.cpuInfo.sockets = 1; sample.cpuInfo.cores = 8; sample.cpuInfo.logical = 16;
  sample.cpuInfo.caches = {512 * 1024, 8 * MiB, 32 * MiB}; sample.cpuInfo.virtualization = true;
  for (unsigned core = 0; core < sample.cpuInfo.logical; ++core)
    sample.cores.push_back(std::clamp(22 + wave + std::sin(phase + core) * 12, 0.0, 100.0));
  sample.memory.total = sample.memory.installed = 32 * GiB;
  sample.memory.available = 18 * GiB + 512 * MiB; sample.memory.committed = 16 * GiB;
  sample.memory.commitLimit = 48 * GiB; sample.memory.cached = 6 * GiB;
  sample.memory.paged = 384 * MiB; sample.memory.nonpaged = 256 * MiB;
  sample.memory.compressed = 384 * MiB; sample.memory.speed = 5600;
  sample.memory.slots = 4; sample.memory.usedSlots = 2; sample.memory.formFactor = L"DIMM";
  DiskInfo disk;
  disk.key = L"preview-disk"; disk.title = L"Disk 0 (C:)"; disk.model = L"Example NVMe SSD";
  disk.type = L"SSD"; disk.letters = L"C:"; disk.active = 7 + std::abs(wave);
  disk.read = (3.4 + std::abs(wave) * .2) * double(MiB); disk.write = .8 * double(MiB);
  disk.response = .6; disk.capacity = disk.formatted = 1024 * GiB;
  disk.system = disk.pagefile = true; sample.disks.push_back(std::move(disk));
  NetworkInfo network;
  network.luid = 1; network.type = L"Ethernet"; network.alias = L"Ethernet";
  network.description = L"Example Ethernet adapter"; network.ipv4 = L"192.0.2.24";
  network.ipv6 = L"2001:db8::24"; network.speed = 1000000000;
  network.send = (48 + std::abs(wave) * 5) * 1024;
  network.receive = (360 + std::abs(wave) * 22) * 1024;
  sample.networks.push_back(std::move(network)); sample.networkAttribution = true;
  GpuInfo gpu;
  gpu.key = L"preview-gpu"; gpu.name = L"Example graphics adapter";
  gpu.usage = 12 + std::abs(wave); gpu.temperature = 42;
  gpu.engines = {{L"3D", gpu.usage}, {L"Copy", 2}, {L"VideoEncode", 0}, {L"VideoDecode", 4}};
  gpu.dedicated = 1536 * MiB; gpu.dedicatedLimit = 8 * GiB;
  gpu.shared = 128 * MiB; gpu.sharedLimit = 16 * GiB;
  gpu.driverVersion = L"1.0.0.0"; gpu.driverDate = L"01/01/2026";
  gpu.location = L"PCI bus 1, device 0, function 0"; sample.gpus.push_back(std::move(gpu));
  for (size_t index = 0; index < std::size(previewProcesses); ++index) {
    const auto& entry = previewProcesses[index]; Process process;
    process.id = PreviewPid + DWORD(index); process.created = 1000 + index;
    process.parent = entry.parent < 0 ? 0 : PreviewPid + DWORD(entry.parent);
    process.session = 1; process.name = entry.name; process.app = entry.app;
    process.cpu = entry.cpu; process.cpuTicks = uint64_t((double(frame) + 1) * entry.cpu * 1000000);
    process.working = process.privateWorking = uint64_t(entry.memory) * MiB;
    process.privateBytes = process.peakWorking = process.working + 8 * MiB;
    process.threads = 12 + DWORD(index) * 3; process.handles = 140 + DWORD(index) * 17;
    process.ioRate = index == 0 ? .4 * double(MiB) : 0;
    process.network = index == 0 ? 420 * 1024 : index == 7 ? 36 * 1024 : 0;
    process.gpu = index == 0 ? 1.8 : index == 10 ? .5 : 0;
    if (process.gpu > 0) process.gpuEngine = L"GPU 0 - 3D";
    process.suspended = index == 8;
    sample.processes.push_back(std::move(process));
  }
  auto inventory = std::make_shared<ServiceInventory>();
  Entry session; session.key = L"preview-user"; session.session = 1;
  session.cells = {L"Demo user", L"Active", L"1", L"Console"}; inventory->sessions.push_back(std::move(session));
  for (const auto* name : {L"Audio service", L"Update service", L"Print service"}) {
    Entry service; service.key = name; service.description = name;
    service.pid = PreviewPid + 11; service.group = L"LocalService";
    service.cells = {name, std::to_wstring(service.pid), name, L"Running", service.group};
    inventory->services.push_back(std::move(service));
  }
  sample.inventory = inventory; sample.services = inventory->services; sample.sessions = inventory->sessions;
  sample.users = std::make_shared<const std::unordered_map<DWORD, std::wstring>>();
  auto startup = std::make_shared<StartupInventory>();
  for (const auto* name : {L"Cloud Drive", L"Music", L"Example updater"}) {
    Entry entry; entry.key = name; entry.enabled = entry.startupEditable = true;
    entry.cells = {name, L"Example Software", L"Enabled", L"Not measured", L"Registry", L"", L"", L"Demo user", L"Demo startup entry"};
    startup->entries.push_back(std::move(entry));
  }
  sample.startup = startup;
  return sample;
}
}
void Application::prepareDemoScreenshots() {
  // Record requested and mapped faces alongside synthetic captures so Windows
  // font substitution can be checked without relying only on screenshot pixels.
  if (const HDC dc = GetDC(window)) {
    Report report{(std::filesystem::path(options.screenshots) / L"font-mapping.json").wstring()};
    report << "{\"fonts\":["; bool separator = false;
    for (const auto& [label, handle] : {std::pair{L"Body", font}, std::pair{L"Page heading", shellHeadingFont}}) {
      LOGFONTW requested{}; GetObjectW(handle, int(sizeof(requested)), &requested);
      const auto previous = SelectObject(dc, handle); wchar_t mapped[LF_FACESIZE]{}; GetTextFaceW(dc, int(std::size(mapped)), mapped);
      if (separator) report << ','; separator = true;
      report << "{\"label\":"; report.jsonString(utf8(label)) << ",\"requested\":"; report.jsonString(utf8(requested.lfFaceName));
      report << ",\"mapped\":"; report.jsonString(utf8(mapped)) << ",\"weight\":" << requested.lfWeight << ",\"height\":" << requested.lfHeight;
      report << ",\"validSelection\":" << (previous && previous != HGDI_ERROR ? "true" : "false") << '}';
      if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
    }
    report << "]}"; ReleaseDC(window, dc);
  }
  for (int second = -60; second <= 0; ++second) {
    current = std::make_shared<Sample>(previewSample(unsigned(second + 60)));
    recordHistories(double(second));
  }
  for (size_t index = 0; index < current->processes.size(); ++index) {
    Metadata metadata;
    metadata.description = previewProcesses[index].title;
    metadata.path = L"C:\\Demo\\" + current->processes[index].name;
    metadata.publisher = L"Example Software"; metadata.user = L"Demo user";
    metadata.commandLine = metadata.path; metadata.virtualization = L"Not allowed";
    metadata.resolved = true;
    icons->setTestMetadata(current->processes[index], std::move(metadata));
  }
  current.reset();
  samplingOverride = [frame = 61u]() mutable { return previewSample(frame++); };
  SetWindowTextW(window, L"Task Manager - Demo data");
}
}
