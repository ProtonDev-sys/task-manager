using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace TaskManager.Monitoring;

internal sealed record MemoryHardware(int Speed, int UsedSlots, int Slots, string FormFactor, ulong InstalledBytes);
internal sealed record DiskHardware(string Model, string Type, long? Capacity);

internal static class HardwareInventory
{
    public static MemoryHardware Memory()
    {
        GetPhysicallyInstalledSystemMemory(out var installed);
        var size = GetSystemFirmwareTable(0x52534d42, 0, null, 0);
        if (size is < 8 or > 1048576) return new(0, 0, 0, "—", installed * 1024);
        var bytes = new byte[size];
        if (GetSystemFirmwareTable(0x52534d42, 0, bytes, size) != size) return new(0, 0, 0, "—", installed * 1024);
        return ParseMemory(bytes, installed * 1024);
    }

    internal static MemoryHardware ParseMemory(byte[] bytes, ulong installed)
    {
        var offset = 8;
        var slots = 0;
        var used = 0;
        var speed = 0;
        var form = "—";
        if (bytes.Length < 8) return new(0, 0, 0, form, installed);
        var end = Math.Min((long)bytes.Length, BitConverter.ToUInt32(bytes, 4) + 8L);
        while (offset + 4 <= end)
        {
            var type = bytes[offset];
            var length = bytes[offset + 1];
            if (length < 4 || offset + length > end) break;
            if (type == 17 && length >= 21)
            {
                slots++;
                var capacity = BitConverter.ToUInt16(bytes, offset + 12);
                if (capacity != 0 && capacity != 0xffff)
                {
                    used++;
                    var configured = length >= 34 ? BitConverter.ToUInt16(bytes, offset + 32) : (ushort)0;
                    var nominal = length >= 23 ? BitConverter.ToUInt16(bytes, offset + 21) : (ushort)0;
                    var moduleSpeed = configured is > 0 and < 65535 ? configured : nominal;
                    if (moduleSpeed > 0 && moduleSpeed < 65535) speed = speed == 0 ? moduleSpeed : Math.Min(speed, moduleSpeed);
                    form = bytes[offset + 14] switch { 9 => "DIMM", 13 => "SODIMM", _ => "—" };
                }
            }
            offset += length;
            while (offset + 1 < end && (bytes[offset] != 0 || bytes[offset + 1] != 0)) offset++;
            offset += 2;
            if (type == 127) break;
        }
        return new(speed, used, slots, form, installed);
    }

    public static DiskHardware Disk(int index)
    {
        using var handle = CreateFileW(@"\\.\PhysicalDrive" + index, 0, 3, 0, 3, 0, 0);
        if (handle.IsInvalid) return new("Physical disk", "—", null);
        var query = new byte[12];
        var descriptor = new byte[4096];
        var model = "Physical disk";
        if (DeviceIoControl(handle, 0x2d1400, query, query.Length, descriptor, descriptor.Length, out var size, 0) && size >= 36)
        {
            var location = BitConverter.ToUInt32(descriptor, 16);
            if (location < size)
            {
                var end = Array.IndexOf(descriptor, (byte)0, (int)location, size - (int)location);
                if (end >= 0) model = Encoding.ASCII.GetString(descriptor, (int)location, end - (int)location).Trim();
            }
        }
        BitConverter.GetBytes(7).CopyTo(query, 0);
        var kind = DeviceIoControl(handle, 0x2d1400, query, query.Length, descriptor, descriptor.Length, out var seekSize, 0) && seekSize >= 9
            ? descriptor[8] == 0 ? "SSD" : "HDD" : "—";
        long? capacity = null;
        if (DeviceIoControl(handle, 0x700a0, null, 0, descriptor, descriptor.Length, out var geometrySize, 0) && geometrySize >= 32)
        {
            var length = BitConverter.ToInt64(descriptor, 24);
            if (length > 0) capacity = length;
        }
        return new(model, kind, capacity);
    }

    [DllImport("kernel32.dll", SetLastError = true)] private static extern uint GetSystemFirmwareTable(uint provider, uint id, byte[]? buffer, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetPhysicallyInstalledSystemMemory(out ulong kilobytes);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, nint security, uint creation, uint flags, nint template);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool DeviceIoControl(SafeFileHandle handle, uint code, byte[]? input, int inputSize, byte[] output, int outputSize, out int returned, nint overlapped);
}
