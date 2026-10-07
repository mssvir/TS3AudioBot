using NUnit.Framework;
using System;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.InteropServices;
using TS3AudioBot.Audio;

namespace TS3ABotUnitTests
{
	[TestFixture]
	public class FfmpegProducerTests
	{
		private static string DescribeStartError(Exception exception)
		{
			var method = typeof(FfmpegProducer).GetMethod(
				"DescribeFfmpegStartError",
				BindingFlags.NonPublic | BindingFlags.Static);

			Assert.NotNull(method, "FFmpeg process-start error classifier must remain testable");
			return (string)method.Invoke(null, new object[] { exception });
		}

		[Test]
		public void MissingExecutableIsReportedAsMissingFfmpeg()
		{
			var result = DescribeStartError(new Win32Exception(2));
			StringAssert.Contains("Ffmpeg could not be found", result);
		}

		[Test]
		public void GenericNativeErrorIsNotReportedAsMissingFfmpeg()
		{
			var result = DescribeStartError(new Win32Exception(13));
			StringAssert.Contains("native error 13", result);
			StringAssert.DoesNotContain("could not be found", result);
		}

		[Test]
		public void NonNativeFailureKeepsGenericStreamError()
		{
			var result = DescribeStartError(new InvalidOperationException("synthetic failure"));
			StringAssert.Contains("Unable to create stream", result);
			StringAssert.Contains("synthetic failure", result);
		}

		[Test]
		public void LinuxEagainIsReportedAsResourceExhaustion()
		{
			if (!RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
				Assert.Ignore("Linux errno semantics are only asserted on Linux");

			var result = DescribeStartError(new Win32Exception(11));
			StringAssert.Contains("resources are temporarily exhausted", result);
			StringAssert.Contains("EAGAIN/errno 11", result);
			StringAssert.DoesNotContain("could not be found", result);
		}
	}
}
