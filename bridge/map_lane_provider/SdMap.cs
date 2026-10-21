using System.Diagnostics;
using System.Numerics;
using System.Runtime.InteropServices;
using TruckLib.HashFs;
using TruckLib.Models.Ppd;
using TruckLib.ScsMap;

// SD map: a routing graph of the whole installed map (base + map DLCs), built once from the
// archives and cached as flat arrays. A vertex is an item traversal (edge): a road in one
// travel direction, or a prefab from an entry node to an exit node along its navigation
// curves. An edge ends at a node and continues into the item on the node's other side, so
// edges chain like the lane walker's steps. Coordinates are ROS world [SCS X, -SCS Z, SCS Y].
[StructLayout(LayoutKind.Sequential)]
struct SdNode {public ulong Uid;public float X, Y, Z;public int Forward, Backward;}
[StructLayout(LayoutKind.Sequential)]
struct SdItem {public ulong Uid;public int Kind;}
[StructLayout(LayoutKind.Sequential)]
struct SdEdge
{
    public int Item, From, To;          // map item, entry and exit node (indices)
    public float Length, Cost, Speed;   // m along the polyline, s, m/s
    public float Turn;                  // deg between entry and exit heading, + left
    public int Geom, GeomCount;         // polyline (Points index): road carriageway centre, prefab best curve path
    public int Lanes, LanePoints;       // road: travel lanes; entry then exit lane ends, ranked from the left
    public int Inputs, InputCount;      // prefab: navigation curves entering at From
    public int Curves, CurveCount;      // prefab: curves of every path from those inputs to To
    public int Flags;
}
// A prefab entry curve: start point, lateral offset (left of entry heading +) and whether it
// reaches any exit / this edge's exit.
[StructLayout(LayoutKind.Sequential)]
struct SdInput {public int Curve;public float X, Y, Left;public int Flags;}
record SdPlace(string Name, string City, ulong Prefab, float X, float Y);

sealed class SdMap
{
    public const int Format = 4;              // bump when the builder changes what it stores
    public const int KindRoad = 0, KindPrefab = 1, KindOther = 2;
    public const int Reversed = 1, TrafficLight = 2, GpsAvoid = 4, Curveless = 8, Hidden = 16, NoLanes = 32;
    // Roads without AI lanes (depot and yard access): both directions on the reference line.
    public const float NoLaneSpeed = 20 / 3.6f;
    public const int ReachesAny = 1, ReachesExit = 2;
    public const float MaxSpeed = 25;         // m/s, upper bound of every edge speed (A* heuristic)
    public string Key = "";
    public float Step;
    public SdNode[] Nodes = [];
    public SdItem[] Items = [];
    public SdEdge[] Edges = [];
    public float[] Points = [];               // x, y, z per polyline point
    public float[] LanePoints = [];           // x, y per lane end
    public SdInput[] Inputs = [];
    public int[] Curves = [];
    public List<SdPlace> Companies = new(), Cities = new();
    // Derived at load: edges leaving each node (CSR), polyline bounds and lengths.
    public int[] OutStart = [], OutEdges = [];
    public float[] Bounds = [];               // min x, min y, max x, max y per edge
    public Dictionary<ulong, int> NodeByUid = new(), ItemByUid = new();

    public Vector2 Point(int i) => new(Points[3 * i], Points[3 * i + 1]);
    public Vector2 NodePos(int n) => new(Nodes[n].X, Nodes[n].Y);
    public Vector2 LanePoint(int i) => new(LanePoints[2 * i], LanePoints[2 * i + 1]);
    // The item an edge continues into at its exit node (-1: none, a dead end).
    public int Next(int e)
    {
        ref var n = ref Nodes[Edges[e].To];
        int item = Edges[e].Item;
        return n.Forward == item ? n.Backward : n.Backward == item ? n.Forward : -1;
    }
    public IEnumerable<int> Successors(int e)
    {
        int next = Next(e), to = Edges[e].To;
        if (next < 0) yield break;
        for (int i = OutStart[to]; i < OutStart[to + 1]; ++i) if (Edges[OutEdges[i]].Item == next) yield return OutEdges[i];
    }

    // Cache key: game version, the map/definition archives (name, size, time), builder format
    // and polyline step. A game update or an installed or removed map DLC rebuilds the cache.
    public static string MakeKey(string game, float step)
    {
        var parts = new List<string> {$"format {Format}", $"step {step:R}"};
        var exe = Path.Combine(game, "bin", "win_x64", "eurotrucks2.exe");
        parts.Add(File.Exists(exe) ? "exe " + FileVersionInfo.GetVersionInfo(exe).FileVersion : "exe ?");
        foreach (var name in new[] {"def.scs", "base_map.scs"}) parts.Add(Stamp(Path.Combine(game, name)));
        foreach (var file in Directory.GetFiles(game, "dlc_*.scs").Order(StringComparer.Ordinal))
        {
            try
            {
                using var reader = HashFsReader.Open(file);
                if (reader.DirectoryExists("/map/europe") || reader.DirectoryExists("/def/world")) parts.Add(Stamp(file));
            }
            catch (Exception) {}
        }
        return string.Join(';', parts);
    }
    static string Stamp(string file) {var info = new FileInfo(file);return $"{info.Name}:{info.Length}:{info.LastWriteTimeUtc.Ticks}";}

    // The cache at path when its key matches the installed game; otherwise rebuilt from the
    // archives (about 2 minutes, every sector of the map) and saved.
    public static SdMap Open(string game, string path, float step, bool rebuild, Action<string> log)
    {
        var watch = Stopwatch.StartNew();
        string key = MakeKey(game, step);
        log($"sdmap: key in {watch.ElapsedMilliseconds} ms");
        if (!rebuild && File.Exists(path))
        {
            try
            {
                watch.Restart();
                var cached = Read(path);
                if (cached.Key == key) {log($"sdmap: loaded {path} in {watch.ElapsedMilliseconds} ms ({cached.Describe()})");return cached;}
                log("sdmap: cache is stale (game or map DLCs changed), rebuilding");
            }
            catch (Exception ex) {log($"sdmap: cache unreadable ({ex.Message}), rebuilding");}
        }
        watch.Restart();
        SdMap map;
        using (var world = new World(game)) map = Build(world, step, log);
        map.Key = key;
        long buildMs = watch.ElapsedMilliseconds;
        watch.Restart();
        string staging = path + "." + Environment.ProcessId + ".tmp";
        map.Write(staging);File.Move(staging, path, true);
        log($"sdmap: built in {buildMs} ms, wrote {new FileInfo(path).Length / 1e6:F1} MB in {watch.ElapsedMilliseconds} ms ({map.Describe()})");
        return map;
    }
    public string Describe() => $"{Nodes.Length} nodes, {Items.Length} items, {Edges.Length} edges, {Points.Length / 3} polyline points, {Companies.Count} companies, {Cities.Count} city areas, step {Step} m";

    // Speed by the travel lanes' traffic lane type (m/s): a truck's free speed per road class.
    // The look carries no speed limit; the lane type (traffic_lane.road.<class>[.variant]:
    // motorway, expressway, local, slow_road, no_vehicles for roads without AI traffic) is the
    // class the game's AI uses. Towns are the road's city flag (see Build).
    public static float LaneSpeed(string type)
    {
        string t = type.ToLowerInvariant();
        if (t.Contains("motorway")) return 85 / 3.6f;
        if (t.Contains("expressway")) return 80 / 3.6f;
        if (t.Contains("local")) return 65 / 3.6f;
        if (t.Contains("no_vehicles")) return 30 / 3.6f;
        return 55 / 3.6f;
    }

    public static SdMap Build(World world, float step, Action<string> log)
    {
        var watch = Stopwatch.StartNew();
        world.RequireAll();
        var source = world.Map;
        log($"sdmap: {world.Sectors} sectors, {source.MapItems.Count} items, {source.Nodes.Count} nodes opened in {watch.ElapsedMilliseconds} ms");
        var m = new SdMap {Step = step};
        var nodes = new List<SdNode>();var items = new List<SdItem>();var edges = new List<SdEdge>();
        var points = new List<float>();var lanePoints = new List<float>();var inputs = new List<SdInput>();var curves = new List<int>();
        int NodeOf(INode n)
        {
            if (m.NodeByUid.TryGetValue(n.Uid, out var i)) return i;
            m.NodeByUid[n.Uid] = i = nodes.Count;
            nodes.Add(new SdNode {Uid = n.Uid, X = n.Position.X, Y = -n.Position.Z, Z = n.Position.Y, Forward = -1, Backward = -1});
            return i;
        }
        int ItemOf(MapItem item, int kind) {m.ItemByUid[item.Uid] = items.Count;items.Add(new SdItem {Uid = item.Uid, Kind = kind});return items.Count - 1;}
        int Polyline(IEnumerable<Vector3> scs)
        {
            int start = points.Count / 3;
            foreach (var p in scs) {points.Add(p.X);points.Add(-p.Z);points.Add(p.Y);}
            return start;
        }
        static float Length(IReadOnlyList<Vector3> p) {float s = 0;for (int i = 1; i < p.Count; ++i) s += Vector2.Distance(Lane.Flat(p[i - 1]), Lane.Flat(p[i]));return s;}
        var laneKm = new Dictionary<string, double>();
        int roads = 0, unsupported = 0, avoid = 0, city = 0, laneless = 0;
        // City areas: rectangles from the item's node over its width (X) and height (Z), as map viewers draw them.
        var areas = source.MapItems.Values.OfType<CityArea>().Where(c => c.Node is not null && c.Width > 0 && c.Height > 0).Select(c => (x: c.Node.Position.X, z: c.Node.Position.Z, w: c.Width, h: c.Height)).ToList();
        // Roads: one edge per travel direction with drivable lanes; the polyline follows the centre
        // of those lanes (the carriageway), lane ends are kept for lanes_ok.
        foreach (var road in source.MapItems.Values.OfType<Road>())
        {
            LaneGeometry? g;
            // Looks without any lanes lead into most company depots (2026-10-22: 133 of 140 mainland
            // depots were unreachable without them); rail and tram tracks stay out.
            try {g = world.Geometry(road);} catch (InvalidOperationException) {if (!world.LaneLess(road)) {++unsupported;continue;}g = null;++laneless;}
            if (road.Node is null || road.ForwardNode is null) continue;
            int item = ItemOf(road, KindRoad);++roads;
            int a = NodeOf(road.Node), b = NodeOf(road.ForwardNode);
            var mid = road.InterpolateCurve(.5f).Position;
            bool inCity = areas.Any(c => mid.X >= c.x && mid.X <= c.x + c.w && mid.Z >= c.z && mid.Z <= c.z + c.h);
            if (inCity) ++city;
            foreach (bool increasing in new[] {true, false})
            {
                if (g is null)
                {
                    int count = Math.Max(1, (int)Math.Ceiling(road.Length / step));
                    var centre = Enumerable.Range(0, count + 1).Select(i => Lane.Point(road, increasing ? (float)i / count : 1 - (float)i / count, 0)).ToList();
                    float centreLength = Length(centre);
                    edges.Add(new SdEdge {Item = item, From = increasing ? a : b, To = increasing ? b : a, Length = centreLength, Cost = centreLength / NoLaneSpeed, Speed = NoLaneSpeed,
                        Turn = (float)SignedTurn(Lane.Flat(centre[1] - centre[0]), Lane.Flat(centre[^1] - centre[^2])), Geom = Polyline(centre), GeomCount = centre.Count,
                        LanePoints = lanePoints.Count / 2, Flags = (increasing ? 0 : Reversed) | NoLanes | (road.ShowInUiMap ? 0 : Hidden)});
                    continue;
                }
                var lanes = g.Lanes(increasing);
                if (lanes.Length == 0) continue;
                float offset = lanes.Average(l => g.Centers[l]);
                int n = Math.Max(1, (int)Math.Ceiling(road.Length / step));
                var line = Enumerable.Range(0, n + 1).Select(i => Lane.Point(road, increasing ? (float)i / n : 1 - (float)i / n, offset)).ToList();
                // Lane types carry no town class: a road in a city area (the map's town rectangles) is capped at 45 km/h.
                float speed = Math.Min(lanes.Max(l => LaneSpeed(g.Types[l])), inCity ? 45 / 3.6f : SdMap.MaxSpeed);
                foreach (var l in lanes) laneKm[g.Types[l]] = laneKm.GetValueOrDefault(g.Types[l]) + road.Length / 1000;
                int lp = lanePoints.Count / 2;
                foreach (float t in new[] {increasing ? 0f : 1f, increasing ? 1f : 0f})
                foreach (var l in lanes) {var p = Lane.Point(road, t, g.Centers[l]);lanePoints.Add(p.X);lanePoints.Add(-p.Z);}
                float length = Length(line);
                int flags = (increasing ? 0 : Reversed) | (road.GpsAvoid ? GpsAvoid : 0) | (road.ShowInUiMap ? 0 : Hidden);
                if (road.GpsAvoid) ++avoid;
                // The game's navigation avoids these (service and yard roads, closures): 3x the time.
                float cost = length / speed * (road.GpsAvoid ? 3 : 1);
                var tangent0 = line[1] - line[0];var tangent1 = line[^1] - line[^2];
                edges.Add(new SdEdge {Item = item, From = increasing ? a : b, To = increasing ? b : a, Length = length, Cost = cost, Speed = speed,
                    Turn = (float)SignedTurn(Lane.Flat(tangent0), Lane.Flat(tangent1)), Geom = Polyline(line), GeomCount = line.Count, Lanes = lanes.Length, LanePoints = lp, Flags = flags});
            }
        }
        log($"sdmap: {roads} roads ({city} in {areas.Count} city areas, {unsupported} without drivable lanes skipped, {laneless} without lanes at 20 km/h, {avoid} gps-avoid directions) in {watch.ElapsedMilliseconds} ms");
        foreach (var (type, km) in laneKm.OrderByDescending(x => x.Value)) log($"  lane type {type}: {km:F0} km, {LaneSpeed(type) * 3.6:F0} km/h");
        // Prefabs: every entry node to every exit its navigation curves reach (small-vehicle-only
        // curves excluded, as in the walker); a prefab without curves is crossed like the walker's
        // PassThrough, to its straightest other node.
        int prefabs = 0, curveless = 0, ferries = 0, noDescriptor = 0;
        var roadSpeed = new Func<INode, Prefab, float>((node, prefab) =>
        {
            var other = node.ForwardItem?.Uid == prefab.Uid ? node.BackwardItem : node.ForwardItem;
            if (other is Road r) try {var g = world.Geometry(r);return Enumerable.Range(0, g.Types.Length).Where(i => g.Drivable[i]).Select(i => LaneSpeed(g.Types[i])).DefaultIfEmpty(50 / 3.6f).Max();} catch (InvalidOperationException) {}
            return 50 / 3.6f;
        });
        foreach (var prefab in source.MapItems.Values.OfType<Prefab>())
        {
            if (prefab.Nodes is null || prefab.Nodes.Count == 0 || prefab.Nodes.Any(n => n is not Node)) continue;
            int item = ItemOf(prefab, KindPrefab);++prefabs;
            foreach (var n in prefab.Nodes) NodeOf(n);
            if (prefab.Ferry is not null || prefab.IsFerryEntrance) {++ferries;continue;}
            var view = PrefabView.Of(world, prefab);
            if (view is null)
            {
                if (world.Descriptor(prefab) is null) ++noDescriptor;
                foreach (var node in prefab.Nodes)
                {
                    var f0 = Lane.Flat(Vector3.Transform(-Vector3.UnitZ, node.Rotation));
                    Vector2 Out(INode x) {var f = Lane.Flat(Vector3.Transform(-Vector3.UnitZ, x.Rotation));f = Vector2.Normalize(f);return Vector2.Dot(f, Lane.Flat(x.Position - node.Position)) < 0 ? -f : f;}
                    var into = Vector2.Normalize(Vector2.Dot(f0, Lane.Flat(prefab.Nodes.Aggregate(Vector3.Zero, (s, x) => s + x.Position) / prefab.Nodes.Count - node.Position)) < 0 ? -f0 : f0);
                    var exit = prefab.Nodes.Where(x => x.Uid != node.Uid && (x.ForwardItem is not null || x.BackwardItem is not null)).MinBy(x => Lane.Turn(into, Out(x)));
                    if (exit is null) continue;
                    float length = Vector2.Distance(Lane.Flat(node.Position), Lane.Flat(exit.Position));
                    double turn = Lane.Turn(into, Out(exit));
                    if (length < .01f || length > 200 || turn > 60) continue;
                    var line = new List<Vector3> {node.Position, exit.Position};
                    float speed = Math.Min(roadSpeed(node, prefab), roadSpeed(exit, prefab));
                    edges.Add(new SdEdge {Item = item, From = NodeOf(node), To = NodeOf(exit), Length = length, Cost = length / speed, Speed = speed, Turn = (float)SignedTurn(into, Out(exit)),
                        Geom = Polyline(line), GeomCount = 2, Flags = Curveless});
                    ++curveless;
                }
                continue;
            }
            var d = view.D;
            var exitOf = new Dictionary<int, int>();
            for (int k = 0; k < view.N; ++k) foreach (var c in d.Nodes[k].OutputLines.Where(view.Valid)) exitOf.TryAdd(c, k);
            for (int i = 0; i < view.N; ++i)
            {
                int kIn = view.Ppd(i);
                var entry = d.Nodes[kIn].InputLines.Where(view.Valid).Distinct().ToArray();
                if (entry.Length == 0) continue;
                var paths = new List<(int start, List<int> curves, int exit, float length)>();
                void Walk(List<int> path)
                {
                    if (paths.Count > 4096 || path.Count > 48) return;
                    int last = path[^1];
                    if (exitOf.TryGetValue(last, out var k)) {if (k != kIn) paths.Add((path[0], path, k, path.Sum(c => d.NavCurves[c].Length)));return;}
                    foreach (var next in d.NavCurves[last].NextLines.Where(view.Valid).Distinct()) if (!path.Contains(next)) Walk([..path, next]);
                }
                foreach (var c in entry) Walk([c]);
                if (paths.Count == 0) continue;
                var heading = view.Direction(entry[0], 0);
                var origin = Lane.Flat(prefab.Nodes[i].Position);
                var reachesAny = paths.Select(p => p.start).ToHashSet();
                var inNode = prefab.Nodes[i];
                foreach (var group in paths.GroupBy(p => p.exit))
                {
                    var best = group.MinBy(p => p.length);
                    var outNode = prefab.Nodes[view.MapIndex(group.Key)];
                    var reachesExit = group.Select(p => p.start).ToHashSet();
                    int firstInput = inputs.Count;
                    foreach (var c in entry)
                    {
                        var p = Lane.Flat(view.Point(c, 0));
                        inputs.Add(new SdInput {Curve = c, X = p.X, Y = p.Y, Left = Vector2.Dot(p - origin, new Vector2(-heading.Y, heading.X)),
                            Flags = (reachesAny.Contains(c) ? ReachesAny : 0) | (reachesExit.Contains(c) ? ReachesExit : 0)});
                    }
                    int firstCurve = curves.Count;
                    curves.AddRange(group.SelectMany(p => p.curves).Distinct().Order());
                    var line = new List<Vector3>();
                    foreach (var c in best.curves)
                    {
                        int steps = Math.Max(1, (int)Math.Ceiling(d.NavCurves[c].Length / step));
                        for (int j = line.Count == 0 ? 0 : 1; j <= steps; ++j) line.Add(view.Point(c, (float)j / steps));
                    }
                    double turn = SignedTurn(view.Direction(best.curves[0], 0), view.Direction(best.curves[^1], 1));
                    bool light = best.curves.Any(c =>
                    {
                        int id = d.NavCurves[c].SemaphoreId;
                        var s = id < 0 ? null : d.Semaphores.FirstOrDefault(x => x.SemaphoreId == (uint)id);
                        return s is not null && s.Type is not (SemaphoreType.ModelOnly or SemaphoreType.TrafficLightVirtual);
                    });
                    float length = Length(line);
                    // Junction speed: the faster adjoining road, limited in a turn to 1.5 m/s^2 lateral
                    // on the radius the turn's curves imply.
                    float speed = Math.Max(roadSpeed(inNode, prefab), roadSpeed(outNode, prefab));
                    double radians = Math.Abs(turn) * Math.PI / 180;
                    if (radians > .2) speed = (float)Math.Clamp(Math.Sqrt(1.5 * length / radians), 3, speed);
                    // Turn and junction penalties (s): crossing the oncoming traffic (left in
                    // right-hand traffic) waits more than turning off; a U-turn is the last resort.
                    double signed = prefab.LeftHandTraffic ? -turn : turn;
                    double penalty = Math.Abs(turn) > 150 ? 60 : signed > 45 ? 8 : signed < -45 ? 3 : 0;
                    if (light) penalty += 10;
                    edges.Add(new SdEdge {Item = item, From = NodeOf(inNode), To = NodeOf(outNode), Length = length, Cost = (float)(length / speed + penalty), Speed = speed, Turn = (float)turn,
                        Geom = Polyline(line), GeomCount = line.Count, Inputs = firstInput, InputCount = entry.Length, Curves = firstCurve, CurveCount = curves.Count - firstCurve,
                        Flags = light ? TrafficLight : 0});
                }
            }
        }
        log($"sdmap: {prefabs} prefabs ({curveless} curveless crossings, {ferries} ferry prefabs skipped, {noDescriptor} without descriptor) in {watch.ElapsedMilliseconds} ms");
        // Which item each node joins, for chaining edges.
        for (int i = 0; i < nodes.Count; ++i)
        {
            if (!source.Nodes.TryGetValue(nodes[i].Uid, out var node)) continue;
            var n = nodes[i];
            n.Forward = node.ForwardItem is MapItem f && m.ItemByUid.TryGetValue(f.Uid, out var fi) ? fi : -1;
            n.Backward = node.BackwardItem is MapItem b && m.ItemByUid.TryGetValue(b.Uid, out var bi) ? bi : -1;
            nodes[i] = n;
        }
        foreach (var c in source.MapItems.Values.OfType<Company>())
            if (c.Node is not null && c.Prefab is not null) m.Companies.Add(new(c.CompanyName.ToString(), c.CityName.ToString(), c.Prefab.Uid, c.Node.Position.X, -c.Node.Position.Z));
        foreach (var c in source.MapItems.Values.OfType<CityArea>())
            if (c.Node is not null) m.Cities.Add(new(c.Name.ToString(), c.Name.ToString(), 0, c.Node.Position.X + c.Width / 2, -(c.Node.Position.Z + c.Height / 2)));
        m.Nodes = nodes.ToArray();m.Items = items.ToArray();m.Edges = edges.ToArray();m.Points = points.ToArray();m.LanePoints = lanePoints.ToArray();
        m.Inputs = inputs.ToArray();m.Curves = curves.ToArray();
        m.Derive();
        log($"sdmap: graph done in {watch.ElapsedMilliseconds} ms: {m.Describe()}");
        return m;
    }
    public static double SignedTurn(Vector2 a, Vector2 b) => Math.Atan2(a.X * b.Y - a.Y * b.X, a.X * b.X + a.Y * b.Y) * 180 / Math.PI;

    void Derive()
    {
        NodeByUid = new(Nodes.Length);ItemByUid = new(Items.Length);
        for (int i = 0; i < Nodes.Length; ++i) NodeByUid[Nodes[i].Uid] = i;
        for (int i = 0; i < Items.Length; ++i) ItemByUid[Items[i].Uid] = i;
        OutStart = new int[Nodes.Length + 1];
        foreach (var e in Edges) ++OutStart[e.From + 1];
        for (int i = 0; i < Nodes.Length; ++i) OutStart[i + 1] += OutStart[i];
        OutEdges = new int[Edges.Length];
        var fill = OutStart[..^1].ToArray();
        for (int e = 0; e < Edges.Length; ++e) OutEdges[fill[Edges[e].From]++] = e;
        Bounds = new float[Edges.Length * 4];
        for (int e = 0; e < Edges.Length; ++e)
        {
            float x0 = float.MaxValue, y0 = float.MaxValue, x1 = float.MinValue, y1 = float.MinValue;
            for (int i = Edges[e].Geom; i < Edges[e].Geom + Edges[e].GeomCount; ++i)
            {
                float x = Points[3 * i], y = Points[3 * i + 1];
                x0 = Math.Min(x0, x);y0 = Math.Min(y0, y);x1 = Math.Max(x1, x);y1 = Math.Max(y1, y);
            }
            Bounds[4 * e] = x0;Bounds[4 * e + 1] = y0;Bounds[4 * e + 2] = x1;Bounds[4 * e + 3] = y1;
        }
    }

    // Flat little-endian arrays behind a header: loading is a few block reads with no parsing
    // (JSON of the same graph would be several times larger and take seconds to parse).
    const string Magic = "ETS2-SDMAP";
    public void Write(string path)
    {
        using var w = new BinaryWriter(new BufferedStream(File.Create(path), 1 << 20));
        w.Write(Magic);w.Write(Format);w.Write(Key);w.Write(Step);
        Put(w, Nodes);Put(w, Items);Put(w, Edges);Put(w, Points);Put(w, LanePoints);Put(w, Inputs);Put(w, Curves);
        foreach (var list in new[] {Companies, Cities})
        {
            w.Write(list.Count);
            foreach (var p in list) {w.Write(p.Name);w.Write(p.City);w.Write(p.Prefab);w.Write(p.X);w.Write(p.Y);}
        }
    }
    public static SdMap Read(string path)
    {
        using var r = new BinaryReader(new BufferedStream(File.OpenRead(path), 1 << 20));
        if (r.ReadString() != Magic || r.ReadInt32() != Format) throw new InvalidDataException("not an SD map cache of this format");
        var m = new SdMap {Key = r.ReadString(), Step = r.ReadSingle()};
        m.Nodes = Get<SdNode>(r);m.Items = Get<SdItem>(r);m.Edges = Get<SdEdge>(r);m.Points = Get<float>(r);m.LanePoints = Get<float>(r);m.Inputs = Get<SdInput>(r);m.Curves = Get<int>(r);
        foreach (var list in new[] {m.Companies, m.Cities})
        {
            int n = r.ReadInt32();
            for (int i = 0; i < n; ++i) list.Add(new(r.ReadString(), r.ReadString(), r.ReadUInt64(), r.ReadSingle(), r.ReadSingle()));
        }
        m.Derive();
        return m;
    }
    static void Put<T>(BinaryWriter w, T[] a) where T : unmanaged {w.Write(a.Length);w.Write(MemoryMarshal.AsBytes(a.AsSpan()));}
    static T[] Get<T>(BinaryReader r) where T : unmanaged
    {
        var a = new T[r.ReadInt32()];
        r.BaseStream.ReadExactly(MemoryMarshal.AsBytes(a.AsSpan()));
        return a;
    }
}
