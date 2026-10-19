using System.IO.Compression;
using System.Numerics;
using System.Reflection;
using System.Text.Json;
using TruckLib.HashFs;
using TruckLib.ScsMap;

// Builds one mod with a flat asphalt test pad in two forms:
// an external map attached to europe (reach it with goto + Ctrl+F9 from any profile)
// and a standalone map started with the console command `preview ot_testground`.
try
{
    var options = new Dictionary<string, string>();
    for (int i = 0; i < args.Length; i += 2)
    {
        if (i + 1 == args.Length || !args[i].StartsWith("--")) throw new ArgumentException("Options require --name value pairs");
        options.Add(args[i], args[i + 1]);
    }
    string output = options.GetValueOrDefault("--out") ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Euro Truck Simulator 2", "mod", "ot_testground.scs");
    string game = options.GetValueOrDefault("--game") ?? Environment.GetEnvironmentVariable("ETS2_DIR")
        ?? throw new ArgumentException("Pass --game <ETS2 install directory> or set ETS2_DIR");
    float width = float.Parse(options.GetValueOrDefault("--width-m", "5100"));
    float length = float.Parse(options.GetValueOrDefault("--length-m", "8000"));
    // Sector (-7,-12) in the North Sea, with a 5x5 block of empty sectors around it.
    // 20 m above sea level keeps it clear of any water surface.
    var europeCenter = new Vector3(float.Parse(options.GetValueOrDefault("--europe-x", "-26000")),
        float.Parse(options.GetValueOrDefault("--europe-y", "20")), float.Parse(options.GetValueOrDefault("--europe-z", "-46000")));
    // Every base map terrain side has at most 63 quad rows (198,836 sides, game 1.61); a
    // 6500 m side (77 rows) was rejected as "Terrain item quad info mismatch". With the
    // plain profile's row steps (1,1,1,2,6,6,10,10,10,20,40,50,50,50,100,100 m, then 100 m)
    // 63 rows reach 5156 m.
    if (width < 100 || width > 5156) throw new ArgumentException("--width-m must be 100..5156 (63 terrain rows)");
    if (length < 100 || length > 50000) throw new ArgumentException("--length-m must be 100..50000");

    string staging = Path.Combine(Path.GetTempPath(), "ot_testground_" + Guid.NewGuid().ToString("N"));
    try
    {
        var europe = Pad(europeCenter, width, length);
        europe.Save(Path.Combine(staging, "europe"), "ot_testground_ext");
        WriteDescs(Path.Combine(staging, "europe", "ot_testground_ext"), europeCenter, width, length);
        var standalone = Pad(Vector3.Zero, width, length);
        standalone.StartPosition = new Vector3(0, 0.5f, 0);
        standalone.Save(Path.Combine(staging, "standalone"), "ot_testground");
        WriteDescs(Path.Combine(staging, "standalone", "ot_testground"), Vector3.Zero, width, length);

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
        string temporary = output + ".tmp";
        using (var zip = ZipFile.Open(temporary, ZipArchiveMode.Create))
        {
            void Add(string source, string entry) => zip.CreateEntryFromFile(source, entry, CompressionLevel.NoCompression);
            // Sector files added to map/europe/ were not loaded. The game attaches the
            // seasonal event map through map/europe.external.sii, which accepts one
            // external_map_data unit. This replaces that file with one root holding the
            // base game's halloween sectors and the pad, and keeps the event's UI camera.
            foreach (var file in Directory.GetFiles(Path.Combine(staging, "europe", "ot_testground_ext")))
                Add(file, "map/ot_testground_ext/" + Path.GetFileName(file));
            using (var baseMap = HashFsReader.Open(Path.Combine(game, "base_map.scs")))
                foreach (var file in baseMap.GetDirectoryListing("/map/halloween_ets2", true).Files)
                {
                    var entry = zip.CreateEntry("map/ot_testground_ext/" + Path.GetFileName(file), CompressionLevel.NoCompression);
                    using var stream = entry.Open();
                    stream.Write(baseMap.Extract(file)[0]);
                }
            var external = zip.CreateEntry("map/europe.external.sii", CompressionLevel.NoCompression);
            using (var w = new StreamWriter(external.Open()))
                w.Write("""
                    SiiNunit
                    {
                    external_map_data : .ext_map_dta
                    {
                    	map_root_path: "/map/ot_testground_ext"
                    	ui_map_camera_min: (-139000.0, 97000)
                    	ui_map_camera_max: (-135000.0, 99000)
                    }
                    }

                    """);
            Add(Path.Combine(staging, "standalone", "ot_testground.mbd"), "map/ot_testground.mbd");
            foreach (var file in Directory.GetFiles(Path.Combine(staging, "standalone", "ot_testground")))
                Add(file, "map/ot_testground/" + Path.GetFileName(file));
            var manifest = zip.CreateEntry("manifest.sii", CompressionLevel.NoCompression);
            using var writer = new StreamWriter(manifest.Open());
            writer.Write("SiiNunit\n{\nmod_package : .package_name\n{\n\tpackage_version: \"1.0\"\n" +
                "\tdisplay_name: \"OT Testground\"\n\tauthor: \"ETS2-Autonomy-Lab\"\n\tcategory[]: \"map\"\n" +
                "\tcompatible_versions[]: \"1.61.*\"\n}\n}\n");
        }
        File.Move(temporary, output, true);
    }
    finally
    {
        if (Directory.Exists(staging)) Directory.Delete(staging, true);
    }
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        output,
        width_m = width,
        length_m = length,
        surface = "terrain_mat.share_28 (asphalt 01), profile0 (plain), coefficient 0, no noise, y=pad center",
        europe_center_scs = new[] {europeCenter.X, europeCenter.Y, europeCenter.Z},
        europe_goto = $"goto {europeCenter.X:0};{europeCenter.Y + 5:0};{europeCenter.Z:0}",
        europe_bounds_scs = new[] {europeCenter.X - width / 2, europeCenter.Z - length / 2, europeCenter.X + width / 2, europeCenter.Z + length / 2},
        standalone = "preview ot_testground",
    }));
    return 0;
}
catch (Exception e)
{
    Console.Error.WriteLine(e.Message);
    return 1;
}

// A chain of 1 km terrain items along +Z on the pad's east edge, with terrain
// only on the right (-X when heading +Z, as base map terrain bounds show).
// A truck placed on the shared node line of two sides, or where separately
// added items met, fell through; the chain keeps every seam on the pad edge
// and the items share nodes along it.
static Map Pad(Vector3 center, float width, float length)
{
    var map = new Map();
    const float strip = 1000;
    var kdop = typeof(MapItem).GetProperty("Kdop", BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new MissingMemberException("TruckLib MapItem.Kdop");
    float x1 = center.X + width / 2, x0 = x1 - width, z = center.Z - length / 2, end = center.Z + length / 2;
    Terrain? previous = null;
    while (z < end)
    {
        float next = Math.Min(z + strip, end);
        var terrain = previous is null
            ? Terrain.Add(map, new Vector3(x1, center.Y, z), new Vector3(x1, center.Y, next), "share_28", 0, width)
            : previous.Append(new Vector3(x1, center.Y, next), "share_28", 0, width);
        foreach (var side in new[] {terrain.Left, terrain.Right})
        {
            side.Terrain.Profile = "profile0";
            side.Terrain.Coefficient = 0;
            side.Terrain.Noise = TerrainNoise.Percent0;
            side.DetailVegetation = false;
        }
        terrain.ViewDistance = 1500;
        // 16 m columns as on most base map terrain (63 per 1 km item; the base map's widest has 157).
        terrain.StepSize = StepSize.Meters16;
        terrain.Recalculate();
        // TruckLib leaves bounds unset; these are the k-DOP axes (x, y, z,
        // (x+z)/2, (x-z)/2) observed on base map terrain items.
        var bounds = (KdopBounds)kdop.GetValue(terrain)!;
        bounds.Minimums = [x0, center.Y - 1, z, (x0 + z) / 2, (x0 - next) / 2];
        bounds.Maximums = [x1, center.Y + 1, next, (x1 + next) / 2, (x1 - z) / 2];
        previous = terrain;
        z = next;
    }
    return map;
}

// TruckLib writes .desc version 2 with bounds fixed at 0..4000. Game 1.61 sectors
// carry version 3: world-space content bounds in 1/256 m (min x, min z, max x,
// max z), flags, then two tokens (base map value 0x15_73C3E700 for both). With
// the old bounds the pad sectors loaded only where they happened to overlap the
// origin. Every pad sector gets the whole pad's bounds.
static void WriteDescs(string sectorDirectory, Vector3 center, float width, float length)
{
    int Fixed(float meters) => checked((int)MathF.Round(meters * 256));
    float x1 = center.X + width / 2;
    foreach (var file in Directory.GetFiles(sectorDirectory, "*.desc"))
    {
        using var w = new BinaryWriter(File.Create(file));
        w.Write(3u);
        w.Write(Fixed(x1 - width)); w.Write(Fixed(center.Z - length / 2));
        w.Write(Fixed(x1)); w.Write(Fixed(center.Z + length / 2));
        w.Write(0u);
        w.Write(0x0000_0015_73C3_E700ul); w.Write(0x0000_0015_73C3_E700ul);
    }
}
