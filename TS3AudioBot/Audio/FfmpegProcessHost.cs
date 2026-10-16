// TS3AudioBot - An advanced Musicbot for Teamspeak 3
// Copyright (C) 2017  TS3AudioBot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the Open Software License v. 3.0

using System;
using System.Diagnostics;
using System.IO;
using TSLib.Helper;

namespace TS3AudioBot.Audio
{
	internal enum FfmpegProcessBackend
	{
		Managed,
		NativeCanary,
	}

	internal readonly struct FfmpegProcessBackendDecision
	{
		public FfmpegProcessBackend Requested { get; }
		public FfmpegProcessBackend Selected { get; }
		public bool FallbackUsed { get; }
		public string? Error { get; }
		public bool Ok => Error is null;

		private FfmpegProcessBackendDecision(
			FfmpegProcessBackend requested,
			FfmpegProcessBackend selected,
			bool fallbackUsed,
			string? error)
		{
			Requested = requested;
			Selected = selected;
			FallbackUsed = fallbackUsed;
			Error = error;
		}

		public static FfmpegProcessBackendDecision SelectedBackend(
			FfmpegProcessBackend requested,
			FfmpegProcessBackend selected,
			bool fallbackUsed = false)
			=> new FfmpegProcessBackendDecision(requested, selected, fallbackUsed, null);

		public static FfmpegProcessBackendDecision Unavailable(
			FfmpegProcessBackend requested,
			string error)
			=> new FfmpegProcessBackendDecision(requested, FfmpegProcessBackend.Managed, false, error);
	}

	internal static class FfmpegProcessBackendSelector
	{
		public static FfmpegProcessBackendDecision Select(
			FfmpegProcessBackend requested,
			bool nativeAvailable,
			bool allowManagedFallback)
		{
			if (requested == FfmpegProcessBackend.Managed)
				return FfmpegProcessBackendDecision.SelectedBackend(requested, FfmpegProcessBackend.Managed);

			if (nativeAvailable)
				return FfmpegProcessBackendDecision.SelectedBackend(requested, FfmpegProcessBackend.NativeCanary);

			if (allowManagedFallback)
				return FfmpegProcessBackendDecision.SelectedBackend(requested, FfmpegProcessBackend.Managed, fallbackUsed: true);

			return FfmpegProcessBackendDecision.Unavailable(
				requested,
				"Native FFmpeg process host is unavailable and managed fallback is disabled.");
		}
	}

	internal interface IFfmpegProcessHandle : IDisposable
	{
		Stream StandardOutput { get; }
		Stream StandardInput { get; }
		bool HasExitedSafe();
		void Kill();
		void CancelErrorRead();
	}

	internal interface IFfmpegProcessHost
	{
		IFfmpegProcessHandle Start(string executable, string arguments, Action<string> onErrorLine);
	}

	internal sealed class ManagedFfmpegProcessHost : IFfmpegProcessHost
	{
		public static ManagedFfmpegProcessHost Instance { get; } = new ManagedFfmpegProcessHost();

		private ManagedFfmpegProcessHost() { }

		public IFfmpegProcessHandle Start(string executable, string arguments, Action<string> onErrorLine)
		{
			if (executable is null)
				throw new ArgumentNullException(nameof(executable));
			if (arguments is null)
				throw new ArgumentNullException(nameof(arguments));
			if (onErrorLine is null)
				throw new ArgumentNullException(nameof(onErrorLine));

			var process = new Process
			{
				StartInfo = new ProcessStartInfo
				{
					FileName = executable,
					Arguments = arguments,
					RedirectStandardOutput = true,
					RedirectStandardInput = true,
					RedirectStandardError = true,
					UseShellExecute = false,
					CreateNoWindow = true,
				},
				EnableRaisingEvents = true,
			};

			process.ErrorDataReceived += (_, eventArgs) =>
			{
				if (eventArgs.Data != null)
					onErrorLine(eventArgs.Data);
			};

			try
			{
				process.Start();
				process.BeginErrorReadLine();
				return new ManagedFfmpegProcessHandle(process);
			}
			catch
			{
				process.Dispose();
				throw;
			}
		}
	}

	internal sealed class ManagedFfmpegProcessHandle : IFfmpegProcessHandle
	{
		private readonly Process process;
		private bool disposed;

		public ManagedFfmpegProcessHandle(Process process)
		{
			this.process = process ?? throw new ArgumentNullException(nameof(process));
		}

		public Stream StandardOutput => process.StandardOutput.BaseStream;
		public Stream StandardInput => process.StandardInput.BaseStream;
		public bool HasExitedSafe() => process.HasExitedSafe();
		public void Kill() => process.Kill();

		public void CancelErrorRead()
		{
			if (!disposed)
				process.CancelErrorRead();
		}

		public void Dispose()
		{
			if (disposed)
				return;
			disposed = true;
			process.Dispose();
		}
	}
}
