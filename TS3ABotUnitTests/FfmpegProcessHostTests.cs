using NUnit.Framework;
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using TS3AudioBot.Audio;

namespace TS3ABotUnitTests
{
	[TestFixture]
	public class FfmpegProcessHostTests
	{
		[Test]
		public void ManagedBackendRemainsTheDefaultSelection()
		{
			var decision = FfmpegProcessBackendSelector.Select(
				FfmpegProcessBackend.Managed,
				nativeAvailable: false,
				allowManagedFallback: false);

			Assert.True(decision.Ok);
			Assert.AreEqual(FfmpegProcessBackend.Managed, decision.Selected);
			Assert.False(decision.FallbackUsed);
		}

		[Test]
		public void NativeCanarySelectsNativeOnlyWhenAvailable()
		{
			var decision = FfmpegProcessBackendSelector.Select(
				FfmpegProcessBackend.NativeCanary,
				nativeAvailable: true,
				allowManagedFallback: true);

			Assert.True(decision.Ok);
			Assert.AreEqual(FfmpegProcessBackend.NativeCanary, decision.Selected);
			Assert.False(decision.FallbackUsed);
		}

		[Test]
		public void UnavailableNativeCanaryFallsBackOnlyWhenExplicitlyAllowed()
		{
			var decision = FfmpegProcessBackendSelector.Select(
				FfmpegProcessBackend.NativeCanary,
				nativeAvailable: false,
				allowManagedFallback: true);

			Assert.True(decision.Ok);
			Assert.AreEqual(FfmpegProcessBackend.Managed, decision.Selected);
			Assert.True(decision.FallbackUsed);
		}

		[Test]
		public void UnavailableNativeCanaryFailsClosedWhenFallbackIsDisabled()
		{
			var decision = FfmpegProcessBackendSelector.Select(
				FfmpegProcessBackend.NativeCanary,
				nativeAvailable: false,
				allowManagedFallback: false);

			Assert.False(decision.Ok);
			Assert.NotNull(decision.Error);
			StringAssert.Contains("fallback is disabled", decision.Error);
		}

		[Test]
		public void ManagedHostPreservesStdinStdoutAndStderrContractsOnLinux()
		{
			if (!RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
				Assert.Ignore("The managed process-host parity fixture uses /bin/sh and is Linux-only");

			string? stderrLine = null;
			using var stderrReady = new ManualResetEventSlim(false);
			using var handle = ManagedFfmpegProcessHost.Instance.Start(
				"/bin/sh",
				"-c \"read line; printf '%s' \\\"$line\\\"; printf 'diagnostic\\n' >&2\"",
				line =>
				{
					stderrLine = line;
					stderrReady.Set();
				});

			var input = Encoding.UTF8.GetBytes("round-trip\n");
			handle.StandardInput.Write(input, 0, input.Length);
			handle.StandardInput.Flush();

			using var output = new MemoryStream();
			handle.StandardOutput.CopyTo(output);
			Assert.AreEqual("round-trip", Encoding.UTF8.GetString(output.ToArray()));
			Assert.True(stderrReady.Wait(TimeSpan.FromSeconds(2)), "stderr callback did not arrive");
			Assert.AreEqual("diagnostic", stderrLine);

			var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
			while (!handle.HasExitedSafe() && DateTime.UtcNow < deadline)
				Thread.Sleep(10);
			Assert.True(handle.HasExitedSafe(), "managed parity fixture did not exit");
			handle.CancelErrorRead();
		}
	}
}
