// Decrypt an ScsC save (game.sii) to text: savedec <in> <out>
var bytes = File.ReadAllBytes(args[0]);
var text = TruckLib.Sii.EncryptedSii.Decrypt(bytes);
File.WriteAllBytes(args[1], text);
