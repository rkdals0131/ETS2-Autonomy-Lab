using System.Diagnostics;
using System.IO.Pipes;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Globalization;
using TruckLib;
using TruckLib.HashFs;
using TruckLib.Models.Ppd;
using TruckLib.ScsMap;
using TruckLib.Sii;

try
{
    var options = new Dictionary<string, string>();
    for (int i = 0; i < args.Length; ++i)
    {
        if (!args[i].StartsWith("--")) throw new ArgumentException("Options require --name value pairs");
        // --follow is the only option without a value.
        if (args[i] == "--follow") {options[args[i]] = "1"; continue;}
        if (i + 1 == args.Length) throw new ArgumentException($"{args[i]} requires a value");
        options.Add(args[i], args[++i]);
    }
    double Number(string name, double fallback, double min = double.Epsilon)
    {
        double value = options.TryGetValue(name, out var text) ? double.Parse(text, CultureInfo.InvariantCulture) : fallback;
        if (!double.IsFinite(value) || value < min) throw new ArgumentException($"{name} must be a finite number >= {min}");
        return value;
    }
    string game = options.GetValueOrDefault("--game") ?? Environment.GetEnvironmentVariable("ETS2_DIR")
        ?? throw new ArgumentException("Pass --game <ETS2 install directory> or set ETS2_DIR");
    if (options.TryGetValue("--audit-looks", out var audit))
    {
        using var all = new World(game);
        Audit.Run(all, audit);
        return 0;
    }
    double distance = Number("--distance-m", 1000);
    bool follow = options.ContainsKey("--follow");
    // Rolling mode: extend once less than extend-below is left ahead, drop points keep-behind past the truck.
    double extendBelow = Number("--extend-below-m", Math.Min(600, distance)), keepBehind = Number("--keep-behind-m", 300, 0), period = Number("--period-s", .5);
    string? snapshotSource = options.GetValueOrDefault("--snapshot");
    string output = options.GetValueOrDefault("--out", Path.Combine(Environment.CurrentDirectory, "bridge", "recordings", "gt-path", "current-lane.json"));
    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
    // "-" reads one snapshot JSON per stdin line (offline follow replay); a file is re-read every cycle.
    (Vector3 position, Vector3 forward)? Placement()
    {
        string? text = null;
        if (snapshotSource == "-") {do text = Console.In.ReadLine(); while (text is not null && text.Trim().Length == 0); if (text is null) return null;}
        else if (snapshotSource is not null) text = File.ReadAllText(snapshotSource);
        using var snapshot = text is null ? ReadSnapshot() : JsonDocument.Parse(text);
        var state = snapshot.RootElement;
        if (state.TryGetProperty("snapshot", out var nested)) state = nested;
        if (!state.TryGetProperty("sdk", out var sdk) || !sdk.TryGetProperty("truck.world.placement", out var entry) || !entry.TryGetProperty("value", out var placement))
            throw new InvalidOperationException("No truck placement in the snapshot (game loading, in a menu or not driving)");
        var xyz = placement.GetProperty("position_m");
        double heading = placement.GetProperty("euler_rotations")[0].GetDouble() * Math.Tau;
        return (new Vector3(xyz[0].GetSingle(), xyz[1].GetSingle(), xyz[2].GetSingle()), new Vector3(-(float)Math.Sin(heading), 0, -(float)Math.Cos(heading)));
    }
    using var world = new World(game);
    (Vector3 position, Vector3 forward)? initial = null;
    for (string? waiting = null; initial is null; )
    {
        try {initial = Placement() ?? throw new InvalidOperationException("No snapshot on stdin");}
        catch (Exception ex) when (follow && snapshotSource != "-")
        {
            // A resident provider may start before the game: wait for the first placement.
            if (ex.Message != waiting) Console.WriteLine(JsonSerializer.Serialize(new {revision = 0, path_id = 0, updated = false, error = ex.Message}));
            waiting = ex.Message;
            Thread.Sleep(TimeSpan.FromSeconds(period));
        }
    }
    var first = initial.Value;
    if (!follow)
    {
        var lane = RollingLane.Start(world, first.position, first.forward, 1);
        lane.Fill(distance);
        lane.Write(output, 1);
        var o = lane.Origin;
        Console.WriteLine(JsonSerializer.Serialize(new {output, start_item = o.Road is not null ? "road" : "prefab", road_uid = o.Road?.Uid.ToString("x"), road_type = o.Road?.RoadType.ToString(), lane_index = o.Lane,
            road_direction = o.Road is null ? null : o.Increasing ? "forward" : "backward", prefab_uid = o.Prefab?.Uid.ToString("x"), prefab_model = o.Prefab?.Model.ToString(), prefab_curve = o.Road is null ? o.Lane : (int?)null,
            projection_distance_m = o.Distance, lane_width_m = lane.Width, point_count = lane.Points.Count, length_m = lane.EndS, end_reason = lane.EndReason, prefabs = lane.Walker.PrefabCount,
            junction_choices = lane.Walker.Choices, events = lane.Events.Count, max_join_m = lane.Walker.MaxJoin, max_join_turn_deg = lane.Walker.MaxJoinTurn, blends = lane.Walker.Blends, max_blend_m = lane.Walker.MaxBlend, sectors = world.Sectors}));
        return 0;
    }
    RollingLane? rolling = null;
    int revision = 0, pathId = 0;
    string? lastError = null;
    // Follow mode also keeps the same-direction lane on each side (<out>.left.json,
    // <out>.right.json; no file without one) for the driver's lane change by turn signal
    // and for re-acquiring the lane the driver changed to before this lane resets.
    var sides = new[] {new Neighbour(-1, SidePath(output, "left")), new Neighbour(1, SidePath(output, "right"))};
    for (var placement = first; ; )
    {
        long started = Stopwatch.GetTimestamp();
        bool updated = false, reset = false;
        string? error = null;
        (double s, double distance) projection = (0, 0);
        try
        {
            var (x, y) = ((double)placement.position.X, -(double)placement.position.Z);
            if (rolling is not null) projection = rolling.Project(x, y);
            // Off the published lane (lane change, other road, end passed): start a new path here.
            if (rolling is null || projection.distance > rolling.Width * .75)
            {
                rolling = null;
                var next = RollingLane.Start(world, placement.position, placement.forward, pathId + 1);
                next.Fill(distance);
                rolling = next;++pathId;
                projection = rolling.Project(x, y);
                updated = reset = true;
            }
            else if (rolling.Extendable && rolling.EndS - projection.s < extendBelow)
            {
                rolling.Fill(projection.s + distance);
                rolling.Trim(projection.s - keepBehind);
                updated = true;
            }
            if (updated) rolling.Write(output, ++revision);
            foreach (var side in sides) side.Update(world, rolling, projection.s, placement.position.Y, x, y, updated, reset, distance, keepBehind, ref pathId);
        }
        catch (Exception ex) {error = ex.Message;}
        // Replay mode reports every cycle so the driver can step in lockstep.
        if (updated || snapshotSource == "-" || (error is not null && error != lastError))
            Console.WriteLine(JsonSerializer.Serialize(new {revision, path_id = rolling?.PathId, updated, reset, start_s = rolling?.StartS, end_s = rolling?.EndS, truck_s = rolling is null ? (double?)null : projection.s,
                ahead_m = rolling is null ? (double?)null : rolling.EndS - projection.s, offset_m = rolling is null ? (double?)null : projection.distance, point_count = rolling?.Points.Count,
                end_reason = rolling?.EndReason, prefabs = rolling?.Walker.PrefabCount, left_path_id = sides[0].Lane?.PathId, right_path_id = sides[1].Lane?.PathId, sectors = world.Sectors, elapsed_ms = Stopwatch.GetElapsedTime(started).TotalMilliseconds, error}));
        lastError = error;
        if (snapshotSource != "-") Thread.Sleep(TimeSpan.FromSeconds(Math.Max(0, period - Stopwatch.GetElapsedTime(started).TotalSeconds)));
        while (true)
        {
            try {var next = Placement(); if (next is null) return 0; placement = next.Value; break;}
            catch (Exception ex) when (snapshotSource != "-")
            {
                // The game may be paused, loading or closed: keep the last file and retry.
                if (ex.Message != lastError) Console.WriteLine(JsonSerializer.Serialize(new {revision, path_id = pathId, updated = false, error = ex.Message}));
                lastError = ex.Message;
                Thread.Sleep(TimeSpan.FromSeconds(period));
            }
        }
    }
}
catch (Exception ex) {Console.Error.WriteLine(ex.Message);return 1;}

static JsonDocument ReadSnapshot()
{
    using var pipe = new NamedPipeClientStream(".", "ot", PipeDirection.InOut, PipeOptions.Asynchronous);
    pipe.Connect(3000);
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
    var request = Encoding.UTF8.GetBytes("{\"cmd\":\"snapshot\"}\n");
    pipe.WriteAsync(request, timeout.Token).AsTask().GetAwaiter().GetResult();
    using var reader = new StreamReader(pipe, Encoding.UTF8, false, 4096, leaveOpen: true);
    using var reply = JsonDocument.Parse(reader.ReadLineAsync(timeout.Token).AsTask().GetAwaiter().GetResult() ?? throw new IOException("Core closed the pipe"));
    if (!reply.RootElement.GetProperty("ok").GetBoolean()) throw new IOException(reply.RootElement.GetProperty("error").GetString());
    return JsonDocument.Parse(reply.RootElement.GetProperty("result").GetRawText());
}

static string SidePath(string output, string side) => Path.Combine(Path.GetDirectoryName(Path.GetFullPath(output))!, Path.GetFileNameWithoutExtension(output) + "." + side + Path.GetExtension(output));

// Lateral lane centres of a road look in the road frame (x right of the road direction).
// Forward: traffic runs in the road direction. Drivable: not a tram/rail lane.
record LaneGeometry(float[] Centers, bool[] Forward, float[] Widths, bool[] Drivable, string[] Types)
{
    // Drivable lanes carrying traffic in the travel direction, ordered from the left of travel.
    public int[] Lanes(bool increasing) => Enumerable.Range(0, Centers.Length).Where(i => Drivable[i] && Forward[i] == increasing)
        .OrderBy(i => increasing ? Centers[i] : -Centers[i]).ToArray();
    public string Describe() => string.Join(' ', Enumerable.Range(0, Centers.Length).Select(i => $"{Centers[i]:+0.00;-0.00}{(Forward[i] ? "F" : "B")}{(Drivable[i] ? "" : "*")}"));
}

// Where a new path starts: on a road lane (Road set, Lane = lane index) or on a prefab
// navigation curve (Prefab set, Lane = curve index).
record StartPoint(Road? Road, Prefab? Prefab, float T, int Lane, bool Increasing, double Distance, float Width);

// Installed map assets: road looks, prefab descriptors and the loaded map sectors of the
// base map and every installed map DLC (dlc_east, dlc_fr, dlc_north, ...).
sealed class World : IDisposable
{
    public const float LaneSpacing = 4.5f;      // AI lane grid of every road look (see README)
    readonly List<IHashFsReader> archives = new();
    readonly string defKey;
    readonly AssetLoader fs, mapFs;
    readonly Dictionary<string, Unit> looks = new();
    readonly Dictionary<string, string> prefabPaths = new();
    readonly Dictionary<string, LaneGeometry> geometries = new();
    readonly Dictionary<string, string> unsupported = new();
    readonly Dictionary<string, PrefabDescriptor?> descriptors = new();
    readonly HashSet<SectorCoordinate> loaded = new();
    public Map Map {get; private set;} = new();
    public int Sectors => loaded.Count;
    public IReadOnlyDictionary<string, Unit> Looks => looks;
    public World(string game)
    {
        var watch = Stopwatch.StartNew();
        // DLC archives override base ones (first match wins in AssetLoader); only archives
        // with map sectors, world definitions or their prefab/road models are kept open.
        var maps = new List<IHashFsReader>();var assets = new List<IHashFsReader>();var keyParts = new List<string>();
        foreach (var file in Directory.GetFiles(game, "dlc_*.scs").Order(StringComparer.Ordinal))
        {
            IHashFsReader reader;
            try {reader = HashFsReader.Open(file);} catch (Exception ex) {Console.Error.WriteLine($"{Path.GetFileName(file)}: {ex.Message}");continue;}
            bool map = reader.DirectoryExists("/map/europe"), def = reader.DirectoryExists("/def/world");
            if (!map && !def && !reader.DirectoryExists("/prefab2") && !reader.DirectoryExists("/prefab") && !reader.DirectoryExists("/road_template")) {reader.Dispose();continue;}
            archives.Add(reader);assets.Add(reader);
            if (map) maps.Add(reader);
            if (def) keyParts.Add(Stamp(file));
        }
        var dlcs = archives.Count;
        foreach (var name in new[] {"def.scs", "base_map.scs", "base_share.scs", "base.scs"})
        {
            var reader = HashFsReader.Open(Path.Combine(game, name));
            archives.Add(reader);assets.Add(reader);
            if (name == "base_map.scs") maps.Add(reader);
            if (name == "def.scs") keyParts.Insert(0, Stamp(Path.Combine(game, name)));
        }
        defKey = string.Join(';', keyParts);
        fs = new AssetLoader(assets.ToArray());
        mapFs = new AssetLoader(maps.ToArray());
        foreach (var file in fs.GetFiles("/def/world/").Where(f => Path.GetFileName(f).StartsWith("road_look") && f.EndsWith(".sii")).Order(StringComparer.Ordinal))
        foreach (var unit in SiiFile.Open(file, fs).Units) looks[unit.Name] = unit;
        Console.Error.WriteLine($"archives: {dlcs} dlc ({maps.Count - 1} with map sectors), {looks.Count} road look units in {watch.ElapsedMilliseconds} ms");
    }
    static string Stamp(string file) {var info = new FileInfo(file);return $"{info.Name}:{info.Length}:{info.LastWriteTimeUtc.Ticks}";}
    // Prefab unit -> .ppd path, read when the lane first reaches a prefab. Parsing the
    // prefab SII takes about 5 s, so the table is cached beside the provider build,
    // keyed by the installed def.scs and definition-carrying DLC archives (size, time).
    Dictionary<string, string> PrefabPaths()
    {
        if (prefabPaths.Count > 0) return prefabPaths;
        string cache = Path.Combine(AppContext.BaseDirectory, "prefab-desc-cache.json");
        try
        {
            var stored = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(cache));
            if (stored is not null && stored.Remove("", out var storedKey) && storedKey == defKey) {foreach (var (k, v) in stored) prefabPaths[k] = v;return prefabPaths;}
        }
        catch (Exception) {} // missing or stale cache: rebuild below
        var watch = Stopwatch.StartNew();
        foreach (var file in fs.GetFiles("/def/world/").Where(f => Path.GetFileName(f).StartsWith("prefab") && f.EndsWith(".sii")).Order(StringComparer.Ordinal))
        foreach (var unit in SiiFile.Open(file, fs).Units)
            if (unit.Attributes.TryGetValue("prefab_desc", out var desc)) prefabPaths[unit.Name] = ((string)desc.ToString()).Trim('"');
        Console.Error.WriteLine($"prefab definitions: {prefabPaths.Count} in {watch.ElapsedMilliseconds} ms");
        // Atomic replace: another provider process may read the cache at the same time.
        try {var staging = cache + "." + Environment.ProcessId + ".tmp";File.WriteAllText(staging, JsonSerializer.Serialize(new Dictionary<string, string>(prefabPaths) {[""] = defKey}));File.Move(staging, cache, true);}
        catch (Exception ex) {Console.Error.WriteLine($"prefab cache not written: {ex.Message}");}
        return prefabPaths;
    }
    public void Dispose() {foreach (var a in archives) a.Dispose();}
    // Loads the sectors within margin of the positions. Returns true when the map was
    // reopened: every item and node reference taken before is then stale.
    public bool Require(IEnumerable<Vector3> positions, float margin = 500)
    {
        var want = new HashSet<SectorCoordinate>();
        foreach (var p in positions) foreach (var dx in new[] {-margin, 0, margin}) foreach (var dz in new[] {-margin, 0, margin})
            want.Add(new((int)Math.Floor((p.X + dx) / Map.SectorSize), (int)Math.Floor((p.Z + dz) / Map.SectorSize)));
        if (want.IsSubsetOf(loaded)) return false;
        // Keep loaded neighbours of the new area so a rolling path does not reload them; drop the rest.
        var keep = loaded.Where(s => want.Any(w => Math.Abs(w.X - s.X) <= 1 && Math.Abs(w.Z - s.Z) <= 1)).ToList();
        loaded.Clear();loaded.UnionWith(want);loaded.UnionWith(keep);
        Open();
        return true;
    }
    // Every sector of the base map and the installed map DLCs (audit).
    public void RequireAll()
    {
        loaded.Clear();
        loaded.UnionWith(mapFs.GetFiles("/map/europe").Where(f => f.EndsWith(".base")).Select(Sector.SectorCoordsFromSectorFilePath));
        Open();
    }
    void Open()
    {
        var watch = Stopwatch.StartNew();
        Map = Map.Open("/map/europe.mbd", mapFs, loaded.ToList());
        Console.Error.WriteLine($"map: {loaded.Count} sectors [{(loaded.Count > 30 ? "..." : string.Join(' ', loaded.Select(s => $"{s.X},{s.Z}")))}] in {watch.ElapsedMilliseconds} ms");
    }
    // Unsupported looks always throw InvalidOperationException (cached).
    public LaneGeometry Geometry(Road road)
    {
        string key = "road." + road.RoadType + (road.LeftHandTraffic ? ":lht" : "");
        if (geometries.TryGetValue(key, out var cached)) return cached;
        if (unsupported.TryGetValue(key, out var reason)) throw new InvalidOperationException(reason);
        try
        {
            var g = Build("road." + road.RoadType);
            // Left-hand traffic keeps the lane positions and swaps their directions.
            if (road.LeftHandTraffic) g = g with {Forward = g.Forward.Select(f => !f).ToArray()};
            Console.Error.WriteLine($"{key}: {g.Describe()}");
            return geometries[key] = g;
        }
        catch (Exception ex)
        {
            unsupported[key] = ex is InvalidOperationException ? ex.Message : $"No lane geometry for {key}: {ex.Message}";
            Console.Error.WriteLine(unsupported[key]);
            throw new InvalidOperationException(unsupported[key]);
        }
    }
    // Lane centres follow the game's AI lane grid, which every road look uses and which
    // the prefab navigation curves join (audited over the whole map, README):
    // - two-way (lanes on both sides, or a divided look with template_left): right lane i
    //   (counted from the centre) at road_offset + 2.25 + 4.5 i + lane_offsets_right[i].x,
    //   left lane i mirrored and carrying traffic against the road direction;
    // - one-way (lanes on one side only): the lanes centred on the reference line with the
    //   odd lane to the left, 4.5 (k + 0.5) for k = -ceil(n/2) .. floor(n/2)-1, plus offsets.
    // Template width, shoulders and legacy road_size do not move the AI lanes.
    LaneGeometry Build(string key)
    {
        if (!looks.TryGetValue(key, out var unit)) throw new InvalidOperationException($"No road look definition for {key}");
        string[] Types(string name) => unit.Attributes.TryGetValue(name, out var v) && v is System.Collections.IEnumerable list && v is not string
            ? list.Cast<object>().Select(o => o is LinkPointer lp ? lp.Value.ToString() : o?.ToString() ?? "").ToArray() : [];
        float Float(string name) => unit.Attributes.TryGetValue(name, out var v) ? Convert.ToSingle(v, CultureInfo.InvariantCulture) : 0;
        float[] Offsets(string name) => unit.Attributes.TryGetValue(name, out var v) && v is System.Collections.IList list
            ? list.Cast<object>().Select(o => o switch {Vector2 p => p.X, ITuple t => Convert.ToSingle(t[0], CultureInfo.InvariantCulture), _ => 0f}).ToArray() : [];
        var left = Types("lanes_left");var right = Types("lanes_right");
        float offset = Float("road_offset");
        var offsetsLeft = Offsets("lane_offsets_left");var offsetsRight = Offsets("lane_offsets_right");
        float At(float[] a, int i) => i < a.Length ? a[i] : 0;
        var lanes = new List<(float x, bool forward, string type)>();
        if (left.Length > 0 && right.Length > 0 || unit.Attributes.ContainsKey("template_left"))
        {
            for (int i = 0; i < right.Length; ++i) lanes.Add((offset + LaneSpacing * (i + .5f) + At(offsetsRight, i), true, right[i]));
            for (int i = 0; i < left.Length; ++i) lanes.Add((-(offset + LaneSpacing * (i + .5f) + At(offsetsLeft, i)), false, left[i]));
        }
        else
        {
            bool forward = right.Length > 0;
            var types = forward ? right : left;var laneOffsets = forward ? offsetsRight : offsetsLeft;
            for (int j = 0, k = -((types.Length + 1) / 2); j < types.Length; ++j, ++k)
            {
                float x = LaneSpacing * (k + .5f) + offset + At(laneOffsets, j);
                lanes.Add((forward ? x : -x, forward, types[j]));
            }
        }
        // Rail and tram tracks share the road but are not lanes to drive in (road lanes shared
        // with trams, traffic_lane.road.*.tram, are).
        bool Drivable(string type) => !type.StartsWith("traffic_lane.rail");
        if (!lanes.Any(l => Drivable(l.type))) throw new InvalidOperationException($"No drivable lanes in {key} (lanes_left=[{string.Join(',', left)}], lanes_right=[{string.Join(',', right)}])");
        var centers = lanes.Select(l => l.x).ToArray();
        var drivable = lanes.Select(l => Drivable(l.type)).ToArray();
        // Width: spacing to the nearest other drivable lane centre, 3-4.5 m (narrow roads
        // put both directions' AI lanes within one physical lane).
        var widths = centers.Select((c, i) => Math.Clamp(centers.Where((o, j) => j != i && drivable[j]).Select(o => Math.Abs(o - c)).DefaultIfEmpty(LaneSpacing).Min(), 3f, LaneSpacing)).ToArray();
        return new(centers, lanes.Select(l => l.forward).ToArray(), widths, drivable, lanes.Select(l => l.type).ToArray());
    }
    public PrefabDescriptor? Descriptor(Prefab prefab)
    {
        string key = "prefab." + prefab.Model;
        if (descriptors.TryGetValue(key, out var cached)) return cached;
        PrefabDescriptor? descriptor = null;
        try {if (PrefabPaths().TryGetValue(key, out var path)) descriptor = PrefabDescriptor.Open(path, fs);}
        catch (Exception ex) {Console.Error.WriteLine($"{key}: {ex.Message}");}
        return descriptors[key] = descriptor;
    }
    // Nearest drivable lane along the truck heading: a road lane, or a prefab navigation
    // curve when the truck is inside a junction, roundabout or other prefab.
    public StartPoint Locate(Vector3 position, Vector3 forward)
    {
        Require([position], Map.SectorSize);
        StartPoint? selected = null;
        var nearest = Map.MapItems.Values.OfType<Road>().Select(road =>
        {
            int n = Math.Max(1, (int)Math.Ceiling(road.Length / 2));
            float bestT = 0, bestDistance = float.PositiveInfinity;
            for (int i = 0; i <= n; ++i)
            {
                float t = (float)i / n;
                float distance = Vector3.DistanceSquared(road.InterpolateCurve(t).Position, position);
                if (distance < bestDistance) {bestT = t; bestDistance = distance;}
            }
            return (road, t: bestT, distance: Math.Sqrt(bestDistance));
        }).OrderBy(item => item.distance).Take(8).ToArray();
        foreach (var item in nearest)
        {
            LaneGeometry geometry;
            try {geometry = Geometry(item.road);} catch (InvalidOperationException) {continue;}
            for (int lane = 0; lane < geometry.Centers.Length; ++lane)
            {
                if (!geometry.Drivable[lane]) continue;
                // Refine the projection onto this actual lane curve, rather than
                // starting the controller at the road reference line.
                float low = Math.Max(0, item.t - 4 / item.road.Length), high = Math.Min(1, item.t + 4 / item.road.Length);
                for (int n = 0; n < 20; ++n)
                {
                    float a = low + (high - low) / 3, b = high - (high - low) / 3;
                    if (Vector3.DistanceSquared(Lane.Point(item.road, a, geometry.Centers[lane]), position) < Vector3.DistanceSquared(Lane.Point(item.road, b, geometry.Centers[lane]), position)) high = b; else low = a;
                }
                float t = (low + high) / 2;
                var tangent = Vector3.Transform(-Vector3.UnitZ, item.road.InterpolateCurve(t).Rotation);
                bool increasing = Vector3.Dot(tangent, forward) > 0;
                // Only lanes whose traffic runs along the heading count (a one-way road
                // driven against its direction, or the other carriageway, does not).
                if (geometry.Forward[lane] != increasing) continue;
                double distance = Vector3.Distance(Lane.Point(item.road, t, geometry.Centers[lane]), position);
                if (selected is null || distance < selected.Distance) selected = new(item.road, null, t, lane, increasing, distance, geometry.Widths[lane]);
            }
        }
        if (selected is not null && selected.Distance <= selected.Width / 2) return selected;
        // Inside a prefab: navigation curves within 45 deg of the heading. Where the curves
        // of several movements still overlap (junction entry), the one leading to the exit
        // closest to the heading wins, so a truck going straight is not put on a turn.
        var heading = Vector2.Normalize(Lane.Flat(forward));
        var curves = new List<(StartPoint start, double turn)>();
        foreach (var prefab in Map.MapItems.Values.OfType<Prefab>().Where(p => p.Nodes.Any(n => Vector3.Distance(n.Position, position) < 400)))
        {
            var view = PrefabView.Of(this, prefab);
            if (view is null) continue;
            for (int c = 0; c < view.D.NavCurves.Count; ++c)
            {
                if (!view.Valid(c)) continue;
                int steps = Math.Max(2, (int)Math.Ceiling(view.D.NavCurves[c].Length));
                StartPoint? best = null;
                for (int i = 0; i <= steps; ++i)
                {
                    float t = (float)i / steps;
                    double distance = Vector3.Distance(view.Point(c, t), position);
                    if (distance >= (best?.Distance ?? double.PositiveInfinity) || Vector2.Dot(view.Direction(c, t), heading) < .7f) continue;
                    best = new(null, prefab, t, c, true, distance, LaneSpacing);
                }
                if (best is not null) curves.Add((best, view.ExitTurn(c, heading)));
            }
        }
        if (curves.Count > 0)
        {
            double closest = curves.Min(c => c.start.Distance);
            if (selected is null || closest < selected.Distance) selected = curves.Where(c => c.start.Distance <= closest + .5).MinBy(c => c.turn).start;
        }
        if (selected is null) throw new InvalidOperationException("No drivable lane along the heading near the current position");
        return selected;
    }
}

static class Lane
{
    public const float JoinTolerance = .5f;   // m between consecutive lane pieces that continue without blending
    public const float MergeLimit = 4.6f;     // m (one lane) from a lane end to the prefab entry curve that continues it, blended
    public const float RankStepLimit = 6f;    // m of lateral step between looks for the lane at the same rank (median width change)
    public const double JoinTurnLimit = 15;   // deg of heading kink allowed at such a join
    public static Vector3 Point(Road road, float t, float x)
    {
        var curve = road.InterpolateCurve(t);
        return curve.Position + Vector3.Transform(Vector3.UnitX, curve.Rotation) * x;
    }
    // ROS world plane [SCS X, -SCS Z].
    public static Vector2 Flat(Vector3 v) => new(v.X, -v.Z);
    public static double Turn(Vector2 a, Vector2 b) => Math.Abs(Math.Atan2(a.X * b.Y - a.Y * b.X, a.X * b.X + a.Y * b.Y)) * 180 / Math.PI;
    // Prefab nav curves are Hermite splines like road curves; tangents scale with the chord.
    public static Vector3 Curve(NavCurve c, float t, bool derivative = false)
    {
        float length = Vector3.Distance(c.StartPosition, c.EndPosition);
        var m0 = Vector3.Transform(new Vector3(0, 0, -length), c.StartRotation);
        var m1 = Vector3.Transform(new Vector3(0, 0, -length), c.EndRotation);
        return derivative ? HermiteSpline.Derivative(c.StartPosition, c.EndPosition, m0, m1, t) : HermiteSpline.Interpolate(c.StartPosition, c.EndPosition, m0, m1, t);
    }
    // Moves the first points of a new piece (sink[from..]) onto the previous end: the
    // offset gap fades out with a smoothstep over 25 m per m of gap (15-150 m, at most
    // the piece). Where the map joins lanes with a lateral step (lane width or median
    // change between looks), the published lane becomes a gentle S instead of a jump.
    public static float Blend(List<Vector3> sink, int from, Vector3 gap)
    {
        float g = gap.Length();
        var s = new float[sink.Count - from];
        for (int i = 1; i < s.Length; ++i) s[i] = s[i - 1] + Vector3.Distance(sink[from + i - 1], sink[from + i]);
        float length = Math.Min(Math.Clamp(25 * g, 15, 150), s[^1]);
        for (int i = 0; i < s.Length; ++i)
        {
            float u = length > 0 ? Math.Min(1, s[i] / length) : 1;
            sink[from + i] += gap * (1 - u * u * (3 - 2 * u));
        }
        return length;
    }
}

// A placed prefab with its descriptor: descriptor (ppd) node and curve space -> world.
sealed class PrefabView
{
    public readonly Prefab Prefab;
    public readonly PrefabDescriptor D;
    readonly ControlNode origin;
    readonly Quaternion rotation;
    PrefabView(Prefab prefab, PrefabDescriptor d)
    {
        Prefab = prefab;D = d;
        // The origin node maps onto prefab.Nodes[0].
        origin = d.Nodes[prefab.Origin];
        rotation = Quaternion.Normalize(prefab.Nodes[0].Rotation * Quaternion.Conjugate(MathEx.GetNodeRotation(origin.Direction)));
    }
    public static PrefabView? Of(World world, Prefab prefab)
    {
        var d = world.Descriptor(prefab);
        return d is null || d.Nodes.Count != prefab.Nodes.Count || d.NavCurves.Count == 0 ? null : new(prefab, d);
    }
    public int N => Prefab.Nodes.Count;
    public int Ppd(int mapIndex) => (mapIndex + Prefab.Origin) % N;
    public int MapIndex(int k) => ((k - Prefab.Origin) % N + N) % N;
    public Vector3 ToWorld(Vector3 local) => Vector3.Transform(local - origin.Position, rotation) + Prefab.Nodes[0].Position;
    public Vector3 Point(int c, float t) => ToWorld(Lane.Curve(D.NavCurves[c], t));
    public Vector2 Direction(int c, float t) => Vector2.Normalize(Lane.Flat(Vector3.Transform(Lane.Curve(D.NavCurves[c], t, true), rotation)));
    // Smallest heading change from heading to an exit reachable from curve c.
    public double ExitTurn(int c, Vector2 heading)
    {
        var outputs = D.Nodes.SelectMany(n => n.OutputLines).ToHashSet();
        double best = 180;
        var seen = new HashSet<int>();var stack = new Stack<int>([c]);
        while (stack.Count > 0)
        {
            int x = stack.Pop();
            if (!seen.Add(x)) continue;
            if (outputs.Contains(x)) {best = Math.Min(best, Lane.Turn(heading, Direction(x, 1)));continue;}
            foreach (var next in D.NavCurves[x].NextLines.Where(Valid)) stack.Push(next);
        }
        return best;
    }
    // Small-vehicle-only curves (car paths beside truck paths) are not followed.
    public bool Valid(int c) => c >= 0 && c < D.NavCurves.Count && D.NavCurves[c].AllowedVehicles != AllowedVehicles.SmallVehicles;
}

// Follows one lane through connected roads and prefabs, one map item per Step. State is
// kept by UID so the map can be reopened with other sectors.
sealed class LaneWalker
{
    readonly World world;
    public string? EndReason;
    public int PrefabCount, Choices, Blends;
    public double MaxJoin, MaxJoinTurn, MaxBlend;
    // Road still to emit, from startT in the travel direction, or a prefab curve to start on.
    ulong roadUid;float startT;bool increasing;int lane;bool roadPending;
    ulong prefabUid;int prefabCurve = -1;
    Vector3 gap; // lateral step at the last join, blended into the next road piece
    // Frontier after the last emitted item: node, the item left behind, end point, heading,
    // road look and the lane's rank among same-direction lanes counted from the left.
    ulong nodeUid, fromUid;Vector3 frontier;Vector2 heading;string roadType = "";int rank;
    // Items passed, with the direction (road) or entry node (prefab): the same item in the
    // same direction again means the rules circle (loop); the other way round is allowed.
    readonly HashSet<(ulong, int)> visited = new();
    readonly Queue<(ulong, int)> visitOrder = new();
    public LaneWalker(World world, StartPoint start, Vector3 position, Vector3 forward)
    {
        this.world = world;startT = start.T;lane = start.Lane;increasing = start.Increasing;
        if (start.Road is Road road)
        {
            roadUid = road.Uid;roadPending = true;Visit(road.Uid, increasing ? 1 : 0);
            rank = Array.IndexOf(world.Geometry(road).Lanes(increasing), lane);
        }
        else {prefabUid = start.Prefab!.Uid;prefabCurve = start.Lane;frontier = position;heading = Vector2.Normalize(Lane.Flat(forward));}
    }
    bool Visit(ulong uid, int way)
    {
        if (!visited.Add((uid, way))) return false;
        visitOrder.Enqueue((uid, way));
        if (visitOrder.Count > 10000) visited.Remove(visitOrder.Dequeue());
        return true;
    }
    bool End(string reason)
    {
        EndReason = reason;
        Console.Error.WriteLine($"end {reason} at node {nodeUid:x} after {fromUid:x}, SCS ({frontier.X:F1}, {frontier.Y:F1}, {frontier.Z:F1})");
        return false;
    }
    // Appends the next item's lane points (SCS world) and events (sink index, type). False once the lane ends.
    public bool Step(List<Vector3> sink, List<(int index, string type)> events)
    {
        for (int attempt = 0; EndReason is null; ++attempt)
        {
            if (attempt == 4) return End("map_unresolved");
            bool? done;
            if (prefabCurve >= 0)
            {
                if (!world.Map.MapItems.TryGetValue(prefabUid, out var item) || item is not Prefab prefab) done = world.Require([frontier]) ? null : End("map_unresolved");
                else done = CrossPrefab(prefab, null, sink, events);
            }
            else done = roadPending ? EmitRoad(sink) : Advance(sink, events);
            if (done is not null) return done.Value;
        }
        return false;
    }
    bool? EmitRoad(List<Vector3> sink)
    {
        if (!world.Map.MapItems.TryGetValue(roadUid, out var item) || item is not Road road)
            return world.Require([frontier], Map.SectorSize / 2) ? null : End("map_unresolved");
        var geometry = world.Geometry(road);
        float end = increasing ? 1 : 0;
        int steps = Math.Max(1, (int)Math.Ceiling(Math.Abs(end - startT) * road.Length / 1.5));
        int from = sink.Count;
        for (int i = 0; i <= steps; ++i) sink.Add(Lane.Point(road, startT + (end - startT) * i / steps, geometry.Centers[lane]));
        if (gap != Vector3.Zero) {Lane.Blend(sink, from, gap);gap = Vector3.Zero;}
        var tangent = Vector3.Transform(-Vector3.UnitZ, road.InterpolateCurve(end).Rotation);
        nodeUid = (increasing ? road.ForwardNode : road.Node).Uid;fromUid = road.Uid;roadType = road.RoadType.ToString();
        frontier = sink[^1];heading = Vector2.Normalize(Lane.Flat(increasing ? tangent : -tangent));roadPending = false;
        return true;
    }
    static IMapObject? Other(INode node, ulong from) =>
        node.ForwardItem?.Uid == from ? node.BackwardItem : node.BackwardItem?.Uid == from ? node.ForwardItem : null;
    bool? Advance(List<Vector3> sink, List<(int, string)> events)
    {
        if (!world.Map.Nodes.TryGetValue(nodeUid, out var node)) return world.Require([frontier]) ? null : End("map_unresolved");
        if (node.ForwardItem?.Uid != fromUid && node.BackwardItem?.Uid != fromUid) return End("ambiguous");
        var next = Other(node, fromUid);
        if (next is null) return End("road_end");
        // Unresolved reference: the item lies in a sector that is not loaded yet.
        if (next is not MapItem) return world.Require([node.Position]) ? null : End("map_unresolved");
        if (next is Road road) return EnterRoad(road, node, sink);
        if (next is Prefab prefab) return CrossPrefab(prefab, node, sink, events);
        return End("unsupported_item");
    }
    // The lane continues on the next road's same-direction lane that starts at this lane's
    // end. Where lane offsets or the median change between looks, no lane starts within
    // 0.5 m: the lane at the same rank from the left continues, and the step (up to 6 m)
    // is blended. A lane change never happens here.
    bool? EnterRoad(Road road, INode node, List<Vector3> sink)
    {
        bool nextIncreasing = road.Node.Uid == node.Uid;
        if (!nextIncreasing && road.ForwardNode.Uid != node.Uid) return End("ambiguous");
        LaneGeometry geometry;
        try {geometry = world.Geometry(road);} catch (InvalidOperationException) {return End("unsupported_road");}
        float t = nextIncreasing ? 0 : 1;
        var lanes = geometry.Lanes(nextIncreasing);
        if (lanes.Length == 0) return End("direction_change");
        var distances = lanes.Select(i => (double)Vector2.Distance(Lane.Flat(Lane.Point(road, t, geometry.Centers[i])), Lane.Flat(frontier))).ToArray();
        int pick = Array.IndexOf(distances, distances.Min());
        if (distances[pick] > Lane.JoinTolerance)
        {
            pick = Math.Clamp(rank, 0, lanes.Length - 1);
            if (distances[pick] > Lane.RankStepLimit)
            {
                Console.Error.WriteLine($"{road.RoadType}: lane at rank {pick} {distances[pick]:F2} m from the lane end [{geometry.Describe()}]");
                return End("lane_ends");
            }
        }
        var tangent = Vector3.Transform(-Vector3.UnitZ, road.InterpolateCurve(t).Rotation);
        double turn = Lane.Turn(heading, Lane.Flat(nextIncreasing ? tangent : -tangent));
        if (turn > Lane.JoinTurnLimit) return End("lane_ends");
        if (!Visit(road.Uid, nextIncreasing ? 1 : 0)) return End("loop");
        var start = Lane.Point(road, t, geometry.Centers[lanes[pick]]);
        if (distances[pick] > Lane.JoinTolerance) {gap = frontier - start;++Blends;MaxBlend = Math.Max(MaxBlend, distances[pick]);Console.Error.WriteLine($"{roadType}->{road.RoadType}: lane step {distances[pick]:F2} m blended, rank {rank}");}
        else MaxJoin = Math.Max(MaxJoin, distances[pick]);
        MaxJoinTurn = Math.Max(MaxJoinTurn, turn);
        roadUid = road.Uid;startT = t;lane = lanes[pick];increasing = nextIncreasing;roadPending = true;rank = pick;
        return EmitRoad(sink);
    }
    // Continues along the prefab's navigation curves. The through exit is, among the exits
    // within 30 deg of the heading, the one with most lanes leaving it, then the same road
    // look as the road before, then the smallest heading change; exits within 2 deg of it
    // with equal lanes and look are a split. When this lane reaches the through exit (or a
    // split branch), the branch keeping the lane's rank from the left wins. Otherwise
    // (junction without a straight-on carriageway, exit-only lane, curve inside the prefab
    // the truck started on) the lane follows its exit with the smallest heading change.
    // Traffic lights and toll gates on the chosen curves are reported as events.
    bool? CrossPrefab(Prefab prefab, INode? node, List<Vector3> sink, List<(int index, string type)> events)
    {
        if (prefab.Ferry is not null || prefab.IsFerryEntrance) return End("ferry");
        if (world.Require(prefab.Nodes.Select(n => n.Position))) return null;
        var view = PrefabView.Of(world, prefab);
        if (view is null)
        {
            var desc = world.Descriptor(prefab);
            Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: {(desc is null ? "no descriptor" : $"descriptor with {desc.Nodes.Count} nodes / {desc.NavCurves.Count} curves for {prefab.Nodes.Count} map nodes")}");
            // A piece without navigation curves (crossing, bridge, decoration or minor
            // junction inserted into a road) is crossed at the same lateral offset.
            return node is not null ? PassThrough(prefab, node, sink) : End("prefab_unsupported");
        }
        var d = view.D;
        int n = view.N, kIn = -1, start;
        float fromT = 0;
        double join = 0, joinTurn = 0;
        int[] inputs;
        var entryPoint = Lane.Flat(frontier);
        if (node is not null)
        {
            int entry = prefab.Nodes.FindIndex(x => x.Uid == node.Uid);
            if (entry < 0) return End("ambiguous");
            kIn = view.Ppd(entry);
            inputs = d.Nodes[kIn].InputLines.Where(view.Valid).Distinct().ToArray();
            if (inputs.Length == 0) return End("prefab_no_lane");
            start = -1;
        }
        else
        {
            // Path start inside this prefab: on curve prefabCurve at startT.
            start = prefabCurve;fromT = startT;inputs = [start];prefabCurve = -1;
            heading = view.Direction(start, fromT);
        }
        var exitOf = new Dictionary<int, int>();
        for (int k = 0; k < n; ++k) foreach (var c in d.Nodes[k].OutputLines.Where(view.Valid)) exitOf.TryAdd(c, k);
        var paths = new List<(int start, List<int> curves, int exit, float length)>();
        void Walk(List<int> path)
        {
            if (paths.Count > 4096 || path.Count > 48) return;
            int last = path[^1];
            if (exitOf.TryGetValue(last, out var k)) {if (k != kIn) paths.Add((path[0], path, k, path.Sum(c => d.NavCurves[c].Length)));return;}
            foreach (var next in d.NavCurves[last].NextLines.Where(view.Valid).Distinct()) if (!path.Contains(next)) Walk([..path, next]);
        }
        foreach (var c in inputs) Walk([c]);
        if (node is not null)
        {
            // This lane's entry curve starts where the road lane ended. Where the prefab has
            // no curve for it (the lane ends at the prefab) or its curve leads nowhere, the
            // nearest entry curve that reaches an exit, up to one lane away, takes over (merge).
            var reachable = paths.Select(p => p.start).ToHashSet();
            var ranked = inputs.OrderBy(c => Vector2.Distance(Lane.Flat(view.Point(c, 0)), entryPoint)).ToList();
            start = ranked.Where(reachable.Contains).DefaultIfEmpty(ranked[0]).First();
            join = Vector2.Distance(Lane.Flat(view.Point(start, 0)), entryPoint);
            joinTurn = Lane.Turn(heading, view.Direction(start, 0));
            if (join > Lane.MergeLimit || joinTurn > Lane.JoinTurnLimit)
            {
                Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: entry curve {start} {join:F2} m, {joinTurn:F1} deg from the lane end ({inputs.Length} entry curves)");
                return End("prefab_lane_mismatch");
            }
        }
        // Lateral position, left of the travel direction positive; lanes closer than 1 m count once.
        static double Left(Vector2 p, Vector2 o, Vector2 dir) => Vector2.Dot(p - o, new Vector2(-dir.Y, dir.X));
        static int Rank(double value, IEnumerable<double> all) => Lanes(all.Where(v => v > value + 1));
        static int Lanes(IEnumerable<double> all) {int count = 0;double last = double.NegativeInfinity;foreach (var v in all.Order()) {if (v > last + 1) ++count;last = v;}return count;}
        var entryHeading = heading;
        var exits = paths.GroupBy(p => p.exit).Select(g =>
        {
            // Lanes and ranks count the whole carriageway leaving this node, also lanes
            // joining from other entries (an on-ramp merging into the motorway).
            var dir = view.Direction(g.First().curves[^1], 1);
            var o = Lane.Flat(view.ToWorld(d.Nodes[g.Key].Position));
            var offsets = d.Nodes[g.Key].OutputLines.Where(view.Valid).Distinct().ToDictionary(c => c, c => Left(Lane.Flat(view.Point(c, 1)), o, dir));
            var item = Other(prefab.Nodes[view.MapIndex(g.Key)], prefab.Uid);
            bool sameLook = item is Road r && r.RoadType.ToString() == roadType;
            return (k: g.Key, lanes: Lanes(offsets.Values), sameLook, turn: Lane.Turn(entryHeading, dir), offsets, item);
        }).OrderByDescending(e => e.lanes).ThenByDescending(e => e.sameLook).ThenBy(e => e.turn).ToList();
        string Describe() => string.Join(' ', exits.Select(e => $"{e.k}:{e.lanes}:{(e.item as Road)?.RoadType.ToString() ?? e.item?.GetType().Name ?? "none"}:{e.turn:F1}"));
        var entryOffsets = inputs.Select(c => Left(Lane.Flat(view.Point(c, 0)), entryPoint, heading)).ToArray();
        int entryRank = node is null ? 0 : Rank(Left(Lane.Flat(view.Point(start, 0)), entryPoint, heading), entryOffsets);
        var options = exits.SelectMany(e => paths.Where(p => p.start == start && p.exit == e.k)
            .Select(p => (exit: e, path: p, shift: node is null ? 0 : Math.Abs(Rank(e.offsets[p.curves[^1]], e.offsets.Values) - entryRank)))).ToList();
        if (options.Count == 0) {Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: curve {start} reaches no exit [{Describe()}]");return End("prefab_no_exit");}
        int through = exits.FindIndex(e => e.turn <= 30);
        var main = options.Where(o => through >= 0 && o.exit.lanes == exits[through].lanes && o.exit.sameLook == exits[through].sameLook && o.exit.turn - exits[through].turn < 2).ToList();
        // No straight-on carriageway for this lane: follow its smallest heading change.
        var best = main.Count > 0 ? main.OrderBy(o => o.shift).ThenBy(o => o.exit.turn).ThenBy(o => o.path.length).First()
            : options.OrderBy(o => o.exit.turn).ThenBy(o => o.shift).ThenBy(o => o.path.length).First();
        string rule = main.Count > 0 ? (main.Select(o => o.exit.k).Distinct().Count() > 1 ? "split" : "through") : through < 0 ? "smallest_turn" : "lane_leaves_through";
        var (chosenExit, chosen, _) = best;
        if (options.Select(o => o.exit.k).Distinct().Count() > 1) ++Choices;
        int exitRank = Rank(chosenExit.offsets[chosen.curves[^1]], chosenExit.offsets.Values);
        if (!Visit(prefab.Uid, kIn)) return End("loop");
        int from = sink.Count;
        for (int i = 0; i < chosen.curves.Count; ++i)
        {
            var curve = d.NavCurves[chosen.curves[i]];
            float t0 = i == 0 ? fromT : 0;
            // A traffic light or barrier on this curve: one event per prefab and type, where
            // the first such curve begins (stop line). The path itself continues.
            int id = curve.SemaphoreId;
            var semaphore = id < 0 ? null : d.Semaphores.FirstOrDefault(s => s.SemaphoreId == (uint)id);
            if (semaphore is not null && semaphore.Type is not (SemaphoreType.ModelOnly or SemaphoreType.TrafficLightVirtual))
            {
                string type = semaphore.Type is SemaphoreType.BarrierManualTimed or SemaphoreType.BarrierDistanceActivated or SemaphoreType.BarrierGas or SemaphoreType.BarrierAutomatic ? "toll_gate" : "traffic_light";
                if (!events.Any(e => e.index >= from && e.type == type)) events.Add((Math.Max(from, sink.Count - 1), type));
            }
            int steps = Math.Max(1, (int)Math.Ceiling(curve.Length * (1 - t0) / 1.5));
            for (int j = i == 0 ? 0 : 1; j <= steps; ++j) sink.Add(view.Point(chosen.curves[i], t0 + (1 - t0) * j / steps));
        }
        if (node is not null && join > .1)
        {
            float length = Lane.Blend(sink, from, frontier - sink[from]);
            ++Blends;MaxBlend = Math.Max(MaxBlend, join);
            Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: entry step {join:F2} m blended over {length:F0} m");
        }
        else MaxJoin = Math.Max(MaxJoin, join);
        Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x} at ({entryPoint.X:F1}, {entryPoint.Y:F1}): exits [{Describe()}] {rule} {chosenExit.k}, lane rank {entryRank}->{exitRank}, curves [{string.Join(',', chosen.curves)}], join {join:F3} m {joinTurn:F2} deg");
        MaxJoinTurn = Math.Max(MaxJoinTurn, joinTurn);++PrefabCount;
        nodeUid = prefab.Nodes[view.MapIndex(chosenExit.k)].Uid;fromUid = prefab.Uid;frontier = sink[^1];heading = view.Direction(chosen.curves[^1], 1);rank = exitRank;
        return true;
    }
    bool? PassThrough(Prefab prefab, INode node, List<Vector3> sink)
    {
        // Travel direction and its right side in SCS (forward -Z, right +X).
        var f0 = new Vector3(heading.X, 0, -heading.Y);
        float lateral = Vector3.Dot(frontier - node.Position, new Vector3(-f0.Z, 0, f0.X));
        // Exit direction pointing away from the entry; with several nodes, the straightest.
        Vector3 Out(INode x) {var f = Vector3.Transform(-Vector3.UnitZ, x.Rotation);f = Vector3.Normalize(new Vector3(f.X, 0, f.Z));return Vector3.Dot(f, x.Position - node.Position) < 0 ? -f : f;}
        var exit = prefab.Nodes.Where(x => x.Uid != node.Uid && (x.ForwardItem is not null || x.BackwardItem is not null)).MinBy(x => Lane.Turn(heading, Lane.Flat(Out(x))));
        if (exit is null) return End("prefab_unsupported");
        var f1 = Out(exit);
        var end = exit.Position + new Vector3(-f1.Z, 0, f1.X) * lateral;
        float length = Vector3.Distance(frontier, end);
        double turn = Lane.Turn(heading, Lane.Flat(f1));
        if (length < .01f || length > 200 || turn > 60) return End("prefab_unsupported");
        if (!Visit(prefab.Uid, prefab.Nodes.FindIndex(x => x.Uid == node.Uid))) return End("loop");
        int steps = Math.Max(1, (int)Math.Ceiling(length / 1.5));
        for (int j = 1; j <= steps; ++j) sink.Add(HermiteSpline.Interpolate(frontier, end, f0 * length, f1 * length, (float)j / steps));
        Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: crossed without curves, {length:F1} m at lateral {lateral:F2} m, turn {turn:F1} deg");
        ++PrefabCount;
        nodeUid = exit.Uid;fromUid = prefab.Uid;frontier = end;heading = Vector2.Normalize(Lane.Flat(f1));
        return true;
    }
}

// The lane beside the followed one (side -1 left, +1 right of travel): started one lane
// width across from the followed lane's centre at the truck, rolled with it, and started
// again after the followed lane resets, when it no longer runs beside the truck (a lane
// drop or a new lane) or when it ended, at most every 2 s.
sealed class Neighbour(int side, string file)
{
    public RollingLane? Lane;
    int revision;
    long retryAfter;
    public void Update(World world, RollingLane main, double truckS, float height, double x, double y, bool mainUpdated, bool reset, double distance, double keepBehind, ref int pathId)
    {
        var (cx, cy) = main.At(truckS);
        var (hx, hy) = main.Direction(truckS);
        // ROS world: left of the heading is (-hy, hx).
        double sx = cx + side * hy * main.Width, sy = cy - side * hx * main.Width;
        bool beside = false;
        if (Lane is not null && !reset)
        {
            var p = Lane.Project(sx, sy);
            beside = p.distance <= Lane.Width / 2 && Lane.EndS - p.s > 50;
            if (beside && mainUpdated && Lane.Extendable) {Lane.Fill(p.s + distance);Lane.Trim(p.s - keepBehind);Lane.Write(file, ++revision);}
        }
        if (beside) return;
        long now = Stopwatch.GetTimestamp();
        if (Lane is null && !reset && now < retryAfter) return;
        retryAfter = now + 2 * Stopwatch.Frequency;
        RollingLane? next = null;
        try
        {
            var candidate = RollingLane.Start(world, new Vector3((float)sx, height, -(float)sy), new Vector3((float)hx, 0, -(float)hy), pathId + 1);
            candidate.Fill(distance);
            // Not the followed lane again (no lane on this side) and beside it here.
            var p = candidate.Project(sx, sy);
            if (main.Project(candidate.At(p.s).x, candidate.At(p.s).y).distance > main.Width / 2 && p.distance <= candidate.Width / 2 && candidate.Points.Count >= 2) {next = candidate;++pathId;}
        }
        catch (InvalidOperationException) {}
        Lane = next;
        if (Lane is not null) Lane.Write(file, ++revision);
        else if (File.Exists(file)) File.Delete(file);
    }
}

// The published lane polyline (ROS world, arc length s from the path start) and the
// walker that extends it. Points are only appended at the end and dropped at the start.
sealed class RollingLane
{
    const double MinSpacing = .5; // joins of map pieces repeat a point within a few cm
    public readonly List<(double X, double Y, double S)> Points = new();
    public readonly List<(string Type, double S)> Events = new();
    readonly Queue<(Vector3 point, string? type)> pending = new();
    public readonly LaneWalker Walker;
    public readonly StartPoint Origin;
    public readonly float Width;
    public readonly int PathId;
    public string EndReason = "distance_limit";
    public double StartS => Points[0].S;
    public double EndS => Points.Count == 0 ? 0 : Points[^1].S;
    public bool Extendable => EndReason == "distance_limit";
    RollingLane(LaneWalker walker, StartPoint origin, int pathId) {Walker = walker;Origin = origin;Width = origin.Width;PathId = pathId;}
    public static RollingLane Start(World world, Vector3 position, Vector3 forward, int pathId)
    {
        var choice = world.Locate(position, forward);
        if (choice.Distance > choice.Width / 2) throw new InvalidOperationException($"Current vehicle is outside the nearest mapped lane (distance {choice.Distance:F3}m)");
        return new(new LaneWalker(world, choice, position, forward), choice, pathId);
    }
    // Appends lane points until the path reaches arc length target or the lane ends.
    public void Fill(double target)
    {
        var buffer = new List<Vector3>();
        var marks = new List<(int index, string type)>();
        while (true)
        {
            if (pending.Count == 0)
            {
                buffer.Clear();marks.Clear();
                if (!Walker.Step(buffer, marks)) {EndReason = Walker.EndReason!;return;}
                for (int i = 0; i < buffer.Count; ++i) pending.Enqueue((buffer[i], marks.FirstOrDefault(m => m.index == i).type));
                continue;
            }
            var (q, type) = pending.Peek();
            double x = q.X, y = -q.Z;
            if (Points.Count == 0) Points.Add((x, y, 0));
            else
            {
                double step = Math.Sqrt((x - Points[^1].X) * (x - Points[^1].X) + (y - Points[^1].Y) * (y - Points[^1].Y));
                if (step >= MinSpacing)
                {
                    if (EndS + step > target) {EndReason = "distance_limit";return;}
                    Points.Add((x, y, EndS + step));
                }
            }
            if (type is not null) Events.Add((type, EndS));
            pending.Dequeue();
        }
    }
    public void Trim(double before)
    {
        int keep = Points.FindIndex(p => p.S >= before) - 1;
        if (keep > 0) Points.RemoveRange(0, Math.Min(keep, Points.Count - 2));
        Events.RemoveAll(e => e.S < StartS);
    }
    // Point and unit direction at arc length s (clamped to the polyline).
    public (double x, double y) At(double s)
    {
        int i = Points.FindIndex(p => p.S >= s);i = i < 0 ? Points.Count - 1 : Math.Max(i, 1);
        var (ax, ay, aS) = Points[i - 1];var (bx, by, bS) = Points[i];
        double t = bS > aS ? Math.Clamp((s - aS) / (bS - aS), 0, 1) : 0;
        return (ax + t * (bx - ax), ay + t * (by - ay));
    }
    public (double x, double y) Direction(double s)
    {
        int i = Points.FindIndex(p => p.S >= s);i = i < 0 ? Points.Count - 1 : Math.Max(i, 1);
        var (ax, ay, _) = Points[i - 1];var (bx, by, _) = Points[i];
        double n = Math.Sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        return n > 0 ? ((bx - ax) / n, (by - ay) / n) : (1, 0);
    }
    // Nearest point on the polyline: arc length and distance.
    public (double s, double distance) Project(double x, double y)
    {
        (double s, double distance) best = (0, double.PositiveInfinity);
        for (int i = 0; i + 1 < Points.Count; ++i)
        {
            var (ax, ay, aS) = Points[i];var (bx, by, bS) = Points[i + 1];
            double dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy;
            double t = Math.Clamp(((x - ax) * dx + (y - ay) * dy) / length2, 0, 1);
            double distance = Math.Sqrt(Math.Pow(x - ax - t * dx, 2) + Math.Pow(y - ay - t * dy, 2));
            if (distance < best.distance) best = (aS + t * (bS - aS), distance);
        }
        return best;
    }
    // Atomic replace. A reader holding the file open makes the replace fail on Windows; retry briefly.
    public void Write(string output, int revision)
    {
        if (Points.Count < 2) throw new InvalidOperationException("No forward lane segment remains");
        string staging = output + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(staging, JsonSerializer.Serialize(new {frame_id = "world", lane_width_m = Width, points = Points.Select(p => new[] {p.X, p.Y}),
                end_reason = EndReason, revision, path_id = PathId, start_s = StartS, events = Events.Select(e => new {type = e.Type, s = e.S})}));
            for (int attempt = 0; ; ++attempt)
            {
                try {File.Move(staging, output, overwrite: true);break;}
                catch (IOException) when (attempt < 40) {Thread.Sleep(25);}
                catch (UnauthorizedAccessException) when (attempt < 40) {Thread.Sleep(25);}
            }
        }
        finally {if (File.Exists(staging)) File.Delete(staging);}
    }
}

// --audit-looks <csv>: checks the lane model of every road look used on the installed map
// against the prefab navigation curves joined to its roads (the game's own lane ends).
static class Audit
{
    sealed class Stats {public int roads, samples, matched;public double km, maxError;public List<double> errors = new();public string model = "";}
    public static void Run(World world, string output)
    {
        world.RequireAll();
        var roads = world.Map.MapItems.Values.OfType<Road>().ToList();
        var stats = new Dictionary<string, Stats>();
        string Key(Road r) => r.RoadType + (r.LeftHandTraffic ? ":lht" : "");
        foreach (var r in roads)
        {
            var key = Key(r);
            if (!stats.TryGetValue(key, out var s))
            {
                string model;
                try {model = world.Geometry(r).Describe();} catch (InvalidOperationException ex) {model = "unsupported: " + ex.Message;}
                stats[key] = s = new Stats {model = model};
            }
            s.roads++;s.km += r.Length / 1000;
        }
        var noCurves = new Dictionary<string, int>();
        foreach (var prefab in world.Map.MapItems.Values.OfType<Prefab>())
        {
            var view = PrefabView.Of(world, prefab);
            if (view is null)
            {
                // Prefabs on roads that the walker cannot follow by navigation curves.
                if (prefab.Nodes.Any(n => n.ForwardItem is Road || n.BackwardItem is Road))
                {
                    string why = $"{prefab.Nodes.Count} nodes, {(world.Descriptor(prefab) is null ? "no descriptor" : "no curves")}";
                    noCurves[why] = noCurves.GetValueOrDefault(why) + 1;
                }
                continue;
            }
            for (int i = 0; i < view.N; ++i)
            {
                var node = prefab.Nodes[i];
                var other = node.ForwardItem?.Uid == prefab.Uid ? node.BackwardItem : node.BackwardItem?.Uid == prefab.Uid ? node.ForwardItem : null;
                if (other is not Road road) continue;
                bool atEnd = road.ForwardNode.Uid == node.Uid;
                if (!atEnd && road.Node.Uid != node.Uid) continue;
                LaneGeometry g;
                try {g = world.Geometry(road);} catch (InvalidOperationException) {g = null!;}
                var c = road.InterpolateCurve(atEnd ? 1 : 0);
                var right = Vector3.Transform(Vector3.UnitX, c.Rotation);var along = Vector3.Transform(-Vector3.UnitZ, c.Rotation);
                int k = view.Ppd(i);
                foreach (var (lines, into) in new[] {(view.D.Nodes[k].InputLines, true), (view.D.Nodes[k].OutputLines, false)})
                foreach (var ci in lines.Where(view.Valid).Distinct())
                {
                    var p = view.Point(ci, into ? 0 : 1) - c.Position;
                    if (Math.Abs(Vector3.Dot(p, along)) > .3f) continue;
                    // A curve entering the prefab at the road's end node carries traffic along the road.
                    bool forward = into == atEnd;
                    float x = Vector3.Dot(p, right);
                    var s = stats[Key(road)];
                    s.samples++;
                    if (g is not null)
                    {
                        double error = Enumerable.Range(0, g.Centers.Length).Where(l => g.Forward[l] == forward).Select(l => (double)Math.Abs(g.Centers[l] - x)).DefaultIfEmpty(double.PositiveInfinity).Min();
                        if (error <= .3) {s.matched++;s.maxError = Math.Max(s.maxError, error);s.errors.Add(error);}
                    }
                }
            }
        }
        using var csv = new StreamWriter(output);
        csv.WriteLine("look,roads,length_km,samples,matched,matched_share,max_matched_error_m,median_error_m,model");
        foreach (var (key, s) in stats.OrderByDescending(x => x.Value.km))
        {
            s.errors.Sort();
            csv.WriteLine(string.Join(',', key, s.roads, s.km.ToString("F1", CultureInfo.InvariantCulture), s.samples, s.matched,
                (s.samples == 0 ? double.NaN : (double)s.matched / s.samples).ToString("F3", CultureInfo.InvariantCulture), s.maxError.ToString("F3", CultureInfo.InvariantCulture),
                (s.errors.Count == 0 ? double.NaN : s.errors[s.errors.Count / 2]).ToString("F3", CultureInfo.InvariantCulture), "\"" + s.model.Replace("\"", "'") + "\""));
        }
        var supported = stats.Values.Where(s => !s.model.StartsWith("unsupported")).ToList();
        Console.WriteLine(JsonSerializer.Serialize(new {output, looks = stats.Count, roads = roads.Count, length_km = Math.Round(roads.Sum(r => r.Length) / 1000, 1),
            supported_km = Math.Round(supported.Sum(s => s.km), 1), unsupported_km = Math.Round(stats.Values.Sum(s => s.km) - supported.Sum(s => s.km), 1),
            samples = supported.Sum(s => s.samples), matched = supported.Sum(s => s.matched), max_matched_error_m = supported.Max(s => s.maxError),
            road_prefabs_without_curves = noCurves,
            looks_below_90pct = stats.Where(x => x.Value.samples >= 20 && !x.Value.model.StartsWith("unsupported") && x.Value.matched < .9 * x.Value.samples).Select(x => $"{x.Key} {x.Value.matched}/{x.Value.samples}"),
            unsupported = stats.Where(x => x.Value.model.StartsWith("unsupported")).OrderByDescending(x => x.Value.km).Select(x => $"{x.Key} {x.Value.km:F1} km: {x.Value.model}")}));
    }
}
