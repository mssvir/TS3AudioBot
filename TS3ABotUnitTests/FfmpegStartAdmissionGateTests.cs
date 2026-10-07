using NUnit.Framework;
using System;
using TS3AudioBot.Audio;

namespace TS3ABotUnitTests
{
	[TestFixture]
	public class FfmpegStartAdmissionGateTests
	{
		[Test]
		public void FreshGateAllowsConcurrentStarts()
		{
			var gate = CreateGate(out _);

			Assert.True(gate.TryEnter(out var firstRetry));
			Assert.AreEqual(TimeSpan.Zero, firstRetry);
			Assert.True(gate.TryEnter(out var secondRetry));
			Assert.AreEqual(TimeSpan.Zero, secondRetry);
		}

		[Test]
		public void ResourceFailureBlocksUntilCooldownExpiresAndAllowsSingleProbe()
		{
			var gate = CreateGate(out var clock);
			Assert.True(gate.TryEnter(out _));

			var cooldown = gate.RecordResourceFailure();
			Assert.AreEqual(TimeSpan.FromMilliseconds(250), cooldown);
			Assert.False(gate.TryEnter(out var retryAfter));
			Assert.AreEqual(TimeSpan.FromMilliseconds(250), retryAfter);

			clock.Advance(cooldown);
			Assert.True(gate.TryEnter(out var probeRetry));
			Assert.AreEqual(TimeSpan.Zero, probeRetry);
			Assert.False(gate.TryEnter(out var competingRetry));
			Assert.AreEqual(TimeSpan.FromMilliseconds(250), competingRetry);
		}

		[Test]
		public void ResourceFailuresUseBoundedExponentialCooldown()
		{
			var gate = CreateGate(out var clock);
			var expected = new[] { 250, 500, 1000, 2000, 4000, 5000, 5000 };

			foreach (var expectedMilliseconds in expected)
			{
				var cooldown = gate.RecordResourceFailure();
				Assert.AreEqual(TimeSpan.FromMilliseconds(expectedMilliseconds), cooldown);
				clock.Advance(cooldown);
				Assert.True(gate.TryEnter(out _));
			}
		}

		[Test]
		public void SuccessResetsResourceFailureCircuit()
		{
			var gate = CreateGate(out var clock);
			var cooldown = gate.RecordResourceFailure();
			clock.Advance(cooldown);
			Assert.True(gate.TryEnter(out _));

			gate.RecordSuccess();

			Assert.True(gate.TryEnter(out var firstRetry));
			Assert.AreEqual(TimeSpan.Zero, firstRetry);
			Assert.True(gate.TryEnter(out var secondRetry));
			Assert.AreEqual(TimeSpan.Zero, secondRetry);
		}

		[Test]
		public void NonResourceFailureReleasesProbeButKeepsResourcePressureState()
		{
			var gate = CreateGate(out var clock);
			var cooldown = gate.RecordResourceFailure();
			clock.Advance(cooldown);
			Assert.True(gate.TryEnter(out _));

			gate.RecordNonResourceFailure();

			Assert.True(gate.TryEnter(out var retryAfter));
			Assert.AreEqual(TimeSpan.Zero, retryAfter);
			Assert.False(gate.TryEnter(out var competingRetry));
			Assert.AreEqual(TimeSpan.FromMilliseconds(250), competingRetry);
		}

		private static FfmpegStartAdmissionGate CreateGate(out FakeClock clock)
		{
			clock = new FakeClock(new DateTimeOffset(2026, 10, 7, 20, 0, 0, TimeSpan.Zero));
			return new FfmpegStartAdmissionGate(
				clock.UtcNow,
				TimeSpan.FromMilliseconds(250),
				TimeSpan.FromSeconds(5),
				TimeSpan.FromMilliseconds(250));
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
