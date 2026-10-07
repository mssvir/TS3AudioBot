using System;

namespace TS3AudioBot.Audio
{
	internal sealed class FfmpegStartAdmissionGate
	{
		private readonly object sync = new object();
		private readonly Func<DateTimeOffset> utcNow;
		private readonly TimeSpan baseCooldown;
		private readonly TimeSpan maxCooldown;
		private readonly TimeSpan probeRetryDelay;

		private int consecutiveResourceFailures;
		private DateTimeOffset blockedUntil = DateTimeOffset.MinValue;
		private bool probeInFlight;

		public FfmpegStartAdmissionGate(
			Func<DateTimeOffset>? utcNow = null,
			TimeSpan? baseCooldown = null,
			TimeSpan? maxCooldown = null,
			TimeSpan? probeRetryDelay = null)
		{
			this.utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
			this.baseCooldown = baseCooldown ?? TimeSpan.FromMilliseconds(250);
			this.maxCooldown = maxCooldown ?? TimeSpan.FromSeconds(5);
			this.probeRetryDelay = probeRetryDelay ?? TimeSpan.FromMilliseconds(250);

			if (this.baseCooldown <= TimeSpan.Zero)
				throw new ArgumentOutOfRangeException(nameof(baseCooldown));
			if (this.maxCooldown < this.baseCooldown)
				throw new ArgumentOutOfRangeException(nameof(maxCooldown));
			if (this.probeRetryDelay <= TimeSpan.Zero)
				throw new ArgumentOutOfRangeException(nameof(probeRetryDelay));
		}

		public bool TryEnter(out TimeSpan retryAfter)
		{
			lock (sync)
			{
				var now = utcNow();
				if (now < blockedUntil)
				{
					retryAfter = blockedUntil - now;
					return false;
				}

				if (consecutiveResourceFailures > 0)
				{
					if (probeInFlight)
					{
						retryAfter = probeRetryDelay;
						return false;
					}

					probeInFlight = true;
				}

				retryAfter = TimeSpan.Zero;
				return true;
			}
		}

		public TimeSpan RecordResourceFailure()
		{
			lock (sync)
			{
				if (consecutiveResourceFailures < 30)
					consecutiveResourceFailures++;

				var cooldown = CalculateCooldown(consecutiveResourceFailures);
				blockedUntil = utcNow() + cooldown;
				probeInFlight = false;
				return cooldown;
			}
		}

		public void RecordSuccess()
		{
			lock (sync)
			{
				consecutiveResourceFailures = 0;
				blockedUntil = DateTimeOffset.MinValue;
				probeInFlight = false;
			}
		}

		public void RecordNonResourceFailure()
		{
			lock (sync)
			{
				// Preserve any previous resource-pressure streak until a real process start
				// proves that the OS can create children again. Only release a half-open probe.
				probeInFlight = false;
			}
		}

		private TimeSpan CalculateCooldown(int failures)
		{
			long ticks = baseCooldown.Ticks;
			for (int i = 1; i < failures && ticks < maxCooldown.Ticks; i++)
			{
				if (ticks >= maxCooldown.Ticks / 2)
				{
					ticks = maxCooldown.Ticks;
					break;
				}
				ticks *= 2;
			}

			return TimeSpan.FromTicks(Math.Min(ticks, maxCooldown.Ticks));
		}
	}
}
