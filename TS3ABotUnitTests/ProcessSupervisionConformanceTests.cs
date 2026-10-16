using NUnit.Framework;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using TS3AudioBot.Audio;

namespace TS3ABotUnitTests
{
	[TestFixture]
	public class ProcessSupervisionConformanceTests
	{
		[Test]
		public void ManagedProcessSupervisionMatchesSharedFixture()
		{
			var fixture = LoadFixture();

			foreach (var item in fixture.Classifications)
			{
				if (item.Platform == "linux" && !RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
					continue;

				Assert.AreEqual(item.Expected, ClassifyManagedStartError(item.NativeError), item.Name);
			}

			AssertGatePolicy(fixture);
		}

		private static string ClassifyManagedStartError(int nativeError)
		{
			var method = typeof(FfmpegProducer).GetMethod(
				"DescribeFfmpegStartError",
				BindingFlags.NonPublic | BindingFlags.Static);

			Assert.NotNull(method, "Managed FFmpeg error classifier must remain available to conformance tests");
			var message = (string)method.Invoke(null, new object[] { new Win32Exception(nativeError) });
			if (message.Contains("Ffmpeg could not be found", StringComparison.Ordinal))
				return "missing_executable";
			if (message.Contains("EAGAIN/errno 11", StringComparison.Ordinal))
				return "resource_exhaustion";
			return "other_native_error";
		}

		private static void AssertGatePolicy(Fixture fixture)
		{
			var freshStarts = GetIntPolicy(fixture, "fresh_concurrent_starts");
			var gate = CreateGate(fixture, out _);
			for (int i = 0; i < freshStarts; i++)
			{
				Assert.True(gate.TryEnter(out var retryAfter));
				Assert.AreEqual(TimeSpan.Zero, retryAfter);
			}

			gate = CreateGate(fixture, out var cooldownClock);
			foreach (var expectedMilliseconds in GetIntListPolicy(fixture, "cooldown_sequence_ms"))
			{
				var cooldown = gate.RecordResourceFailure();
				Assert.AreEqual(TimeSpan.FromMilliseconds(expectedMilliseconds), cooldown);
				cooldownClock.Advance(cooldown);
				Assert.True(gate.TryEnter(out _));
			}

			if (GetBoolPolicy(fixture, "single_half_open_probe"))
			{
				gate = CreateGate(fixture, out var probeClock);
				var cooldown = gate.RecordResourceFailure();
				probeClock.Advance(cooldown);
				Assert.True(gate.TryEnter(out var firstRetry));
				Assert.AreEqual(TimeSpan.Zero, firstRetry);
				Assert.False(gate.TryEnter(out var competingRetry));
				Assert.AreEqual(TimeSpan.FromMilliseconds(GetIntPolicy(fixture, "probe_retry_ms")), competingRetry);
			}

			if (GetBoolPolicy(fixture, "success_resets"))
			{
				gate = CreateGate(fixture, out var successClock);
				var cooldown = gate.RecordResourceFailure();
				successClock.Advance(cooldown);
				Assert.True(gate.TryEnter(out _));
				gate.RecordSuccess();
				Assert.True(gate.TryEnter(out _));
				Assert.True(gate.TryEnter(out _));
			}

			if (GetBoolPolicy(fixture, "non_resource_failure_preserves_streak"))
			{
				gate = CreateGate(fixture, out var nonResourceClock);
				var cooldown = gate.RecordResourceFailure();
				nonResourceClock.Advance(cooldown);
				Assert.True(gate.TryEnter(out _));
				gate.RecordNonResourceFailure();
				Assert.True(gate.TryEnter(out _));
				Assert.False(gate.TryEnter(out _));
			}
		}

		private static FfmpegStartAdmissionGate CreateGate(Fixture fixture, out FakeClock clock)
		{
			clock = new FakeClock(new DateTimeOffset(2026, 10, 16, 0, 0, 0, TimeSpan.Zero));
			return new FfmpegStartAdmissionGate(
				clock.UtcNow,
				TimeSpan.FromMilliseconds(GetIntPolicy(fixture, "base_cooldown_ms")),
				TimeSpan.FromMilliseconds(GetIntPolicy(fixture, "max_cooldown_ms")),
				TimeSpan.FromMilliseconds(GetIntPolicy(fixture, "probe_retry_ms")));
		}

		private static Fixture LoadFixture()
		{
			var path = Path.Combine(TestContext.CurrentContext.TestDirectory, "conformance", "process_supervision_v1.tsv");
			Assert.True(File.Exists(path), "Shared process supervision fixture must be copied to test output");

			var fixture = new Fixture();
			foreach (var rawLine in File.ReadLines(path))
			{
				var line = rawLine.Trim();
				if (line.Length == 0 || line.StartsWith("#", StringComparison.Ordinal))
					continue;

				var fields = line.Split('\t');
				if (fields[0] == "classification")
				{
					Assert.AreEqual(5, fields.Length, "Classification fixture row must have five fields");
					fixture.Classifications.Add(new ClassificationCase
					{
						Name = fields[1],
						NativeError = int.Parse(fields[2]),
						Platform = fields[3],
						Expected = fields[4],
					});
				}
				else if (fields[0] == "policy")
				{
					Assert.AreEqual(3, fields.Length, "Policy fixture row must have three fields");
					fixture.Policies.Add(fields[1], fields[2]);
				}
				else
				{
					Assert.Fail("Unknown fixture row kind: " + fields[0]);
				}
			}

			Assert.IsNotEmpty(fixture.Classifications);
			return fixture;
		}

		private static int GetIntPolicy(Fixture fixture, string name) => int.Parse(GetPolicy(fixture, name));

		private static bool GetBoolPolicy(Fixture fixture, string name) => bool.Parse(GetPolicy(fixture, name));

		private static IEnumerable<int> GetIntListPolicy(Fixture fixture, string name)
		{
			foreach (var value in GetPolicy(fixture, name).Split(','))
				yield return int.Parse(value);
		}

		private static string GetPolicy(Fixture fixture, string name)
		{
			Assert.True(fixture.Policies.TryGetValue(name, out var value), "Missing fixture policy: " + name);
			return value;
		}

		private sealed class Fixture
		{
			public List<ClassificationCase> Classifications { get; } = new List<ClassificationCase>();
			public Dictionary<string, string> Policies { get; } = new Dictionary<string, string>(StringComparer.Ordinal);
		}

		private sealed class ClassificationCase
		{
			public string Name { get; set; }
			public int NativeError { get; set; }
			public string Platform { get; set; }
			public string Expected { get; set; }
		}

		private sealed class FakeClock
		{
			private DateTimeOffset now;

			public FakeClock(DateTimeOffset now)
			{
				this.now = now;
			}

			public DateTimeOffset UtcNow() => now;

			public void Advance(TimeSpan by)
			{
				now += by;
			}
		}
	}
}
