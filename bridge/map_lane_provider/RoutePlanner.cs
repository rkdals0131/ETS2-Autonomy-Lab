using System.Diagnostics;
using System.Globalization;
using System.IO.Pipes;
using System.Numerics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

// A point on an edge's polyline: arc length from the edge start, distance to the query point.
record EdgeSpot(int Edge, double S, double Distance);
// A planned route: edges in order, with the start and goal spots on the first and last.
record PlannedRoute(List<int> Edges, EdgeSpot Start, EdgeSpot Goal, double Cost, int Settled);

// Time-optimal routes on the SD map: A* over edges (an edge's successors are the edges of the
// item beyond its exit node), cost = travel time + turn and traffic light penalties, heuristic =
// straight-line distance at the fastest edge speed (admissible, so the result is optimal).
sealed class RoutePlanner(SdMap map)
{
    // Nearest point of edge e's polyline to p: arc length and distance (heading: unit travel direction there).
    public (double s, double distance, Vector2 heading) Project(int e, Vector2 p)
    {
        ref var edge = ref map.Edges[e];
        (double s, double distance, Vector2 heading) best = (0, double.PositiveInfinity, Vector2.UnitX);
        double along = 0;
        for (int i = edge.Geom + 1; i < edge.Geom + edge.GeomCount; ++i)
        {
            var a = map.Point(i - 1);var b = map.Point(i);var ab = b - a;
            float length = ab.Length();
            if (length < 1e-4f) continue;
            float t = Math.Clamp(Vector2.Dot(p - a, ab) / (length * length), 0, 1);
            double distance = Vector2.Distance(a + ab * t, p);
            if (distance < best.distance) best = (along + t * length, distance, ab / length);
            along += length;
        }
        return best;
    }
    // Edges passing within radius of p; with a heading, only travel directions within maxTurn.
    public List<EdgeSpot> Near(Vector2 p, double radius, Vector2? heading = null, double maxTurn = 60)
    {
        var found = new List<EdgeSpot>();
        for (int e = 0; e < map.Edges.Length; ++e)
        {
            if (p.X < map.Bounds[4 * e] - radius || p.Y < map.Bounds[4 * e + 1] - radius || p.X > map.Bounds[4 * e + 2] + radius || p.Y > map.Bounds[4 * e + 3] + radius) continue;
            var (s, distance, dir) = Project(e, p);
            if (distance > radius || heading is Vector2 h && Lane.Turn(h, dir) > maxTurn) continue;
            found.Add(new(e, s, distance));
        }
        return found;
    }
    double Fraction(EdgeSpot spot) {double length = map.Edges[spot.Edge].Length;return length > 0 ? Math.Clamp(spot.S / length, 0, 1) : 1;}

    // Where a job's destination depot is reached: the end of any edge entering its prefab (a yard
    // road or prefab); where no navigation curve leads in (AI traffic does not enter many yards,
    // and many company prefabs join no road), or with a radius, the roads within it (300 m by
    // default; large sites need up to 2 km). Up to 64 of them (512 further out): the nearest are often yard road
    // pieces joined to nothing (with 8, 38 depots beside a reachable road were unreachable,
    // 2026-10-22).
    public List<EdgeSpot> DepotTargets(SdPlace depot, double? around = null)
    {
        var found = new List<EdgeSpot>();
        if (around is null && map.ItemByUid.TryGetValue(depot.Prefab, out var item))
            for (int e = 0; e < map.Edges.Length; ++e) if (map.Next(e) == item) found.Add(new(e, map.Edges[e].Length, 0));
        if (found.Count == 0) found.AddRange(Near(new(depot.X, depot.Y), around ?? 300).OrderBy(t => t.Distance).Take((around ?? 300) > 300 ? 512 : 64));
        return found;
    }
    public static readonly double[] DepotRadii = [300, 1000, 2000];
    // Multi-source, multi-target A*. Sources pay the rest of their edge plus 2 s per m off it (a
    // nearby parallel road is a last resort); targets pay the part of their edge up to the spot
    // (an arrival target, fraction 1, is reached at the end of its edge).
    public PlannedRoute? Plan(List<EdgeSpot> sources, List<EdgeSpot> targets, List<Vector2> goals, bool astar = true)
    {
        int n = map.Edges.Length;
        var dist = new double[n];Array.Fill(dist, double.PositiveInfinity);
        var prev = new int[n];Array.Fill(prev, -1);
        var start = new Dictionary<int, EdgeSpot>();
        var open = new PriorityQueue<int, double>();
        double H(int e)
        {
            if (!astar) return 0;
            var to = map.NodePos(map.Edges[e].To);
            double best = double.PositiveInfinity;
            foreach (var g in goals) best = Math.Min(best, Vector2.Distance(to, g));
            return best / SdMap.MaxSpeed;
        }
        var targetOf = new Dictionary<int, EdgeSpot>();
        foreach (var t in targets) if (!targetOf.TryGetValue(t.Edge, out var other) || t.Distance < other.Distance) targetOf[t.Edge] = t;
        double bestGoal = double.PositiveInfinity;
        (int last, EdgeSpot? goal, EdgeSpot? from) found = (-1, null, null);
        foreach (var s in sources)
        {
            double cost = (1 - Fraction(s)) * map.Edges[s.Edge].Cost + 2 * s.Distance;
            if (cost < dist[s.Edge]) {dist[s.Edge] = cost;start[s.Edge] = s;open.Enqueue(s.Edge, cost + H(s.Edge));}
            // Goal ahead on the same edge.
            if (targetOf.TryGetValue(s.Edge, out var t) && t.S >= s.S)
            {
                double direct = (Fraction(t) - Fraction(s)) * map.Edges[s.Edge].Cost + 2 * s.Distance;
                if (direct < bestGoal) {bestGoal = direct;found = (s.Edge, t, s);}
            }
        }
        int settled = 0;
        while (open.TryDequeue(out int e, out double f))
        {
            if (f >= bestGoal) break;
            if (f > dist[e] + H(e) + 1e-9) continue; // stale entry
            ++settled;
            foreach (int next in map.Successors(e))
            {
                double nd = dist[e] + map.Edges[next].Cost;
                if (targetOf.TryGetValue(next, out var t))
                {
                    double arrive = dist[e] + Fraction(t) * map.Edges[next].Cost;
                    if (arrive < bestGoal) {bestGoal = arrive;found = (e, t, null);}
                }
                if (nd < dist[next]) {dist[next] = nd;prev[next] = e;open.Enqueue(next, nd + H(next));}
            }
        }
        if (found.goal is null) return null;
        var edges = new List<int>();
        if (found.from is not null) edges.Add(found.last);
        else
        {
            edges.Add(found.goal.Edge);
            for (int e = found.last; e >= 0; e = prev[e]) edges.Add(e);
            edges.Reverse();
        }
        return new(edges, start.GetValueOrDefault(edges[0]) ?? found.from!, found.goal, bestGoal, settled);
    }

    // Lane ranks (from the left of travel) of a road edge whose lanes can stay in lane into the
    // next route item: before a prefab with curves, the lanes whose entry curve (the walker's:
    // the nearest one that reaches any exit, within one lane) reaches the route's exit; before
    // a road, the lanes that continue (walker rules: within 0.5 m, else same rank within 6 m)
    // into an allowed lane there; otherwise every lane.
    public int[] LanesOk(int road, int next, int[]? nextOk)
    {
        var r = map.Edges[road];
        var all = Enumerable.Range(0, r.Lanes).ToArray();
        if (next < 0) return all;
        var x = map.Edges[next];
        var item = map.Items[x.Item];
        if (item.Kind == SdMap.KindPrefab && (x.Flags & SdMap.Curveless) == 0)
            return all.Where(rank =>
            {
                var end = map.LanePoint(r.LanePoints + r.Lanes + rank);
                var ranked = Enumerable.Range(x.Inputs, x.InputCount).OrderBy(i => Vector2.Distance(new(map.Inputs[i].X, map.Inputs[i].Y), end)).ToList();
                int pick = ranked.Where(i => (map.Inputs[i].Flags & SdMap.ReachesAny) != 0).DefaultIfEmpty(ranked[0]).First();
                return (map.Inputs[pick].Flags & SdMap.ReachesExit) != 0 && Vector2.Distance(new(map.Inputs[pick].X, map.Inputs[pick].Y), end) <= Lane.MergeLimit;
            }).ToArray();
        if (item.Kind == SdMap.KindRoad && nextOk is not null && x.Lanes > 0)
            return all.Where(rank =>
            {
                var end = map.LanePoint(r.LanePoints + r.Lanes + rank);
                var starts = Enumerable.Range(0, x.Lanes).Select(k => Vector2.Distance(map.LanePoint(x.LanePoints + k), end)).ToArray();
                int pick = Array.IndexOf(starts, starts.Min());
                if (starts[pick] > Lane.JoinTolerance) pick = Math.Clamp(rank, 0, x.Lanes - 1);
                return starts[pick] <= Lane.RankStepLimit && nextOk.Contains(pick);
            }).ToArray();
        return all;
    }
    // Ranks of the entry curves of a prefab edge that reach its exit, counted from the left like
    // the walker (curves closer than 1 m are one lane).
    public int[] EntryRanks(int e)
    {
        ref var x = ref map.Edges[e];
        var lefts = Enumerable.Range(x.Inputs, x.InputCount).Select(i => (double)map.Inputs[i].Left).ToList();
        static int Lanes(IEnumerable<double> all) {int count = 0;double last = double.NegativeInfinity;foreach (var v in all.Order()) {if (v > last + 1) ++count;last = v;}return count;}
        return Enumerable.Range(x.Inputs, x.InputCount).Where(i => (map.Inputs[i].Flags & SdMap.ReachesExit) != 0)
            .Select(i => Lanes(lefts.Where(v => v > map.Inputs[i].Left + 1))).Distinct().Order().ToArray();
    }

    // A route file of any source (the game's lists items by entry and exit node only) filled from the
    // SD map: exit_curves, entry_curves and lanes_ok as the planner writes them, lanes, and the
    // polyline along the map geometry. Items the map does not have (a gap across a ferry, an item
    // outside the installed map) are left as they are. Returns the items matched.
    public int Enrich(JsonObject route)
    {
        if (route["items"] is not JsonArray items) return 0;
        var edges = new int[items.Count];
        for (int i = 0; i < items.Count; ++i)
        {
            edges[i] = -1;
            ulong? Hex(string name) => items[i]?[name] is JsonValue v && v.TryGetValue<string>(out var s) && ulong.TryParse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var x) ? x : null;
            if (Hex("uid") is not ulong uid || Hex("entry_node") is not ulong a || Hex("exit_node") is not ulong b) continue;
            if (!map.ItemByUid.TryGetValue(uid, out var item) || !map.NodeByUid.TryGetValue(a, out var from) || !map.NodeByUid.TryGetValue(b, out var to)) continue;
            for (int k = map.OutStart[from]; k < map.OutStart[from + 1]; ++k)
                if (map.Edges[map.OutEdges[k]].Item == item && map.Edges[map.OutEdges[k]].To == to) {edges[i] = map.OutEdges[k];break;}
        }
        var ok = new int[items.Count][];
        for (int i = items.Count - 1; i >= 0; --i)
        {
            int e = edges[i];
            if (e < 0) continue;
            bool linked = i + 1 < items.Count && edges[i + 1] >= 0;
            ok[i] = map.Items[map.Edges[e].Item].Kind == SdMap.KindRoad ? LanesOk(e, linked ? edges[i + 1] : -1, linked ? ok[i + 1] : null)
                : (map.Edges[e].Flags & SdMap.Curveless) != 0 ? [] : EntryRanks(e);
        }
        var polyline = new JsonArray();
        static double R(double v) => Math.Round(v, 2);
        for (int i = 0; i < items.Count; ++i)
        {
            int e = edges[i];
            if (e < 0) continue;
            var edge = map.Edges[e];var item = items[i]!.AsObject();
            item["exit_curves"] = new JsonArray(Enumerable.Range(edge.Curves, edge.CurveCount).Select(c => (JsonNode)map.Curves[c]).ToArray());
            item["lanes_ok"] = new JsonArray(ok[i].Select(v => (JsonNode)v).ToArray());
            if (map.Items[edge.Item].Kind == SdMap.KindRoad) item["lanes"] = edge.Lanes;
            else item["entry_curves"] = new JsonArray(Enumerable.Range(edge.Inputs, edge.InputCount).Where(k => (map.Inputs[k].Flags & SdMap.ReachesExit) != 0).Select(k => (JsonNode)map.Inputs[k].Curve).ToArray());
            for (int k = edge.Geom; k < edge.Geom + edge.GeomCount; ++k)
            {
                if (k == edge.Geom && polyline.Count > 0) continue;
                polyline.Add(new JsonArray(R(map.Points[3 * k]), R(map.Points[3 * k + 1]), R(map.Points[3 * k + 2])));
            }
        }
        if (polyline.Count > 1) route["polyline"] = polyline;
        route["enriched"] = "sdmap";
        return edges.Count(e => e >= 0);
    }

    // The route file (README "경로 파일"): items with their part of the route, lanes_ok, polyline.
    public JsonObject ToJson(PlannedRoute route, Vector2 start, Vector2 goal, int revision = 1)
    {
        var items = new List<JsonObject>();
        var polyline = new JsonArray();
        static double R(double v) => Math.Round(v, 2);
        void Add(Vector3 p)
        {
            if (polyline.Count > 0 && polyline[^1] is JsonArray q && Math.Abs((double)q[0]! - R(p.X)) + Math.Abs((double)q[1]! - R(p.Y)) < .05) return;
            polyline.Add(new JsonArray(R(p.X), R(p.Y), R(p.Z)));
        }
        double total = 0, eta = 0;
        int count = route.Edges.Count;
        var ok = new int[count][];
        for (int i = count - 1; i >= 0; --i)
        {
            int e = route.Edges[i];
            ok[i] = map.Items[map.Edges[e].Item].Kind == SdMap.KindRoad ? LanesOk(e, i + 1 < count ? route.Edges[i + 1] : -1, i + 1 < count ? ok[i + 1] : null)
                : (map.Edges[e].Flags & SdMap.Curveless) != 0 ? [] : EntryRanks(e);
        }
        for (int i = 0; i < count; ++i)
        {
            int e = route.Edges[i];
            ref var edge = ref map.Edges[e];
            double from = i == 0 ? route.Start.S : 0, to = i == count - 1 ? route.Goal.S : edge.Length;
            if (count == 1) to = Math.Max(from, route.Goal.S);
            from = Math.Clamp(from, 0, edge.Length);to = Math.Clamp(to, from, edge.Length);
            // Polyline part [from, to] of the edge.
            double along = 0;
            for (int k = edge.Geom; k < edge.Geom + edge.GeomCount; ++k)
            {
                var p = new Vector3(map.Points[3 * k], map.Points[3 * k + 1], map.Points[3 * k + 2]);
                if (k > edge.Geom)
                {
                    var a = new Vector3(map.Points[3 * k - 3], map.Points[3 * k - 2], map.Points[3 * k - 1]);
                    double length = Vector2.Distance(new(a.X, a.Y), new(p.X, p.Y));
                    if (along < from && along + length > from) Add(Vector3.Lerp(a, p, (float)((from - along) / length)));
                    if (along < to && along + length >= to) {Add(Vector3.Lerp(a, p, (float)((to - along) / length)));break;}
                    along += length;
                }
                if (along >= from && along <= to) Add(p);
            }
            bool prefab = map.Items[edge.Item].Kind == SdMap.KindPrefab;
            var item = new JsonObject
            {
                ["kind"] = prefab ? "prefab" : "road",
                ["uid"] = map.Items[edge.Item].Uid.ToString("x"),
                ["entry_node"] = map.Nodes[edge.From].Uid.ToString("x"),
                ["exit_node"] = map.Nodes[edge.To].Uid.ToString("x"),
                ["exit_curves"] = new JsonArray(Enumerable.Range(edge.Curves, edge.CurveCount).Select(c => (JsonNode)map.Curves[c]).ToArray()),
                ["lanes_ok"] = new JsonArray(ok[i].Select(v => (JsonNode)v).ToArray()),
                ["s_start_m"] = R(total),
                ["length_m"] = R(to - from),
            };
            if (!prefab) item["lanes"] = edge.Lanes;
            else
            {
                item["entry_curves"] = new JsonArray(Enumerable.Range(edge.Inputs, edge.InputCount).Where(k => (map.Inputs[k].Flags & SdMap.ReachesExit) != 0).Select(k => (JsonNode)map.Inputs[k].Curve).ToArray());
                item["turn_deg"] = R(edge.Turn);
                if ((edge.Flags & SdMap.TrafficLight) != 0) item["traffic_light"] = true;
                if ((edge.Flags & SdMap.Curveless) != 0) item["curveless"] = true;
            }
            items.Add(item);
            total += to - from;
            eta += edge.Length > 0 ? (to - from) / edge.Length * edge.Cost : 0;
        }
        return new JsonObject
        {
            ["frame_id"] = "world", ["source"] = "sdmap", ["revision"] = revision, ["created"] = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture),
            ["start"] = new JsonArray(R(start.X), R(start.Y)), ["goal"] = new JsonArray(R(goal.X), R(goal.Y)),
            ["length_m"] = R(total), ["eta_s"] = R(eta), ["items"] = new JsonArray(items.ToArray()), ["polyline"] = polyline,
        };
    }
}

// --route: the route file read by the walker. At a prefab on the route it takes the route's
// exit when the lane's curves reach it; otherwise it keeps its own choice and reports
// route_lane_mismatch with the entry lanes that would. The file is re-read when it changes
// (checked at most once a second), so a planner can replace it while following.
sealed class RouteGuide
{
    // A route event on the walked lane: the prefab, its route exit node, the entry lanes (ranks
    // from the left of travel) whose curves reach it, and the lane the walker came in on.
    public record Note(string Type, ulong Prefab, ulong Exit, int[] LanesOk, int Lane);
    readonly string file;
    Dictionary<(ulong prefab, ulong entry), ulong> exits = new();
    Dictionary<ulong, ulong> byPrefab = new();
    DateTime stamp;
    long checkedAt;
    public int Revision {get; private set;}
    public string Source {get; private set;} = "";
    RouteGuide(string file)
    {
        this.file = Path.GetFullPath(file);
        // The file may come later (the game route is written once the game has one).
        if (File.Exists(this.file)) Reload(); else Console.Error.WriteLine($"route: {this.file} not there yet");
    }
    public static RouteGuide Load(string file) => new(file);
    // Any route file of the contract, whatever its source: only prefab items with a uid and their
    // entry and exit nodes matter here (a "gap" across a ferry, extra fields and a file that says
    // "unavailable" without items are fine; the latter clears the route).
    void Reload()
    {
        stamp = File.GetLastWriteTimeUtc(file);
        using var doc = JsonDocument.Parse(File.ReadAllText(file));
        var root = doc.RootElement;
        var next = new Dictionary<(ulong, ulong), ulong>();var first = new Dictionary<ulong, ulong>();
        if (root.TryGetProperty("items", out var items) && items.ValueKind == JsonValueKind.Array)
            foreach (var item in items.EnumerateArray())
            {
                if (!item.TryGetProperty("kind", out var kind) || kind.GetString() != "prefab") continue;
                ulong? Hex(string name) => item.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String && ulong.TryParse(v.GetString(), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var x) ? x : null;
                if (Hex("uid") is not ulong uid || Hex("entry_node") is not ulong entry || Hex("exit_node") is not ulong exit) continue;
                next[(uid, entry)] = exit;
                first.TryAdd(uid, exit);
            }
        exits = next;byPrefab = first;
        Revision = root.TryGetProperty("revision", out var r) && r.TryGetInt32(out var rv) ? rv : 0;
        Source = root.TryGetProperty("source", out var s) ? s.GetString() ?? "" : "";
        string unavailable = root.TryGetProperty("unavailable", out var u) ? $", unavailable: {u}" : "";
        Console.Error.WriteLine($"route: {file} {Source} revision {Revision}, {exits.Count} prefabs{unavailable}");
    }
    // The route's exit node of a prefab entered at entry (null: the path starts inside it).
    public ulong? Exit(ulong prefab, ulong? entry)
    {
        long now = Stopwatch.GetTimestamp();
        if (now - checkedAt > Stopwatch.Frequency)
        {
            checkedAt = now;
            // A missing file keeps the last route (the writer replaces it atomically).
            try {if (File.Exists(file) && File.GetLastWriteTimeUtc(file) != stamp) Reload();}
            catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException or InvalidOperationException) {Console.Error.WriteLine($"route: {ex.Message}");}
        }
        if (entry is ulong n) return exits.TryGetValue((prefab, n), out var x) ? x : null;
        return byPrefab.TryGetValue(prefab, out var y) ? y : null;
    }
    public static object Event(Note note, double s) => new {type = note.Type, s, prefab = note.Prefab.ToString("x"), exit_node = note.Exit.ToString("x"), lanes_ok = note.LanesOk, lane = note.Lane};
    // Status line with the route fields appended; unchanged without --route.
    public static string Status(World world, RollingLane? lane, string json)
    {
        if (world.Route is not RouteGuide route) return json;
        var node = JsonNode.Parse(json)!.AsObject();
        node["route_revision"] = route.Revision;
        node["route_taken"] = lane?.Walker.RouteTaken;
        node["route_events"] = lane is null ? null : JsonSerializer.SerializeToNode(lane.RouteEvents.Select(e => Event(e.Note, e.S)));
        return node.ToJsonString();
    }
}

// --plan-route, --sdmap-build, --route-selftest, --route-bench, --route-compare.
static class RouteCli
{
    public static bool Handles(Dictionary<string, string> options) =>
        new[] {"--plan-route", "--sdmap-build", "--route-selftest", "--route-bench", "--route-compare", "--enrich-route"}.Any(options.ContainsKey);
    static void Log(string text) => Console.Error.WriteLine(text);
    static Vector2 Xy(string text)
    {
        var v = text.Split(',').Select(t => double.Parse(t, CultureInfo.InvariantCulture)).ToArray();
        return new((float)v[0], (float)v[1]);
    }
    public static int Run(Dictionary<string, string> options, string game)
    {
        float step = (float)(options.TryGetValue("--sdmap-step-m", out var st) ? double.Parse(st, CultureInfo.InvariantCulture) : 10);
        if (!(step >= .5f && step <= 100)) throw new ArgumentException("--sdmap-step-m must be 0.5-100");
        string cache = options.GetValueOrDefault("--sdmap-cache") ?? Path.Combine(AppContext.BaseDirectory, "sdmap-cache.bin");
        var watch = Stopwatch.StartNew();
        var map = SdMap.Open(game, cache, step, options.ContainsKey("--sdmap-build"), Log);
        double loadMs = watch.Elapsed.TotalMilliseconds;
        var planner = new RoutePlanner(map);
        if (options.ContainsKey("--route-selftest")) return SelfTest(map, planner, cache);
        if (options.ContainsKey("--route-bench")) return Bench(map, planner, int.Parse(options.GetValueOrDefault("--route-bench") is "1" or null ? "20" : options["--route-bench"]));
        if (options.TryGetValue("--route-compare", out var poses)) return Compare(map, planner, poses, options.GetValueOrDefault("--out"));
        if (options.TryGetValue("--enrich-route", out var input))
        {
            var given = JsonNode.Parse(File.ReadAllText(input))?.AsObject() ?? throw new InvalidDataException("not a route file");
            watch.Restart();
            int matched = planner.Enrich(given);
            string target = options.GetValueOrDefault("--out") ?? input;
            RollingLane.Replace(target, given.ToJsonString());
            Console.WriteLine(JsonSerializer.Serialize(new {output = target, source = (string?)given["source"], items = (given["items"] as JsonArray)?.Count ?? 0, matched, enrich_ms = Math.Round(watch.Elapsed.TotalMilliseconds, 2), load_ms = Math.Round(loadMs, 1)}));
            return 0;
        }
        if (!options.ContainsKey("--plan-route")) {Console.WriteLine(JsonSerializer.Serialize(new {cache, load_ms = loadMs, map = map.Describe()}));return 0;}

        // Start: --from x,y,heading_deg (ROS world, heading counter-clockwise from +x) or the
        // truck placement (--snapshot file / - / the ot pipe).
        Vector2 from, heading;
        JsonElement? snapshot = null;
        if (options.TryGetValue("--from", out var f))
        {
            var v = f.Split(',').Select(t => double.Parse(t, CultureInfo.InvariantCulture)).ToArray();
            from = new((float)v[0], (float)v[1]);
            double h = (v.Length > 2 ? v[2] : 0) * Math.PI / 180;
            heading = new((float)Math.Cos(h), (float)Math.Sin(h));
        }
        else
        {
            string? source = options.GetValueOrDefault("--snapshot");
            string text = source == "-" ? Console.In.ReadLine() ?? throw new InvalidOperationException("No snapshot on stdin") : source is not null ? File.ReadAllText(source) : Pipe("snapshot");
            var root = JsonDocument.Parse(text).RootElement;
            if (root.TryGetProperty("snapshot", out var nested)) root = nested;
            snapshot = root;
            var placement = root.GetProperty("sdk").GetProperty("truck.world.placement").GetProperty("value");
            var xyz = placement.GetProperty("position_m");
            from = new(xyz[0].GetSingle(), -xyz[2].GetSingle());
            double h = placement.GetProperty("euler_rotations")[0].GetDouble() * Math.Tau;
            heading = new(-(float)Math.Sin(h), (float)Math.Cos(h));
        }
        // Goal: --to x,y, or the job destination (company in a city) from --job company,city, the
        // snapshot's "configs" (as the ot pipe's configs command returns them) or the live pipe.
        var goals = new List<Vector2>();var targets = new List<EdgeSpot>();var places = new List<SdPlace>();
        string goalText;
        if (options.TryGetValue("--to", out var to))
        {
            var g = Xy(to);goals.Add(g);
            // The nearest roads, looking further out when the point is off the road network.
            foreach (var radius in new[] {50, 500, 3000}) if (targets.Count == 0) targets.AddRange(planner.Near(g, radius).OrderBy(t => t.Distance).Take(8));
            goalText = $"point {g.X:F0},{g.Y:F0}";
        }
        else
        {
            var (company, city) = options.TryGetValue("--job", out var job) ? (job.Split(',')[0], job.Split(',').ElementAtOrDefault(1) ?? "") : JobDestination(snapshot, options.ContainsKey("--snapshot"));
            places = map.Companies.Where(c => c.Name == company && (city == "" || c.City == city)).ToList();
            goalText = $"company {company} in {city} ({places.Count} depots)";
            foreach (var p in places) {targets.AddRange(planner.DepotTargets(p));goals.Add(new(p.X, p.Y));}
            if (places.Count == 0 && city != "")
            {
                // A city without the company (or no company given): the road nearest its area centre.
                foreach (var c in map.Cities.Where(c => c.Name == city)) {var g = new Vector2(c.X, c.Y);goals.Add(g);targets.AddRange(planner.Near(g, 300).OrderBy(t => t.Distance).Take(4));}
                goalText = $"city {city}";
            }
        }
        if (targets.Count == 0) throw new InvalidOperationException($"No road near the goal ({goalText})");
        var sources = planner.Near(from, 15, heading, 45);
        if (sources.Count == 0) sources = planner.Near(from, 40, heading, 60);
        if (sources.Count == 0) throw new InvalidOperationException("No road along the heading near the start");
        watch.Restart();
        var route = planner.Plan(sources, targets, goals);
        // A depot whose way in is cut off from the start: the roads around it, further out in steps.
        foreach (var radius in RoutePlanner.DepotRadii)
            if (route is null && places.Count > 0) route = planner.Plan(sources, places.SelectMany(p => planner.DepotTargets(p, radius)).ToList(), goals);
        if (route is null) throw new InvalidOperationException($"No route to {goalText}");
        double queryMs = watch.Elapsed.TotalMilliseconds;
        var goalPoint = goals.Count == 1 ? goals[0] : goals.MinBy(g => Vector2.Distance(g, LastPoint(map, route)));
        var json = planner.ToJson(route, from, goalPoint);
        string output = options.GetValueOrDefault("--out") ?? Path.Combine(Environment.CurrentDirectory, "bridge", "recordings", "gt-path", "current-lane.json");
        string file = output.EndsWith(".route.json", StringComparison.OrdinalIgnoreCase) ? output
            : Path.Combine(Path.GetDirectoryName(Path.GetFullPath(output))!, Path.GetFileNameWithoutExtension(output) + ".route.json");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file))!);
        RollingLane.Replace(file, json.ToJsonString());
        Console.WriteLine(JsonSerializer.Serialize(new {output = file, goal = goalText, length_m = (double)json["length_m"]!, eta_s = (double)json["eta_s"]!, items = route.Edges.Count,
            prefabs = route.Edges.Count(e => map.Items[map.Edges[e].Item].Kind == SdMap.KindPrefab), sources = sources.Count, targets = targets.Count, settled = route.Settled,
            load_ms = Math.Round(loadMs, 1), query_ms = Math.Round(queryMs, 2)}));
        return 0;
    }
    static Vector2 LastPoint(SdMap map, PlannedRoute route) {var e = map.Edges[route.Edges[^1]];return map.Point(e.Geom + e.GeomCount - 1);}
    // SDK job configuration: destination.company.id / destination.city.id.
    static (string company, string city) JobDestination(JsonElement? snapshot, bool offline)
    {
        JsonElement configs;
        if (snapshot is JsonElement s && s.TryGetProperty("configs", out var c)) configs = c;
        else if (!offline) configs = JsonDocument.Parse(Pipe("configs")).RootElement;
        else throw new InvalidOperationException("Pass --to x,y or --job company,city (the snapshot has no configs.job)");
        if (!configs.TryGetProperty("job", out var job)) throw new InvalidOperationException("No job: pass --to x,y");
        string? company = null, city = null;
        foreach (var a in job.GetProperty("attributes").EnumerateArray())
        {
            string name = a.GetProperty("name").GetString() ?? "";
            if (name == "destination.company.id") company = a.GetProperty("value").GetString();
            if (name == "destination.city.id") city = a.GetProperty("value").GetString();
        }
        return (company ?? throw new InvalidOperationException("The job has no destination company"), city ?? "");
    }
    static string Pipe(string cmd)
    {
        using var pipe = new NamedPipeClientStream(".", "ot", PipeDirection.InOut, PipeOptions.Asynchronous);
        pipe.Connect(3000);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        pipe.WriteAsync(Encoding.UTF8.GetBytes($"{{\"cmd\":\"{cmd}\"}}\n"), timeout.Token).AsTask().GetAwaiter().GetResult();
        using var reader = new StreamReader(pipe, Encoding.UTF8, false, 4096, leaveOpen: true);
        using var reply = JsonDocument.Parse(reader.ReadLineAsync(timeout.Token).AsTask().GetAwaiter().GetResult() ?? throw new IOException("Core closed the pipe"));
        if (!reply.RootElement.GetProperty("ok").GetBoolean()) throw new IOException(reply.RootElement.GetProperty("error").GetString());
        return reply.RootElement.GetProperty("result").GetRawText();
    }

    // Random road positions (length weighted), heading along a road edge.
    static (Vector2 p, Vector2 heading, int edge) RandomSpot(SdMap map, Random random, int[] roads, double[] cumulative)
    {
        int k = Array.BinarySearch(cumulative, random.NextDouble() * cumulative[^1]);
        int e = roads[k < 0 ? ~k : k];
        var edge = map.Edges[e];
        int i = edge.Geom + random.Next(Math.Max(1, edge.GeomCount - 1));
        var a = map.Point(i);var b = map.Point(Math.Min(i + 1, edge.Geom + edge.GeomCount - 1));
        return ((a + b) / 2, b == a ? Vector2.UnitX : Vector2.Normalize(b - a), e);
    }
    // Road edges of the main network (random routes between scenery roads would mostly fail).
    static (int[] roads, double[] cumulative) Roads(SdMap map)
    {
        var reach = Reach(map);
        var roads = Enumerable.Range(0, map.Edges.Length).Where(e => reach[e] == 0 && map.Items[map.Edges[e].Item].Kind == SdMap.KindRoad && map.Edges[e].GeomCount >= 2 && (map.Edges[e].Flags & SdMap.GpsAvoid) == 0).ToArray();
        var cumulative = new double[roads.Length];double sum = 0;
        for (int i = 0; i < roads.Length; ++i) cumulative[i] = sum += map.Edges[roads[i]].Length;
        return (roads, cumulative);
    }
    // Query times for straight-line distance bands (5, 50, 500+ km) between random road positions.
    static int Bench(SdMap map, RoutePlanner planner, int perBand)
    {
        var random = new Random(7);
        var (roads, cumulative) = Roads(map);
        // World metres: the game world is about 1:19 of its map scale outside towns (a 50 km world route
        // is some 950 km on the in-game map), and the installed map spans about 100 km.
        foreach (var (low, high) in new[] {(4e3, 6e3), (40e3, 60e3), (80e3, 400e3)})
        {
            var times = new List<double>();var settled = new List<int>();var ratios = new List<double>();var lengths = new List<double>();int failed = 0;
            for (int tries = 0; times.Count + failed < perBand && tries < perBand * 2000; ++tries)
            {
                var a = RandomSpot(map, random, roads, cumulative);var b = RandomSpot(map, random, roads, cumulative);
                double d = Vector2.Distance(a.p, b.p);
                if (d < low || d > high) continue;
                var sources = planner.Near(a.p, 15, a.heading, 45);var targets = planner.Near(b.p, 50).OrderBy(t => t.Distance).Take(8).ToList();
                var watch = Stopwatch.StartNew();
                var route = planner.Plan(sources, targets, [b.p]);
                double ms = watch.Elapsed.TotalMilliseconds;
                if (route is null) {++failed;continue;}
                times.Add(ms);settled.Add(route.Settled);
                ratios.Add(route.Edges.Sum(e => map.Edges[e].Length) / d);lengths.Add(route.Edges.Sum(e => map.Edges[e].Length) / 1000);
            }
            times.Sort();settled.Sort();
            double? Q(List<double> v, double q) => v.Count == 0 ? null : Math.Round(v[Math.Min(v.Count - 1, (int)(q * v.Count))], 3);
            Console.WriteLine(JsonSerializer.Serialize(new {band_km = $"{low / 1e3}-{high / 1e3}", routes = times.Count, no_route = failed, median_ms = Q(times, .5), p95_ms = Q(times, .95),
                max_ms = Q(times, 1), median_settled = settled.Count == 0 ? 0 : settled[settled.Count / 2], median_detour = Q(ratios.Order().ToList(), .5), median_route_km = Q(lengths.Order().ToList(), .5), max_route_km = Q(lengths.Order().ToList(), 1)}));
        }
        return 0;
    }
    // Plans from the first to the last recorded pose (lines {"X","Z","h"} SCS, or poses_f style
    // ROS poses) and reports the share of driven positions within 10 m of the route polyline.
    static int Compare(SdMap map, RoutePlanner planner, string posesFile, string? output)
    {
        var poses = new List<(Vector2 p, Vector2 heading)>();
        foreach (var line in File.ReadLines(posesFile))
        {
            if (line.Trim().Length == 0) continue;
            var o = JsonDocument.Parse(line).RootElement;
            if (o.TryGetProperty("X", out var x))
            {
                double h = o.GetProperty("h").GetDouble() * Math.Tau;
                poses.Add((new(x.GetSingle(), -o.GetProperty("Z").GetSingle()), new(-(float)Math.Sin(h), (float)Math.Cos(h))));
            }
            else
            {
                double qz = o.GetProperty("pose.orientation.z").GetDouble(), qw = o.GetProperty("pose.orientation.w").GetDouble();
                double yaw = 2 * Math.Atan2(qz, qw);
                poses.Add((new(o.GetProperty("pose.position.x").GetSingle(), o.GetProperty("pose.position.y").GetSingle()), new((float)Math.Cos(yaw), (float)Math.Sin(yaw))));
            }
        }
        // A drive may start or end off the mapped roads (yard, parking): from the first pose beside a
        // road along its heading to the last pose near a road.
        int skipStart = 0, skipEnd = 0;
        var sources = new List<EdgeSpot>();var targets = new List<EdgeSpot>();
        while (skipStart < poses.Count && (sources = planner.Near(poses[skipStart].p, 15, poses[skipStart].heading, 45)).Count == 0) ++skipStart;
        while (skipEnd < poses.Count && (targets = planner.Near(poses[^(skipEnd + 1)].p, 15).OrderBy(t => t.Distance).Take(8).ToList()).Count == 0) ++skipEnd;
        var first = poses[Math.Min(skipStart, poses.Count - 1)];var last = poses[^Math.Min(skipEnd + 1, poses.Count)];
        var watch = Stopwatch.StartNew();
        var route = sources.Count == 0 || targets.Count == 0 ? null : planner.Plan(sources, targets, [last.p]);
        double ms = watch.Elapsed.TotalMilliseconds;
        if (route is null) {Console.WriteLine(JsonSerializer.Serialize(new {poses = posesFile, error = "no route", sources = sources.Count, targets = targets.Count}));return 1;}
        var json = planner.ToJson(route, first.p, last.p);
        if (output is not null) RollingLane.Replace(output, json.ToJsonString());
        var line2 = json["polyline"]!.AsArray().Select(q => new Vector2((float)(double)q![0]!, (float)(double)q[1]!)).ToList();
        double Distance(Vector2 p)
        {
            double best = double.PositiveInfinity;
            for (int i = 1; i < line2.Count; ++i)
            {
                var a = line2[i - 1];var ab = line2[i] - a;float l2 = ab.LengthSquared();
                float t = l2 > 0 ? Math.Clamp(Vector2.Dot(p - a, ab) / l2, 0, 1) : 0;
                best = Math.Min(best, Vector2.Distance(a + ab * t, p));
            }
            return best;
        }
        var used = poses.Skip(skipStart).Take(Math.Max(1, poses.Count - skipStart - skipEnd)).ToList();
        var distances = used.Select(p => Distance(p.p)).ToList();
        double driven = 0;for (int i = 1; i < used.Count; ++i) driven += Vector2.Distance(used[i - 1].p, used[i].p);
        // Off-route stretches (> 10 m) with where they start and end, to explain mismatches.
        var off = new List<object>();
        for (int i = 0; i < distances.Count; )
        {
            if (distances[i] <= 10) {++i;continue;}
            int j = i;while (j + 1 < distances.Count && distances[j + 1] > 10) ++j;
            off.Add(new {from = i, to = j, max_m = Math.Round(distances.Skip(i).Take(j - i + 1).Max(), 1), at = new[] {Math.Round(used[i].p.X), Math.Round(used[i].p.Y)}});
            i = j + 1;
        }
        Console.WriteLine(JsonSerializer.Serialize(new {poses = posesFile, count = poses.Count, skipped_start = skipStart, skipped_end = skipEnd, driven_m = Math.Round(driven), route_m = (double)json["length_m"]!, eta_s = (double)json["eta_s"]!,
            items = route.Edges.Count, query_ms = Math.Round(ms, 2), within_10m = Math.Round(distances.Count(d => d <= 10) / (double)distances.Count, 4),
            median_m = Math.Round(distances.Order().ElementAt(distances.Count / 2), 2), off_route = off}));
        return 0;
    }

    // Per edge: 0 in the main network (largest strongly connected component), 1 reachable from it
    // only (a dead-end stub), 2 leading into it only, 3 neither (isolated).
    public static int[] Reach(SdMap map)
    {
        var component = Components(map);
        var size = new Dictionary<int, double>();
        for (int e = 0; e < map.Edges.Length; ++e) size[component[e]] = size.GetValueOrDefault(component[e]) + map.Edges[e].Length;
        int main = size.MaxBy(x => x.Value).Key;
        var preds = new List<int>?[map.Edges.Length];
        for (int e = 0; e < map.Edges.Length; ++e) foreach (var s in map.Successors(e)) (preds[s] ??= new()).Add(e);
        bool[] Flood(bool forward)
        {
            var seen = new bool[map.Edges.Length];var queue = new Queue<int>();
            for (int e = 0; e < map.Edges.Length; ++e) if (component[e] == main) {seen[e] = true;queue.Enqueue(e);}
            while (queue.Count > 0)
            {
                int e = queue.Dequeue();
                foreach (var s in forward ? map.Successors(e) : preds[e] ?? []) if (!seen[s]) {seen[s] = true;queue.Enqueue(s);}
            }
            return seen;
        }
        var from = Flood(true);var to = Flood(false);
        return Enumerable.Range(0, map.Edges.Length).Select(e => component[e] == main ? 0 : from[e] && !to[e] ? 1 : to[e] && !from[e] ? 2 : 3).ToArray();
    }
    // Strongly connected components of the edge graph (iterative Tarjan): component id per edge.
    public static int[] Components(SdMap map)
    {
        int n = map.Edges.Length, counter = 0, components = 0;
        var index = new int[n];Array.Fill(index, -1);
        var low = new int[n];var component = new int[n];Array.Fill(component, -1);
        var onStack = new bool[n];var stack = new Stack<int>();
        var work = new Stack<(int e, IEnumerator<int> next)>();
        for (int root = 0; root < n; ++root)
        {
            if (index[root] >= 0) continue;
            index[root] = low[root] = counter++;stack.Push(root);onStack[root] = true;
            work.Push((root, map.Successors(root).GetEnumerator()));
            while (work.Count > 0)
            {
                var (e, next) = work.Peek();
                if (next.MoveNext())
                {
                    int s = next.Current;
                    if (index[s] < 0) {index[s] = low[s] = counter++;stack.Push(s);onStack[s] = true;work.Push((s, map.Successors(s).GetEnumerator()));}
                    else if (onStack[s]) low[e] = Math.Min(low[e], index[s]);
                    continue;
                }
                work.Pop();
                if (work.Count > 0) low[work.Peek().e] = Math.Min(low[work.Peek().e], low[e]);
                if (low[e] == index[e])
                {
                    int x;
                    do {x = stack.Pop();onStack[x] = false;component[x] = components;} while (x != e);
                    ++components;
                }
            }
        }
        return component;
    }

    // Offline checks of the cache and the planner (no game needed beyond the cache key).
    static int SelfTest(SdMap map, RoutePlanner planner, string cache)
    {
        int failures = 0;
        void Check(bool ok, string what, object? detail = null)
        {
            Console.WriteLine(JsonSerializer.Serialize(new {check = what, ok, detail}));
            if (!ok) ++failures;
        }
        // 1. Cache round trip.
        string copy = Path.Combine(Path.GetTempPath(), $"sdmap-selftest-{Environment.ProcessId}.bin");
        try
        {
            map.Write(copy);
            var again = SdMap.Read(copy);
            Check(again.Key == map.Key && again.Edges.Length == map.Edges.Length && again.Nodes.Length == map.Nodes.Length && again.Points.SequenceEqual(map.Points)
                && again.Edges.Select(e => (e.From, e.To, e.Cost)).SequenceEqual(map.Edges.Select(e => (e.From, e.To, e.Cost))) && again.Companies.SequenceEqual(map.Companies), "cache round trip", new {bytes = new FileInfo(copy).Length});
        }
        finally {File.Delete(copy);}
        // 2. Graph consistency: edge endpoints, polylines starting/ending at their nodes' side, costs.
        int bad = 0, deadEnds = 0;double worstEnd = 0;
        var deadKinds = new Dictionary<string, int>();
        for (int e = 0; e < map.Edges.Length; ++e)
        {
            var edge = map.Edges[e];
            if (edge.From < 0 || edge.To < 0 || edge.From >= map.Nodes.Length || edge.To >= map.Nodes.Length || !(edge.Cost > 0) || edge.Cost < edge.Length / SdMap.MaxSpeed - 1e-3 || edge.GeomCount < 2) ++bad;
            // Polyline ends lie at most a carriageway (or a prefab's span) from their nodes.
            if (map.Items[edge.Item].Kind == SdMap.KindRoad)
                worstEnd = Math.Max(worstEnd, Math.Max(Vector2.Distance(map.Point(edge.Geom), map.NodePos(edge.From)), Vector2.Distance(map.Point(edge.Geom + edge.GeomCount - 1), map.NodePos(edge.To))));
            if (!map.Successors(e).Any())
            {
                ++deadEnds;
                int next = map.Next(e);
                string why = next < 0 ? (map.Nodes[edge.To].Forward < 0 && map.Nodes[edge.To].Backward < 0 ? "node joins nothing routable" : "nothing beyond the node")
                    : map.Items[next].Kind == SdMap.KindRoad ? "road beyond has no lane this way" : "prefab beyond has no traversal from this node";
                deadKinds[why] = deadKinds.GetValueOrDefault(why) + 1;
            }
        }
        Console.WriteLine(JsonSerializer.Serialize(new {dead_ends = deadKinds}));
        Check(bad == 0, "edges valid (nodes, cost >= length / max speed, polyline)", new {bad, edges = map.Edges.Length});
        Check(worstEnd < 40, "road polylines end beside their nodes", new {max_m = Math.Round(worstEnd, 2)});
        Check(deadEnds < map.Edges.Length / 5, "dead ends are a minority", new {dead_ends = deadEnds});
        // Connectivity: the main network is the largest strongly connected piece; the rest are dead-end
        // stubs (reachable one way), yards, scenery roads joined to nothing, and islands (ferries are
        // not routed). Shares of directed road length, and of hidden (not on the UI map) roads.
        var reach = Reach(map);
        double roadLength = 0, hidden = 0, hiddenIsolated = 0;
        var share = new double[4];
        for (int e = 0; e < map.Edges.Length; ++e)
        {
            if (map.Items[map.Edges[e].Item].Kind != SdMap.KindRoad) continue;
            double length = map.Edges[e].Length;
            roadLength += length;share[reach[e]] += length;
            if ((map.Edges[e].Flags & SdMap.Hidden) != 0) {hidden += length;if (reach[e] == 3) hiddenIsolated += length;}
        }
        Console.WriteLine(JsonSerializer.Serialize(new {main_network = Math.Round(share[0] / roadLength, 4), only_from_main = Math.Round(share[1] / roadLength, 4), only_to_main = Math.Round(share[2] / roadLength, 4),
            isolated = Math.Round(share[3] / roadLength, 4), hidden = Math.Round(hidden / roadLength, 4), isolated_hidden = Math.Round(hiddenIsolated / Math.Max(1, share[3]), 4), road_km = Math.Round(roadLength / 1000)}));
        var component = Components(map);
        var size = new Dictionary<int, double>();
        for (int e = 0; e < map.Edges.Length; ++e) if (map.Items[map.Edges[e].Item].Kind == SdMap.KindRoad) size[component[e]] = size.GetValueOrDefault(component[e]) + map.Edges[e].Length;
        var largest = size.Values.OrderDescending().Take(3).Select(x => Math.Round(x / roadLength, 4)).ToArray();
        Check(largest[0] > 5 * largest[1], "one main network (strongly connected, share of directed road length)", new {largest, components = size.Count});
        // 3. A* equals Dijkstra (the heuristic is admissible), and routes chain item to item.
        var random = new Random(3);
        var (roads, cumulative) = Roads(map);
        int compared = 0, mismatched = 0, broken = 0;double maxGap = 0;
        for (int tries = 0; compared < 40 && tries < 4000; ++tries)
        {
            var a = RandomSpot(map, random, roads, cumulative);var b = RandomSpot(map, random, roads, cumulative);
            if (Vector2.Distance(a.p, b.p) > 60e3) continue;
            var sources = planner.Near(a.p, 15, a.heading, 45);var targets = planner.Near(b.p, 50).OrderBy(t => t.Distance).Take(8).ToList();
            var fast = planner.Plan(sources, targets, [b.p]);
            if (fast is null) continue;
            var slow = planner.Plan(sources, targets, [b.p], astar: false)!;
            ++compared;
            if (Math.Abs(fast.Cost - slow.Cost) > 1e-6 * Math.Max(1, slow.Cost)) ++mismatched;
            for (int i = 1; i < fast.Edges.Count; ++i)
            {
                int p = fast.Edges[i - 1], q = fast.Edges[i];
                if (map.Edges[p].To != map.Edges[q].From || map.Next(p) != map.Edges[q].Item) ++broken;
            }
            var json = planner.ToJson(fast, a.p, b.p);
            var line = json["polyline"]!.AsArray();
            for (int i = 1; i < line.Count; ++i) maxGap = Math.Max(maxGap, Math.Sqrt(Math.Pow((double)line[i]![0]! - (double)line[i - 1]![0]!, 2) + Math.Pow((double)line[i]![1]! - (double)line[i - 1]![1]!, 2)));
        }
        Check(compared >= 20 && mismatched == 0, "A* cost equals Dijkstra", new {compared, mismatched});
        Check(broken == 0, "route items chain through shared nodes", new {broken});
        Check(maxGap < 3 * map.Step + 15, "route polyline has no jumps", new {max_gap_m = Math.Round(maxGap, 2), step_m = map.Step});
        // 4. lanes_ok: every road right before a prefab with curves has a lane reaching the route's exit.
        int roadsBefore = 0, empty = 0;
        for (int tries = 0; roadsBefore < 300 && tries < 3000; ++tries)
        {
            var a = RandomSpot(map, random, roads, cumulative);var b = RandomSpot(map, random, roads, cumulative);
            if (Vector2.Distance(a.p, b.p) > 20e3) continue;
            var route = planner.Plan(planner.Near(a.p, 15, a.heading, 45), planner.Near(b.p, 50).OrderBy(t => t.Distance).Take(8).ToList(), [b.p]);
            if (route is null) continue;
            var json = planner.ToJson(route, a.p, b.p);
            var items = json["items"]!.AsArray();
            for (int i = 0; i + 1 < items.Count; ++i)
            {
                if ((string)items[i]!["kind"]! != "road" || (string)items[i + 1]!["kind"]! != "prefab" || items[i + 1]!["curveless"] is not null) continue;
                ++roadsBefore;
                if (items[i]!["lanes_ok"]!.AsArray().Count == 0) ++empty;
            }
        }
        Check(roadsBefore > 0 && empty <= roadsBefore / 50, "roads before route prefabs have lanes_ok", new {roads = roadsBefore, empty});
        // 5. Job destinations: a company depot (company and city tokens of the SDK job) resolves to
        // targets reachable from the main network: the edges entering its prefab, or nearby roads.
        var depotWhy = new Dictionary<string, int>();
        int depots = 0;
        var offCities = new SortedSet<string>();
        foreach (var c in map.Companies)
        {
            var targetsOf = planner.DepotTargets(c);
            foreach (var radius in RoutePlanner.DepotRadii) if (!targetsOf.Any(t => reach[t.Edge] <= 1)) targetsOf = planner.DepotTargets(c, radius);
            string why = targetsOf.Any(t => reach[t.Edge] <= 1) ? (targetsOf.All(t => t.Distance == 0) ? "prefab entered" : "road within 300 m")
                : targetsOf.Count == 0 ? "no road within 300 m" : "off the main network (island, ferry)";
            depotWhy[why] = depotWhy.GetValueOrDefault(why) + 1;
            if (why is "prefab entered" or "road within 300 m") ++depots;
            else offCities.Add(c.City);
        }
        Console.WriteLine(JsonSerializer.Serialize(new {depots_off_main_network = offCities.Order()}));
        // Off the main network: the UK and islands behind ferries, depots of map DLCs that are not
        // installed, and a few yards joined only through dead ends.
        Check(depots >= map.Companies.Count * 2 / 3, "company depots resolve to reachable targets", new {companies = map.Companies.Count, reachable = depots, by = depotWhy});
        // 6. The walker's reading of a route file.
        string file = Path.Combine(Path.GetTempPath(), $"route-selftest-{Environment.ProcessId}.route.json");
        try
        {
            PlannedRoute? r = null;
            for (int tries = 0; r is null || r.Edges.Count(e => map.Items[map.Edges[e].Item].Kind == SdMap.KindPrefab) == 0; ++tries)
            {
                var a = RandomSpot(map, random, roads, cumulative);var b = RandomSpot(map, random, roads, cumulative);
                if (Vector2.Distance(a.p, b.p) < 20e3) r = planner.Plan(planner.Near(a.p, 15, a.heading, 45), planner.Near(b.p, 50).OrderBy(t => t.Distance).Take(8).ToList(), [b.p]);
            }
            File.WriteAllText(file, planner.ToJson(r, Vector2.Zero, Vector2.Zero).ToJsonString());
            var guide = RouteGuide.Load(file);
            int ok = 0, total = 0;
            foreach (var e in r.Edges.Where(e => map.Items[map.Edges[e].Item].Kind == SdMap.KindPrefab))
            {
                ++total;
                if (guide.Exit(map.Items[map.Edges[e].Item].Uid, map.Nodes[map.Edges[e].From].Uid) == map.Nodes[map.Edges[e].To].Uid) ++ok;
            }
            Check(ok == total, "route file gives the walker each prefab's exit", new {prefabs = total, ok});
        }
        finally {File.Delete(file);}
        Console.WriteLine(JsonSerializer.Serialize(new {cache, failures}));
        return failures == 0 ? 0 : 1;
    }
}
