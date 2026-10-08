using LiteDB;
using NUnit.Framework;
using System;
using System.IO;
using TS3AudioBot;
using TS3AudioBot.Config;

namespace TS3ABotUnitTests
{
	[TestFixture]
	public class ProductionCompatibilityTests
	{
		[Test]
		public void ExistingLiteDb5FileOpensWithoutRewritingIt()
		{
			var directory = Path.Combine(Path.GetTempPath(), "audiobot-db-contract-" + Guid.NewGuid().ToString("N"));
			Directory.CreateDirectory(directory);
			var path = Path.Combine(directory, "fixture.db");
			try
			{
				using (var existing = new LiteDatabase(path))
				{
					existing.GetCollection<DbMetaData>("dbmeta").Insert(new DbMetaData
					{
						Id = "history",
						Version = 7,
						CustomData = "synthetic compatibility fixture",
					});
				}

				var before = File.ReadAllBytes(path);
				using var output = new StringWriter();
				Assert.AreEqual(0, CompatibilityProbe.Run(path, output));
				StringAssert.Contains("database_probe=compatible", output.ToString());
				StringAssert.DoesNotContain(path, output.ToString());
				StringAssert.DoesNotContain("synthetic compatibility fixture", output.ToString());
				CollectionAssert.AreEqual(before, File.ReadAllBytes(path), "Probe must not change database bytes");
			}
			finally
			{
				Directory.Delete(directory, true);
			}
		}

		[Test]
		public void MissingDatabaseIsNotCreated()
		{
			var path = Path.Combine(Path.GetTempPath(), "audiobot-missing-" + Guid.NewGuid().ToString("N") + ".db");
			using var output = new StringWriter();
			Assert.AreEqual(3, CompatibilityProbe.Run(path, output));
			Assert.False(File.Exists(path));
			StringAssert.Contains("database_probe=missing", output.ToString());
		}

		[Test]
		public void UnsupportedFileIsRejectedWithoutRepair()
		{
			var path = Path.Combine(Path.GetTempPath(), "audiobot-invalid-" + Guid.NewGuid().ToString("N") + ".db");
			var original = new byte[8192];
			try
			{
				File.WriteAllBytes(path, original);
				using var output = new StringWriter();
				Assert.AreEqual(3, CompatibilityProbe.Run(path, output));
				StringAssert.Contains("database_probe=incompatible", output.ToString());
				StringAssert.DoesNotContain(path, output.ToString());
				CollectionAssert.AreEqual(original, File.ReadAllBytes(path));
			}
			finally
			{
				File.Delete(path);
			}
		}
	}
}
