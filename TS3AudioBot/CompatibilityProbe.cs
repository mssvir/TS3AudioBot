using LiteDB;
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace TS3AudioBot
{
	internal static class CompatibilityProbe
	{
		// This path exits before log, configuration, web, TeamSpeak, or stats startup.
		// Operators must point it at a consistent offline database copy.
		internal static int Run(string databasePath, TextWriter output)
		{
			output.WriteLine("runtime=" + RuntimeInformation.FrameworkDescription);
			output.WriteLine("litedb=" + typeof(LiteDatabase).Assembly.GetName().Version);
			try
			{
				if (!File.Exists(databasePath))
				{
					output.WriteLine("database_probe=missing");
					return 3;
				}

				using var database = new LiteDatabase(new ConnectionString
				{
					Filename = Path.GetFullPath(databasePath),
					ReadOnly = true,
				});
				_ = database.GetCollection<BsonDocument>("dbmeta").Count();
				output.WriteLine("database_probe=compatible");
				return 0;
			}
			catch (Exception)
			{
				output.WriteLine("database_probe=incompatible");
				return 3;
			}
		}
	}
}
