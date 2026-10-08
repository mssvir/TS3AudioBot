#!/usr/bin/env python3
from pathlib import Path

path = Path("TS3AudioBot/Audio/FfmpegProducer.cs")
text = path.read_text(encoding="utf-8")

if "FfmpegStartGate" in text and "DescribeFfmpegStartError" in text:
    print("MSSV P0 FFmpeg EAGAIN patch is already present")
    raise SystemExit(0)

replacements = [
    (
        "using System.IO;\nusing System.Text.RegularExpressions;",
        "using System.IO;\nusing System.Runtime.InteropServices;\nusing System.Text.RegularExpressions;",
    ),
    (
        "\t\tprivate static readonly TimeSpan retryOnDropBeforeEnd = TimeSpan.FromSeconds(10);\n\n\t\tprivate readonly ConfToolsFfmpeg config;",
        "\t\tprivate static readonly TimeSpan retryOnDropBeforeEnd = TimeSpan.FromSeconds(10);\n\t\tprivate static readonly FfmpegStartAdmissionGate FfmpegStartGate = new FfmpegStartAdmissionGate();\n\n\t\tprivate readonly ConfToolsFfmpeg config;",
    ),
    (
        "\t\tprivate R<FfmpegInstance, string> StartFfmpegProcessInternal(FfmpegInstance instance, string arguments)\n\t\t{\n\t\t\ttry",
        "\t\tprivate R<FfmpegInstance, string> StartFfmpegProcessInternal(FfmpegInstance instance, string arguments)\n\t\t{\n\t\t\tif (!FfmpegStartGate.TryEnter(out var retryAfter))\n\t\t\t{\n\t\t\t\tvar error = $\"FFmpeg start temporarily throttled after operating system resource exhaustion. Retry after approximately {Math.Ceiling(retryAfter.TotalMilliseconds):0} ms.\";\n\t\t\t\tLog.Warn(error);\n\t\t\t\tinstance.Close();\n\t\t\t\treturn error;\n\t\t\t}\n\n\t\t\ttry",
    ),
    (
        "\t\t\t\tinstance.FfmpegProcess.Start();\n\t\t\t\tinstance.FfmpegProcess.BeginErrorReadLine();",
        "\t\t\t\tinstance.FfmpegProcess.Start();\n\t\t\t\tFfmpegStartGate.RecordSuccess();\n\t\t\t\tinstance.FfmpegProcess.BeginErrorReadLine();",
    ),
    (
        "\t\t\tcatch (Exception ex)\n\t\t\t{\n\t\t\t\tvar error = ex is Win32Exception\n\t\t\t\t\t? $\"Ffmpeg could not be found ({ex.Message})\"\n\t\t\t\t\t: $\"Unable to create stream ({ex.Message})\";\n\t\t\t\tLog.Error(ex, error);\n\t\t\t\tinstance.Close();\n\t\t\t\tStopFfmpegProcess();\n\t\t\t\treturn error;\n\t\t\t}\n\t\t}\n\n\t\tprivate void StopFfmpegProcess()",
        "\t\t\tcatch (Exception ex)\n\t\t\t{\n\t\t\t\tstring error;\n\t\t\t\tif (IsFfmpegStartResourceExhaustion(ex))\n\t\t\t\t{\n\t\t\t\t\tvar cooldown = FfmpegStartGate.RecordResourceFailure();\n\t\t\t\t\terror = $\"{DescribeFfmpegStartError(ex)} New FFmpeg starts are throttled for approximately {Math.Ceiling(cooldown.TotalMilliseconds):0} ms.\";\n\t\t\t\t}\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\tFfmpegStartGate.RecordNonResourceFailure();\n\t\t\t\t\terror = DescribeFfmpegStartError(ex);\n\t\t\t\t}\n\n\t\t\t\tLog.Error(ex, error);\n\t\t\t\tinstance.Close();\n\t\t\t\tStopFfmpegProcess();\n\t\t\t\treturn error;\n\t\t\t}\n\t\t}\n\n\t\tprivate static bool IsFfmpegStartResourceExhaustion(Exception ex)\n\t\t\t=> RuntimeInformation.IsOSPlatform(OSPlatform.Linux)\n\t\t\t\t&& ex is Win32Exception win32\n\t\t\t\t&& win32.NativeErrorCode == 11;\n\n\t\tprivate static string DescribeFfmpegStartError(Exception ex)\n\t\t{\n\t\t\tif (!(ex is Win32Exception win32))\n\t\t\t\treturn $\"Unable to create stream ({ex.Message})\";\n\n\t\t\tif (win32.NativeErrorCode == 2)\n\t\t\t\treturn $\"Ffmpeg could not be found ({win32.Message})\";\n\n\t\t\tif (IsFfmpegStartResourceExhaustion(ex))\n\t\t\t\treturn $\"Unable to start ffmpeg: operating system process/thread resources are temporarily exhausted (EAGAIN/errno 11: {win32.Message})\";\n\n\t\t\treturn $\"Unable to start ffmpeg (native error {win32.NativeErrorCode}: {win32.Message})\";\n\t\t}\n\n\t\tprivate void StopFfmpegProcess()",
    ),
]

for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"Refusing to patch: expected exactly one anchor, found {count}: {old[:80]!r}")
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("Applied MSSV P0 FFmpeg EAGAIN classification/admission patch")
