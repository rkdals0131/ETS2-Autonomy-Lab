using System.Diagnostics;
using System.IO.Pipes;
using System.Numerics;
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
        var placement = state.GetProperty("sdk").GetProperty("truck.world.placement").GetProperty("value");
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
        Console.WriteLine(JsonSerializer.Serialize(new {output, road_uid = lane.Origin.road.Uid.ToString("x"), road_type = lane.Origin.road.RoadType.ToString(), lane_index = lane.Origin.lane, road_direction = lane.Origin.increasing ? "forward" : "backward",
            projection_distance_m = lane.Origin.distance, point_count = lane.Points.Count, length_m = lane.EndS, end_reason = lane.EndReason, prefabs = lane.Walker.PrefabCount,
            max_join_m = lane.Walker.MaxJoin, max_join_turn_deg = lane.Walker.MaxJoinTurn, sectors = world.Sectors}));
        return 0;
    }
    RollingLane? rolling = null;
    int revision = 0, pathId = 0;
    string? lastError = null;
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
        }
        catch (Exception ex) {error = ex.Message;}
        // Replay mode reports every cycle so the driver can step in lockstep.
        if (updated || snapshotSource == "-" || (error is not null && error != lastError))
            Console.WriteLine(JsonSerializer.Serialize(new {revision, path_id = pathId, updated, reset, start_s = rolling?.StartS, end_s = rolling?.EndS, truck_s = rolling is null ? (double?)null : projection.s,
                ahead_m = rolling is null ? (double?)null : rolling.EndS - projection.s, offset_m = rolling is null ? (double?)null : projection.distance, point_count = rolling?.Points.Count,
                end_reason = rolling?.EndReason, prefabs = rolling?.Walker.PrefabCount, sectors = world.Sectors, elapsed_ms = Stopwatch.GetElapsedTime(started).TotalMilliseconds, error}));
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

// Forward: the lane carries traffic in the road direction (false only on the left side of a divided road).
record LaneGeometry(float Width, float[] Centers, bool[] Forward);

// Installed map assets: road looks, prefab descriptors and the loaded map sectors.
sealed class World : IDisposable
{
    readonly IHashFsReader archive, definitions, shared, baseAssets;
    readonly string game;
    readonly AssetLoader fs;
    readonly Dictionary<string, Unit> looks = new();
    readonly Dictionary<string, string> prefabPaths = new();
    readonly Dictionary<string, LaneGeometry> geometries = new();
    readonly Dictionary<string, string> unsupported = new();
    readonly Dictionary<string, PrefabDescriptor?> descriptors = new();
    readonly HashSet<SectorCoordinate> loaded = new();
    public Map Map {get; private set;} = new();
    public int Sectors => loaded.Count;
    public World(string game)
    {
        this.game = game;
        archive = HashFsReader.Open(Path.Combine(game, "base_map.scs"));
        definitions = HashFsReader.Open(Path.Combine(game, "def.scs"));
        shared = HashFsReader.Open(Path.Combine(game, "base_share.scs"));
        baseAssets = HashFsReader.Open(Path.Combine(game, "base.scs"));
        fs = new AssetLoader([definitions, shared, baseAssets, archive]);
        foreach (var file in fs.GetFiles("/def/world/").Where(f => Path.GetFileName(f).StartsWith("road_look") && f.EndsWith(".sii")))
        foreach (var unit in SiiFile.Open(file, fs).Units) looks[unit.Name] = unit;
    }
    // Prefab unit -> .ppd path, read when the lane first reaches a prefab. Parsing the
    // prefab SII takes about 5 s, so the table is cached beside the provider build,
    // keyed by the installed def.scs size and time.
    Dictionary<string, string> PrefabPaths()
    {
        if (prefabPaths.Count > 0) return prefabPaths;
        var def = new FileInfo(Path.Combine(game, "def.scs"));
        string key = $"{def.Length}:{def.LastWriteTimeUtc.Ticks}", cache = Path.Combine(AppContext.BaseDirectory, "prefab-desc-cache.json");
        try
        {
            var stored = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(cache));
            if (stored is not null && stored.Remove("", out var storedKey) && storedKey == key) {foreach (var (k, v) in stored) prefabPaths[k] = v;return prefabPaths;}
        }
        catch (Exception) {} // missing or stale cache: rebuild below
        var watch = Stopwatch.StartNew();
        foreach (var file in fs.GetFiles("/def/world/").Where(f => Path.GetFileName(f).StartsWith("prefab") && f.EndsWith(".sii")))
        foreach (var unit in SiiFile.Open(file, fs).Units)
            if (unit.Attributes.TryGetValue("prefab_desc", out var desc)) prefabPaths[unit.Name] = ((string)desc.ToString()).Trim('"');
        Console.Error.WriteLine($"prefab definitions: {prefabPaths.Count} in {watch.ElapsedMilliseconds} ms");
        try {File.WriteAllText(cache, JsonSerializer.Serialize(new Dictionary<string, string>(prefabPaths) {[""] = key}));}
        catch (Exception ex) {Console.Error.WriteLine($"prefab cache not written: {ex.Message}");}
        return prefabPaths;
    }
    public void Dispose() {archive.Dispose();definitions.Dispose();shared.Dispose();baseAssets.Dispose();}
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
        var watch = Stopwatch.StartNew();
        Map = Map.Open("/map/europe.mbd", archive, loaded.ToList());
        Console.Error.WriteLine($"map: {loaded.Count} sectors [{string.Join(' ', loaded.Select(s => $"{s.X},{s.Z}"))}] in {watch.ElapsedMilliseconds} ms");
        return true;
    }
    // Unsupported or incomplete looks always throw InvalidOperationException (cached).
    public LaneGeometry Geometry(Road road)
    {
        string key = "road." + road.RoadType;
        if (geometries.TryGetValue(key, out var cached)) return cached;
        if (unsupported.TryGetValue(key, out var reason)) throw new InvalidOperationException(reason);
        try {return geometries[key] = Build(key);}
        catch (Exception ex) {unsupported[key] = ex is InvalidOperationException ? ex.Message : $"No lane geometry adapter for {key}: {ex.Message}";throw new InvalidOperationException(unsupported[key]);}
    }
    (float min, float max) Surface(string modelPath)
    {
        var model = TruckLib.Models.Model.Open(modelPath, fs);
        var surface = model.Parts.Single(p => p.Name.ToString() == "vis").Pieces.SelectMany(p => p.Vertices).ToArray();
        return (surface.Min(v => v.Position.X), surface.Max(v => v.Position.X));
    }
    LaneGeometry Build(string key)
    {
        // One-way templates (one carriageway per road) and divided two-way templates
        // are supported. Other road layouts require their own geometry before a path can cross them.
        if (!looks.TryGetValue(key, out var unit) || !unit.Attributes.ContainsKey("template_right") || !unit.Attributes.ContainsKey("lanes_right"))
            throw new InvalidOperationException($"No lane geometry adapter for {key}");
        if (unit.Attributes.ContainsKey("lanes_left")) return BuildDivided(key, unit);
        int lanes = ((System.Collections.ICollection)unit.Attributes["lanes_right"]).Count;
        string modelPath = unit.Attributes["template_right"];
        var (min, max) = Surface(modelPath);
        float shoulderLeft = Convert.ToSingle(unit.Attributes["shoulder_space_left"], CultureInfo.InvariantCulture);
        float shoulderRight = Convert.ToSingle(unit.Attributes["shoulder_space_right"], CultureInfo.InvariantCulture);
        float width = (max - min - shoulderLeft - shoulderRight) / lanes;
        if (!(width > 0) || !float.IsFinite(width)) throw new InvalidOperationException($"Invalid lane width in {key}");
        // Right-lane indices run from the road reference outward, matching
        // lanes_right/lane_offsets_right in the SII definition.
        var centers = Enumerable.Range(0, lanes).Select(i => max - shoulderRight - width * (i + .5f)).ToArray();
        if (unit.Attributes.ContainsKey("road_offset"))
        {
            float offset = Convert.ToSingle(unit.Attributes["road_offset"], CultureInfo.InvariantCulture);
            for (int i = 0; i < centers.Length; ++i) centers[i] -= offset;
        }
        if (unit.Attributes.ContainsKey("lane_offsets_right"))
        {
            var offsets = (System.Collections.IList)unit.Attributes["lane_offsets_right"];
            for (int i = 0; i < centers.Length; ++i) centers[i] -= ((Vector2)offsets[i]!).X;
        }
        Console.Error.WriteLine($"{key}: template={modelPath}, road surface x=[{min},{max}], shoulders=[{shoulderLeft},{shoulderRight}], lanes={lanes}, width={width}, centers=[{string.Join(',', centers)}]");
        return new(width, centers, centers.Select(_ => true).ToArray());
    }
    // Divided two-way look with both carriageways in one road (ger11/ger14 motorways):
    // each side's template runs from its median edge (x = 0) outward to the outer
    // shoulder and is moved out by road_offset; the left side is mirrored and carries
    // traffic against the road direction. Templates not starting at the median edge
    // and per-lane offsets have no verified mapping and stay unsupported.
    LaneGeometry BuildDivided(string key, Unit unit)
    {
        if (!unit.Attributes.ContainsKey("template_left") || unit.Attributes.ContainsKey("lane_offsets_right") || unit.Attributes.ContainsKey("lane_offsets_left"))
            throw new InvalidOperationException($"No lane geometry adapter for {key}");
        float offset = unit.Attributes.ContainsKey("road_offset") ? Convert.ToSingle(unit.Attributes["road_offset"], CultureInfo.InvariantCulture) : 0;
        var centers = new List<float>();var forward = new List<bool>();float width = 0;
        foreach (var (side, sign) in new[] {("right", 1f), ("left", -1f)})
        {
            int lanes = ((System.Collections.ICollection)unit.Attributes["lanes_" + side]).Count;
            string modelPath = unit.Attributes["template_" + side];
            var (min, max) = Surface(modelPath);
            float shoulder = Convert.ToSingle(unit.Attributes["shoulder_space_" + side], CultureInfo.InvariantCulture);
            float w = (max - shoulder) / lanes;
            if (Math.Abs(min) > .05f) throw new InvalidOperationException($"No lane geometry adapter for {key}: {side} template {modelPath} x=[{min},{max}] is not median-aligned");
            if (!(w > 0) || !float.IsFinite(w) || (width > 0 && Math.Abs(w - width) > .01f)) throw new InvalidOperationException($"Invalid lane width in {key}");
            width = w;
            for (int i = 0; i < lanes; ++i) {centers.Add(sign * (offset + max - shoulder - w * (i + .5f)));forward.Add(sign > 0);}
            Console.Error.WriteLine($"{key} {side}: template={modelPath}, road surface x=[{min},{max}], offset={offset}, shoulder={shoulder}, lanes={lanes}, width={w}");
        }
        Console.Error.WriteLine($"{key}: centers=[{string.Join(',', centers)}]");
        return new(width, centers.ToArray(), forward.ToArray());
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
    // Nearest supported lane to the truck, oriented along its heading.
    public (Road road, float t, int lane, bool increasing, double distance) Locate(Vector3 position, Vector3 forward)
    {
        Require([position], Map.SectorSize);
        var nearest = Map.MapItems.Values.OfType<Road>().Select(road =>
        {
            float bestT = 0, bestDistance = float.PositiveInfinity;
            for (int i = 0; i <= Math.Max(1, (int)Math.Ceiling(road.Length / 2)); ++i)
            {
                float t = (float)i / Math.Max(1, (int)Math.Ceiling(road.Length / 2));
                var p = road.InterpolateCurve(t);
                float distance = Vector3.DistanceSquared(p.Position, position);
                if (distance < bestDistance) {bestT = t; bestDistance = distance;}
            }
            return (road, t: bestT, distance: Math.Sqrt(bestDistance));
        }).OrderBy(item => item.distance).Take(5).ToArray();
        (Road road, float t, int lane, bool increasing, double distance)? selected = null;
        foreach (var item in nearest)
        {
            LaneGeometry geometry;
            try {geometry = Geometry(item.road);} catch (InvalidOperationException) {continue;}
            for (int lane = 0; lane < geometry.Centers.Length; ++lane)
            {
                float t = item.t;
                // Refine the projection onto this actual lane curve, rather than
                // starting the controller at the road reference line.
                float low = Math.Max(0, t - 4 / item.road.Length), high = Math.Min(1, t + 4 / item.road.Length);
                for (int n = 0; n < 20; ++n)
                {
                    float a = low + (high - low) / 3, b = high - (high - low) / 3;
                    if (Vector3.DistanceSquared(Lane.Point(item.road, a, geometry.Centers[lane]), position) < Vector3.DistanceSquared(Lane.Point(item.road, b, geometry.Centers[lane]), position)) high = b; else low = a;
                }
                t = (low + high) / 2;
                var tangent = Vector3.Transform(-Vector3.UnitZ, item.road.InterpolateCurve(t).Rotation);
                bool increasing = Vector3.Dot(tangent, forward) > 0;
                // Only lanes whose traffic runs along the heading count (a one-way road
                // driven against its direction, or the other carriageway, does not).
                if (geometry.Forward[lane] != increasing) continue;
                double distance = Vector3.Distance(Lane.Point(item.road, t, geometry.Centers[lane]), position);
                if (selected is null || distance < selected.Value.distance) selected = (item.road, t, lane, increasing, distance);
            }
        }
        return selected ?? throw new InvalidOperationException("Current position is not on a supported one-way or divided road template");
    }
}

static class Lane
{
    public const float JoinTolerance = .5f;   // m between consecutive lane pieces (road template vs prefab curve)
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
}

// Follows one lane through connected roads and through-carriageway prefabs, one map
// item per Step. State is kept by UID so the map can be reopened with other sectors.
sealed class LaneWalker
{
    readonly World world;
    public string? EndReason;
    public int PrefabCount;
    public double MaxJoin, MaxJoinTurn;
    // Road still to emit, from startT in the travel direction.
    ulong roadUid;float startT;bool increasing;int lane;bool roadPending;
    // Frontier after the last emitted item: node, the item left behind, end point and heading.
    ulong nodeUid, fromUid;Vector3 frontier;Vector2 heading;string roadType = "";
    readonly HashSet<ulong> visited = new();
    readonly Queue<ulong> visitOrder = new();
    public LaneWalker(World world, Road road, float t, int lane, bool increasing)
    {
        this.world = world;roadUid = road.Uid;startT = t;this.lane = lane;this.increasing = increasing;roadPending = true;Visit(road.Uid);
    }
    bool Visit(ulong uid)
    {
        if (!visited.Add(uid)) return false;
        visitOrder.Enqueue(uid);
        if (visitOrder.Count > 10000) visited.Remove(visitOrder.Dequeue());
        return true;
    }
    bool End(string reason)
    {
        EndReason = reason;
        Console.Error.WriteLine($"end {reason} at node {nodeUid:x} after {fromUid:x}, SCS ({frontier.X:F1}, {frontier.Y:F1}, {frontier.Z:F1})");
        return false;
    }
    // Appends the next item's lane points (SCS world). False once the lane ends.
    public bool Step(List<Vector3> sink)
    {
        for (int attempt = 0; EndReason is null; ++attempt)
        {
            if (attempt == 4) return End("map_unresolved");
            var done = roadPending ? EmitRoad(sink) : Advance(sink);
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
        for (int i = 0; i <= steps; ++i) sink.Add(Lane.Point(road, startT + (end - startT) * i / steps, geometry.Centers[lane]));
        var tangent = Vector3.Transform(-Vector3.UnitZ, road.InterpolateCurve(end).Rotation);
        nodeUid = (increasing ? road.ForwardNode : road.Node).Uid;fromUid = road.Uid;roadType = road.RoadType.ToString();
        frontier = sink[^1];heading = Vector2.Normalize(Lane.Flat(increasing ? tangent : -tangent));roadPending = false;
        return true;
    }
    static IMapObject? Other(INode node, ulong from) =>
        node.ForwardItem?.Uid == from ? node.BackwardItem : node.BackwardItem?.Uid == from ? node.ForwardItem : null;
    bool? Advance(List<Vector3> sink)
    {
        if (!world.Map.Nodes.TryGetValue(nodeUid, out var node)) return world.Require([frontier]) ? null : End("map_unresolved");
        if (node.ForwardItem?.Uid != fromUid && node.BackwardItem?.Uid != fromUid) return End("ambiguous");
        var next = Other(node, fromUid);
        if (next is null) return End("road_end");
        // Unresolved reference: the item lies in a sector that is not loaded yet.
        if (next is not MapItem) return world.Require([node.Position]) ? null : End("map_unresolved");
        if (next is Road road) return EnterRoad(road, node, sink);
        if (next is Prefab prefab) return CrossPrefab(prefab, node, sink);
        return End("unsupported_item");
    }
    bool? EnterRoad(Road road, INode node, List<Vector3> sink)
    {
        bool nextIncreasing = road.Node.Uid == node.Uid;
        if (!nextIncreasing && road.ForwardNode.Uid != node.Uid) return End("ambiguous");
        LaneGeometry geometry;
        try {geometry = world.Geometry(road);} catch (InvalidOperationException ex) {Console.Error.WriteLine(ex.Message);return End("unsupported_road");}
        // Only lanes whose traffic leaves this node: a one-way road must start here, a
        // divided road is entered on the matching carriageway.
        if (!geometry.Forward.Contains(nextIncreasing)) return End("direction_change");
        float t = nextIncreasing ? 0 : 1;
        // The lane continues where a lane of the next road starts; lane counts may differ
        // (lane added or dropped beside this one), a lane change never happens here.
        int best = -1;double bestDistance = double.PositiveInfinity;
        for (int i = 0; i < geometry.Centers.Length; ++i)
        {
            if (geometry.Forward[i] != nextIncreasing) continue;
            double d = Vector2.Distance(Lane.Flat(Lane.Point(road, t, geometry.Centers[i])), Lane.Flat(frontier));
            if (d < bestDistance) {best = i;bestDistance = d;}
        }
        if (bestDistance > Lane.JoinTolerance) {Console.Error.WriteLine($"{road.RoadType}: nearest lane {bestDistance:F2} m from the lane end");return End("lane_ends");}
        var tangent = Vector3.Transform(-Vector3.UnitZ, road.InterpolateCurve(t).Rotation);
        double turn = Lane.Turn(heading, Lane.Flat(nextIncreasing ? tangent : -tangent));
        if (turn > Lane.JoinTurnLimit) return End("lane_ends");
        if (!Visit(road.Uid)) return End("loop");
        MaxJoin = Math.Max(MaxJoin, bestDistance);MaxJoinTurn = Math.Max(MaxJoinTurn, turn);
        roadUid = road.Uid;startT = t;lane = best;increasing = nextIncreasing;roadPending = true;
        return EmitRoad(sink);
    }
    // Continues along the prefab's navigation curves to its through carriageway:
    // among the exits reachable from the entry node, the one with most lanes, then the
    // same road look as the road before, then the smallest heading change. This lane
    // must reach that exit (an exit-only lane ends the path); where it branches inside
    // the prefab, the branch keeping the lane's position counted from the left
    // (median) side wins, so added or dropped right-hand lanes do not move the truck.
    // Exits within 2 deg of equally ranked ones are a split, resolved by the same rule.
    bool? CrossPrefab(Prefab prefab, INode node, List<Vector3> sink)
    {
        if (prefab.Ferry is not null || prefab.IsFerryEntrance) return End("ferry");
        if (world.Require(prefab.Nodes.Select(n => n.Position))) return null;
        var d = world.Descriptor(prefab);
        int n = prefab.Nodes.Count;
        if (d is null || d.Nodes.Count != n || d.NavCurves.Count == 0) return End("prefab_unsupported");
        int entry = prefab.Nodes.FindIndex(x => x.Uid == node.Uid);
        if (entry < 0) return End("ambiguous");
        int Ppd(int i) => (i + prefab.Origin) % n;
        int MapIndex(int k) => ((k - prefab.Origin) % n + n) % n;
        // Descriptor space -> world: the origin node maps onto prefab.Nodes[0].
        var origin = d.Nodes[prefab.Origin];
        var rotation = Quaternion.Normalize(prefab.Nodes[0].Rotation * Quaternion.Conjugate(MathEx.GetNodeRotation(origin.Direction)));
        Vector3 ToWorld(Vector3 local) => Vector3.Transform(local - origin.Position, rotation) + prefab.Nodes[0].Position;
        Vector2 Direction(NavCurve c, float t) => Vector2.Normalize(Lane.Flat(Vector3.Transform(Lane.Curve(c, t, true), rotation)));
        bool Valid(int c) => c >= 0 && c < d.NavCurves.Count && d.NavCurves[c].AllowedVehicles != AllowedVehicles.SmallVehicles;
        int kIn = Ppd(entry);
        var inputs = d.Nodes[kIn].InputLines.Where(Valid).Distinct().ToArray();
        if (inputs.Length == 0) return End("prefab_no_lane");
        var exitOf = new Dictionary<int, int>();
        for (int k = 0; k < n; ++k) foreach (var c in d.Nodes[k].OutputLines.Where(Valid)) exitOf.TryAdd(c, k);
        var paths = new List<(int start, List<int> curves, int exit, float length)>();
        void Walk(List<int> path)
        {
            if (paths.Count > 4096 || path.Count > 48) return;
            int last = path[^1];
            if (exitOf.TryGetValue(last, out var k)) {if (k != kIn) paths.Add((path[0], path, k, path.Sum(c => d.NavCurves[c].Length)));return;}
            foreach (var next in d.NavCurves[last].NextLines.Where(Valid).Distinct()) if (!path.Contains(next)) Walk([..path, next]);
        }
        foreach (var c in inputs) Walk([c]);
        // This lane's entry curve starts where the road lane ended.
        var entryPoint = Lane.Flat(frontier);
        int start = inputs.MinBy(c => Vector2.Distance(Lane.Flat(ToWorld(d.NavCurves[c].StartPosition)), entryPoint));
        double join = Vector2.Distance(Lane.Flat(ToWorld(d.NavCurves[start].StartPosition)), entryPoint);
        double joinTurn = Lane.Turn(heading, Direction(d.NavCurves[start], 0));
        if (join > Lane.JoinTolerance || joinTurn > Lane.JoinTurnLimit) return End("prefab_lane_mismatch");
        // Lateral position, left of the travel direction positive; lanes closer than 1 m count once.
        static double Left(Vector2 p, Vector2 o, Vector2 dir) => Vector2.Dot(p - o, new Vector2(-dir.Y, dir.X));
        static int Rank(double value, IEnumerable<double> all) => Lanes(all.Where(v => v > value + 1));
        static int Lanes(IEnumerable<double> all) {int count = 0;double last = double.NegativeInfinity;foreach (var v in all.Order()) {if (v > last + 1) ++count;last = v;}return count;}
        var exits = paths.GroupBy(p => p.exit).Select(g =>
        {
            // Lanes and ranks count the whole carriageway leaving this node, also lanes
            // joining from other entries (an on-ramp merging into the motorway).
            var dir = Direction(d.NavCurves[g.First().curves[^1]], 1);
            var o = Lane.Flat(ToWorld(d.Nodes[g.Key].Position));
            var offsets = d.Nodes[g.Key].OutputLines.Where(Valid).Distinct().ToDictionary(c => c, c => Left(Lane.Flat(ToWorld(d.NavCurves[c].EndPosition)), o, dir));
            var item = Other(prefab.Nodes[MapIndex(g.Key)], prefab.Uid);
            bool sameLook = item is Road r && r.RoadType.ToString() == roadType;
            return (k: g.Key, lanes: Lanes(offsets.Values), sameLook, turn: Lane.Turn(heading, dir), offsets, item);
        }).OrderByDescending(e => e.lanes).ThenByDescending(e => e.sameLook).ThenBy(e => e.turn).ToList();
        string Describe() => string.Join(' ', exits.Select(e => $"{e.k}:{e.lanes}:{(e.item as Road)?.RoadType.ToString() ?? e.item?.GetType().Name ?? "none"}:{e.turn:F1}"));
        if (exits.Count == 0) return End("prefab_no_exit");
        var best = exits[0];
        // A multi-lane carriageway whose only continuation is a single lane narrows into a ramp or road end.
        var entryOffsets = inputs.Select(c => Left(Lane.Flat(ToWorld(d.NavCurves[c].StartPosition)), entryPoint, heading)).ToArray();
        int entryLanes = Lanes(entryOffsets);
        if (best.turn > 30 || (best.lanes == 1 && entryLanes > 1))
        {
            Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: no through exit for {entryLanes} lanes [{Describe()}]");
            return End("prefab_no_through");
        }
        // Exits as good as the best (true split): this lane decides, without a lane
        // change, preferring the exit where it keeps its rank from the left.
        int entryRank = Rank(Left(Lane.Flat(ToWorld(d.NavCurves[start].StartPosition)), entryPoint, heading), entryOffsets);
        var options = exits.Where(e => e.lanes == best.lanes && e.sameLook == best.sameLook && e.turn - best.turn < 2)
            .SelectMany(e => paths.Where(p => p.start == start && p.exit == e.k)
                .Select(p => (exit: e, path: p, shift: Math.Abs(Rank(e.offsets[p.curves[^1]], e.offsets.Values) - entryRank))))
            .OrderBy(o => o.shift).ThenBy(o => o.exit.turn).ThenBy(o => o.path.length).ToList();
        if (options.Count == 0) {Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: lane {start} leaves the through exit [{Describe()}]");return End("lane_leaves_through");}
        if (options.Any(o => o.exit.k != options[0].exit.k && o.shift == options[0].shift))
        {
            Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x}: ambiguous exits for lane rank {entryRank} [{Describe()}]");
            return End("prefab_ambiguous");
        }
        var (through, chosen, _) = options[0];
        int exitRank = Rank(through.offsets[chosen.curves[^1]], through.offsets.Values);
        foreach (var c in chosen.curves)
        {
            int id = d.NavCurves[c].SemaphoreId;
            if (id < 0) continue;
            var semaphore = d.Semaphores.FirstOrDefault(s => s.SemaphoreId == (uint)id);
            if (semaphore is null || semaphore.Type is SemaphoreType.ModelOnly or SemaphoreType.TrafficLightVirtual) continue;
            return End(semaphore.Type is SemaphoreType.BarrierManualTimed or SemaphoreType.BarrierDistanceActivated or SemaphoreType.BarrierGas or SemaphoreType.BarrierAutomatic ? "toll_gate" : "traffic_light");
        }
        if (!Visit(prefab.Uid)) return End("loop");
        for (int i = 0; i < chosen.curves.Count; ++i)
        {
            var curve = d.NavCurves[chosen.curves[i]];
            int steps = Math.Max(1, (int)Math.Ceiling(curve.Length / 1.5));
            for (int j = i == 0 ? 0 : 1; j <= steps; ++j) sink.Add(ToWorld(Lane.Curve(curve, (float)j / steps)));
        }
        var exitNode = prefab.Nodes[MapIndex(through.k)];
        Console.Error.WriteLine($"prefab {prefab.Model} {prefab.Uid:x} at ({entryPoint.X:F1}, {entryPoint.Y:F1}): exits [{Describe()}] through {through.k}, lane rank {entryRank}->{exitRank}, curves [{string.Join(',', chosen.curves)}], join {join:F3} m {joinTurn:F2} deg");
        MaxJoin = Math.Max(MaxJoin, join);MaxJoinTurn = Math.Max(MaxJoinTurn, joinTurn);++PrefabCount;
        nodeUid = exitNode.Uid;fromUid = prefab.Uid;frontier = sink[^1];heading = Direction(d.NavCurves[chosen.curves[^1]], 1);
        return true;
    }
}

// The published lane polyline (ROS world, arc length s from the path start) and the
// walker that extends it. Points are only appended at the end and dropped at the start.
sealed class RollingLane
{
    const double MinSpacing = .5; // joins of map pieces repeat a point within a few cm
    public readonly List<(double X, double Y, double S)> Points = new();
    readonly Queue<Vector3> pending = new();
    public readonly LaneWalker Walker;
    public readonly (Road road, float t, int lane, bool increasing, double distance) Origin;
    public readonly float Width;
    public readonly int PathId;
    public string EndReason = "distance_limit";
    public double StartS => Points[0].S;
    public double EndS => Points.Count == 0 ? 0 : Points[^1].S;
    public bool Extendable => EndReason == "distance_limit";
    RollingLane(LaneWalker walker, (Road, float, int, bool, double) origin, float width, int pathId) {Walker = walker;Origin = origin;Width = width;PathId = pathId;}
    public static RollingLane Start(World world, Vector3 position, Vector3 forward, int pathId)
    {
        var choice = world.Locate(position, forward);
        var geometry = world.Geometry(choice.road);
        if (choice.distance > geometry.Width / 2) throw new InvalidOperationException($"Current vehicle is outside the nearest mapped lane (distance {choice.distance:F3}m)");
        return new(new LaneWalker(world, choice.road, choice.t, choice.lane, choice.increasing), choice, geometry.Width, pathId);
    }
    // Appends lane points until the path reaches arc length target or the lane ends.
    public void Fill(double target)
    {
        var buffer = new List<Vector3>();
        while (true)
        {
            if (pending.Count == 0)
            {
                buffer.Clear();
                if (!Walker.Step(buffer)) {EndReason = Walker.EndReason!;return;}
                foreach (var p in buffer) pending.Enqueue(p);
                continue;
            }
            var q = pending.Peek();
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
            pending.Dequeue();
        }
    }
    public void Trim(double before)
    {
        int keep = Points.FindIndex(p => p.S >= before) - 1;
        if (keep > 0) Points.RemoveRange(0, Math.Min(keep, Points.Count - 2));
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
                end_reason = EndReason, revision, path_id = PathId, start_s = StartS}));
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
