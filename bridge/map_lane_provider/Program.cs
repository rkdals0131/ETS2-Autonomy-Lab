using System.IO.Pipes;
using System.Numerics;
using System.Text;
using System.Text.Json;
using System.Globalization;
using TruckLib.HashFs;
using TruckLib.ScsMap;
using TruckLib.Sii;

try
{
    var options = new Dictionary<string, string>();
    for (int i = 0; i < args.Length; i += 2)
    {
        if (i + 1 == args.Length || !args[i].StartsWith("--")) throw new ArgumentException("Options require --name value pairs");
        options.Add(args[i], args[i + 1]);
    }
    string game = options.GetValueOrDefault("--game", @"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2");
    using var snapshot = options.TryGetValue("--snapshot", out var saved)
        ? JsonDocument.Parse(File.ReadAllText(saved)) : ReadSnapshot();
    var state = snapshot.RootElement;
    if (state.TryGetProperty("snapshot", out var nested)) state = nested;
    var placement = state.GetProperty("sdk").GetProperty("truck.world.placement").GetProperty("value");
    var xyz = placement.GetProperty("position_m");
    var position = new Vector3(xyz[0].GetSingle(), xyz[1].GetSingle(), xyz[2].GetSingle());
    double heading = placement.GetProperty("euler_rotations")[0].GetDouble() * Math.Tau;
    var forward = new Vector3(-(float)Math.Sin(heading), 0, -(float)Math.Cos(heading));
    using var archive = HashFsReader.Open(Path.Combine(game, "base_map.scs"));
    int sx = (int)Math.Floor(position.X / Map.SectorSize), sz = (int)Math.Floor(position.Z / Map.SectorSize);
    var sectors = new List<SectorCoordinate>();
    for (int x = sx - 1; x <= sx + 1; ++x) for (int z = sz - 1; z <= sz + 1; ++z) sectors.Add(new(x, z));
    var map = Map.Open("/map/europe.mbd", archive, sectors);
    var nearest = map.MapItems.Values.OfType<Road>().Select(road =>
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
    using var definitions = HashFsReader.Open(Path.Combine(game, "def.scs"));
    using var shared = HashFsReader.Open(Path.Combine(game, "base_share.scs"));
    using var baseAssets = HashFsReader.Open(Path.Combine(game, "base.scs"));
    var fs = new AssetLoader([definitions, shared, baseAssets, archive]);
    var looks = new Dictionary<string, Unit>();
    foreach (var file in fs.GetFiles("/def/world/").Where(f => Path.GetFileName(f).StartsWith("road_look") && f.EndsWith(".sii")))
    foreach (var unit in SiiFile.Open(file, fs).Units) looks[unit.Name] = unit;
    var geometries = new Dictionary<string, LaneGeometry>();
    LaneGeometry Geometry(Road road)
    {
        string key = "road." + road.RoadType;
        if (geometries.TryGetValue(key, out var cached)) return cached;
        var unit = looks[key];
        // The current divided highway uses a one-way template. Other road
        // layouts require their own geometry before a path can cross them.
        if (!unit.Attributes.ContainsKey("template_right") || unit.Attributes.ContainsKey("lanes_left"))
            throw new InvalidOperationException($"No lane geometry adapter for {key}");
        int lanes = ((System.Collections.ICollection)unit.Attributes["lanes_right"]).Count;
        string modelPath = unit.Attributes["template_right"];
        var model = TruckLib.Models.Model.Open(modelPath, fs);
        var surface = model.Parts.Single(p => p.Name.ToString() == "vis").Pieces.SelectMany(p => p.Vertices).ToArray();
        float min = surface.Min(v => v.Position.X), max = surface.Max(v => v.Position.X);
        float shoulderLeft = Convert.ToSingle(unit.Attributes["shoulder_space_left"], CultureInfo.InvariantCulture);
        float shoulderRight = Convert.ToSingle(unit.Attributes["shoulder_space_right"], CultureInfo.InvariantCulture);
        float width = (max - min - shoulderLeft - shoulderRight) / lanes;
        if (!(width > 0) || !float.IsFinite(width)) throw new InvalidDataException($"Invalid lane width in {key}");
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
        Console.Error.WriteLine($"{key}: template={modelPath}, road surface x=[{min},{max}], shoulders=[{shoulderLeft},{shoulderRight}], lanes={lanes}, width={width}, centers=[{string.Join(',',centers)}]");
        return geometries[key] = new(width, centers);
    }
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
            float low = Math.Max(0,t - 4 / item.road.Length), high = Math.Min(1,t + 4 / item.road.Length);
            for (int n = 0; n < 20; ++n)
            {
                float a = low+(high-low)/3, b = high-(high-low)/3;
                if (Vector3.DistanceSquared(Point(item.road,a,geometry.Centers[lane]),position) < Vector3.DistanceSquared(Point(item.road,b,geometry.Centers[lane]),position)) high=b; else low=a;
            }
            t=(low+high)/2;
            var tangent = Vector3.Transform(-Vector3.UnitZ,item.road.InterpolateCurve(t).Rotation);
            bool increasing = Vector3.Dot(tangent,forward)>0;
            double distance = Vector3.Distance(Point(item.road,t,geometry.Centers[lane]),position);
            if (selected is null || distance<selected.Value.distance) selected=(item.road,t,lane,increasing,distance);
        }
    }
    var choice = selected ?? throw new InvalidOperationException("Current position is not on a supported one-way road template");
    var initialGeometry=Geometry(choice.road);
    if(choice.distance>initialGeometry.Width/2) throw new InvalidOperationException($"Current vehicle is outside the nearest mapped lane (distance {choice.distance:F3}m)");
    var points = new List<double[]>();
    var visited = new HashSet<ulong>();
    var current=choice.road;float start=choice.t;bool increasingDirection=choice.increasing;int laneIndex=choice.lane;
    string endReason="road_end";
    double remaining=double.Parse(options.GetValueOrDefault("--distance-m","1000"),CultureInfo.InvariantCulture);
    if(!double.IsFinite(remaining)||remaining<=0) throw new ArgumentException("--distance-m must be positive");
    while(visited.Add(current.Uid))
    {
        var geometry=Geometry(current);
        float end=increasingDirection?1:0;
        int steps=Math.Max(1,(int)Math.Ceiling(Math.Abs(end-start)*current.Length/1.5));
        for(int i=0;i<=steps;++i)
        {
            var p=Point(current,start+(end-start)*i/steps,geometry.Centers[laneIndex]);
            double[] xy=[p.X,-p.Z];
            if(points.Count>0)
            {
                double distance=Math.Sqrt(Math.Pow(xy[0]-points[^1][0],2)+Math.Pow(xy[1]-points[^1][1],2));
                if(distance==0) continue;
                if(distance>remaining) {endReason="distance_limit";goto complete;}
                remaining-=distance;
            }
            points.Add(xy);
        }
        var exitNode=increasingDirection?current.ForwardNode:current.Node;
        var next=increasingDirection?current.ForwardItem:current.BackwardItem;
        if(next is Prefab) {endReason="prefab";break;}
        if(next is not Road nextRoad) break;
        LaneGeometry nextGeometry;
        try {nextGeometry=Geometry(nextRoad);} catch(InvalidOperationException) {endReason="ambiguous";break;}
        bool nextIncreasing=nextRoad.Node.Uid==exitNode.Uid;
        if(!nextIncreasing && nextRoad.ForwardNode.Uid!=exitNode.Uid) {endReason="ambiguous";break;}
        if(nextGeometry.Centers.Length!=geometry.Centers.Length) {endReason="ambiguous";break;}
        if(nextIncreasing!=increasingDirection) {endReason="ambiguous";break;}
        if(!nextGeometry.Centers.SequenceEqual(geometry.Centers)) {endReason="ambiguous";break;}
        if(visited.Contains(nextRoad.Uid)) {endReason="ambiguous";break;}
        current=nextRoad;start=nextIncreasing?0:1;increasingDirection=nextIncreasing;
    }
    complete:
    if(points.Count<2) throw new InvalidOperationException("No forward lane segment remains");
    string output=options.GetValueOrDefault("--out",Path.Combine(Environment.CurrentDirectory,"bridge","recordings","gt-path","current-lane.json"));
    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
    string staging=output+"."+Guid.NewGuid().ToString("N")+".tmp";
    try
    {
        File.WriteAllText(staging,JsonSerializer.Serialize(new {frame_id="world",lane_width_m=initialGeometry.Width,points,end_reason=endReason}));
        File.Move(staging,output,overwrite:true);
    }
    finally {if(File.Exists(staging)) File.Delete(staging);}
    Console.WriteLine(JsonSerializer.Serialize(new {output,road_uid=choice.road.Uid.ToString("x"),road_type=choice.road.RoadType.ToString(),lane_index=choice.lane,projection_distance_m=choice.distance,point_count=points.Count,end_reason=endReason}));
    return 0;
}
catch (Exception ex) {Console.Error.WriteLine(ex.Message);return 1;}
static Vector3 Point(Road road,float t,float x)
{
    var curve=road.InterpolateCurve(t);
    return curve.Position+Vector3.Transform(Vector3.UnitX,curve.Rotation)*x;
}

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
record LaneGeometry(float Width,float[] Centers);
