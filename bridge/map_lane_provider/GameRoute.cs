using System.Diagnostics;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

// The route the game's GPS shows, read from the running game with PROCESS_VM_READ only (no
// injection, no writes): the static base_ctrl pointer (ETS2LA's pattern in the installed
// executable) holds the game_ctrl unit; its gps_manager (member offset from ETS2LA's second
// pattern) has a simple route source whose route task lists the route in order as nodes, with
// the prefabs between them (research/sources/ETS2LA_plugin-3b01d90b5be2 src/prism/navigation,
// checked on 1.61.1.1 and 1.61.1.9). Consecutive nodes share the road or prefab between them
// (node_item_t backward/forward item), which gives the item chain without the map. Written to
// <out>.route.json in the route file format (README) when the game's route task changes.
sealed class GameRoute(string file) : IDisposable
{
    const uint VmRead = 0x10, QueryLimited = 0x1000;
    [DllImport("kernel32", SetLastError = true)] static extern nint OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32", SetLastError = true)] static extern bool ReadProcessMemory(nint process, nint address, byte[] buffer, nint size, out nint read);
    [DllImport("kernel32")] static extern bool CloseHandle(nint handle);

    nint process;
    int pid;
    long baseAddress, ctrlRva, gpsOffset;
    (long task, long data, long size) seen;
    int revision;
    public string? LastError;

    // One read: the bytes or an exception (a freed or moved route task reads as an error or as
    // data that fails the checks below; the next cycle reads again).
    byte[] Read(long address, int size)
    {
        if (address == 0) throw new InvalidOperationException("null game pointer");
        var buffer = new byte[size];
        if (!ReadProcessMemory(process, (nint)address, buffer, size, out var read) || read != size)
            throw new InvalidOperationException($"game memory at {address:x} unreadable");
        return buffer;
    }
    long Pointer(long address) => BitConverter.ToInt64(Read(address, 8));

    void Attach()
    {
        var found = Process.GetProcessesByName("eurotrucks2").FirstOrDefault() ?? throw new InvalidOperationException("game not running");
        if (process != 0 && found.Id == pid) return;
        Detach();
        process = OpenProcess(VmRead | QueryLimited, false, found.Id);
        if (process == 0) throw new InvalidOperationException($"OpenProcess failed ({Marshal.GetLastWin32Error()})");
        pid = found.Id;baseAddress = found.MainModule!.BaseAddress;
        if (ctrlRva == 0) Scan(found.MainModule.FileName);
        seen = default;
    }
    void Detach() {if (process != 0) CloseHandle(process);process = 0;pid = 0;}
    public void Dispose() => Detach();

    // base_ctrl: mov rax,[rip+disp32] (instance pointer); gps_manager: lea rcx,[rax+disp32] (member offset).
    void Scan(string exe)
    {
        var image = File.ReadAllBytes(exe);
        int pe = BitConverter.ToInt32(image, 0x3C), sections = BitConverter.ToUInt16(image, pe + 6), table = pe + 24 + BitConverter.ToUInt16(image, pe + 20);
        for (int i = 0; i < sections; ++i)
        {
            int at = table + 40 * i;
            if (Encoding.ASCII.GetString(image, at, 8).TrimEnd('\0') != ".text") continue;
            int va = BitConverter.ToInt32(image, at + 12), size = BitConverter.ToInt32(image, at + 16), raw = BitConverter.ToInt32(image, at + 20);
            var code = image.AsSpan(raw, size);
            int ctrl = Find(code, "48 8b ? ? ? ? ? 48 8b ? 48 8b ? 48 8b 80"), gps = Find(code, "48 8d 88 ? ? ? ? 48 85 c9 74 ? 48 8b 01 48 8d 54");
            if (ctrl < 0 || gps < 0) throw new InvalidOperationException("game GPS patterns not found (game update?)");
            ctrlRva = va + ctrl + 7 + BitConverter.ToInt32(code.Slice(ctrl + 3, 4));
            gpsOffset = BitConverter.ToUInt32(code.Slice(gps + 3, 4));
            Console.Error.WriteLine($"game route: base_ctrl pointer rva {ctrlRva:x}, gps_manager +{gpsOffset:x}");
            return;
        }
        throw new InvalidOperationException("no .text section in the game executable");
    }
    static int Find(ReadOnlySpan<byte> code, string pattern)
    {
        var parts = pattern.Split(' ');
        var bytes = parts.Select(p => p == "?" ? -1 : Convert.ToInt32(p, 16)).ToArray();
        for (int i = 0; i + bytes.Length <= code.Length; ++i)
        {
            int k = 0;
            while (k < bytes.Length && (bytes[k] < 0 || code[i + k] == bytes[k])) ++k;
            if (k == bytes.Length) return i;
        }
        return -1;
    }

    // The unit's class name from its descriptor getter (vtable slot 5: lea rax,[rip+disp32]; ret), never called.
    string UnitName(long unit)
    {
        long getter = Pointer(Pointer(unit) + 5 * 8);
        var code = Read(getter, 8);
        if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05 || code[7] != 0xC3) throw new InvalidOperationException("unexpected unit descriptor getter");
        long descriptor = getter + 7 + BitConverter.ToInt32(code, 3);
        var name = Read(Pointer(Pointer(descriptor)), 32);
        return Encoding.ASCII.GetString(name, 0, Math.Max(0, Array.IndexOf(name, (byte)0)));
    }

    // Remaining: the game's distance and time to the destination from this node (map scale: about
    // 19x the world outside cities, 3x inside, as the GPS shows it).
    sealed record Node(ulong Uid, Vector3 Position, (byte Kind, ulong Uid) Back, (byte Kind, ulong Uid) Forward, float Remaining, float RemainingTime);

    // (written, items): reads the route task's header every call, the route only when it changed.
    public (bool written, int items) Update()
    {
        Attach();
        long ctrl = Pointer(baseAddress + ctrlRva);
        if (ctrl == 0 || UnitName(ctrl) != "game_ctrl") return Clear("not driving");
        long gps = ctrl + gpsOffset, task = Pointer(gps + 8 + 0x20);
        if (task == 0) return Clear("no route");
        // route_items: dynamic array {vptr, data, size, capacity, allocator} of 0x30-byte entries.
        var header = Read(task + 0x78, 32);
        long data = BitConverter.ToInt64(header, 8), size = BitConverter.ToInt64(header, 16), capacity = BitConverter.ToInt64(header, 24);
        if (size <= 0 || size > capacity || size > 200_000 || data == 0) throw new InvalidOperationException($"inconsistent route items ({size}/{capacity})");
        if (seen == (task, data, size)) return (false, (int)size);
        var raw = Read(data, (int)size * 0x30);
        var nodes = new List<Node>();
        for (int i = 0; i < size; ++i)
        {
            int at = i * 0x30;
            if (BitConverter.ToUInt32(raw, at) != 0) continue; // a prefab entry: the nodes around it share it
            var node = Read(BitConverter.ToInt64(raw, at + 8), 0x38);
            var p = new Vector3(BitConverter.ToInt32(node, 0), BitConverter.ToInt32(node, 4), BitConverter.ToInt32(node, 8)) / 256;
            nodes.Add(new(BitConverter.ToUInt64(node, 0x30), p, Item(BitConverter.ToInt64(node, 0x20)), Item(BitConverter.ToInt64(node, 0x28)),
                BitConverter.ToSingle(raw, at + 0x24), BitConverter.ToSingle(raw, at + 0x28)));
        }
        // The task may have been replaced while reading: keep the result only if its header still holds.
        if (Pointer(gps + 8 + 0x20) != task || BitConverter.ToInt64(Read(task + 0x78 + 8, 8)) != data) throw new InvalidOperationException("route changed while reading");
        if (nodes.Count < 2 || nodes.Any(n => n.Uid == 0)) throw new InvalidOperationException("route nodes unreadable");
        Write(nodes);
        seen = (task, data, size);
        return (true, nodes.Count);
    }
    (byte, ulong) Item(long address)
    {
        if (address == 0) return (0, 0);
        var item = Read(address, 0x48);
        return (item[0xA], BitConverter.ToUInt64(item, 0x40));
    }
    (bool, int) Clear(string reason)
    {
        if (seen != default || !File.Exists(file)) RollingLane.Replace(file, JsonSerializer.Serialize(new {frame_id = "world", source = "game", revision = ++revision, created = DateTime.UtcNow.ToString("o"), unavailable = reason}));
        seen = default;
        return (false, 0);
    }

    void Write(List<Node> nodes)
    {
        static double R(double v) => Math.Round(v, 2);
        static double[] Ros(Vector3 p) => [R(p.X), R(-p.Z), R(p.Y)];
        var items = new List<object>();
        double s = 0;
        for (int i = 0; i + 1 < nodes.Count; ++i)
        {
            var (a, b) = (nodes[i], nodes[i + 1]);
            double length = Vector2.Distance(new(a.Position.X, a.Position.Z), new(b.Position.X, b.Position.Z));
            // The road or prefab both nodes touch; none across a ferry or train (the game joins
            // the ports' nodes directly).
            var shared = new[] {a.Forward, a.Back}.FirstOrDefault(x => x.Uid != 0 && (x == b.Back || x == b.Forward));
            string kind = shared.Uid == 0 ? "gap" : shared.Kind == 3 ? "road" : shared.Kind == 4 ? "prefab" : $"item{shared.Kind}";
            items.Add(new {kind, uid = shared.Uid == 0 ? null : shared.Uid.ToString("x16"), entry_node = a.Uid.ToString("x16"), exit_node = b.Uid.ToString("x16"),
                forward = kind == "road" ? shared == a.Forward : (bool?)null, exit_curves = Array.Empty<int>(), lanes_ok = Array.Empty<int>(),
                s_start_m = R(s), length_m = R(length), game_distance_m = R(a.Remaining - b.Remaining)});
            s += length;
        }
        RollingLane.Replace(file, JsonSerializer.Serialize(new {frame_id = "world", source = "game", revision = ++revision, created = DateTime.UtcNow.ToString("o"),
            start = Ros(nodes[0].Position)[..2], goal = Ros(nodes[^1].Position)[..2], length_m = R(s), eta_s = (double?)null,
            game_distance_m = R(nodes[0].Remaining), game_time_s = R(nodes[0].RemainingTime), items, polyline = nodes.Select(n => Ros(n.Position))}));
    }
}
