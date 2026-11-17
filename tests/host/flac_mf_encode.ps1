# flac_mf_encode.ps1: encodes a WAV file to FLAC with Windows' own encoder (Windows.Media.Transcoding, MediaEncodingProfile.CreateFlac): the stream it writes says
# "reference libFLAC" in its VORBIS_COMMENT, so these are files made by the reference encoder, which the decoder has to read, and their STREAMINFO MD5 is
# computed by that encoder from the samples it was given. Used by tests/host/flac_reference_check.py. Windows 10 (1607) or later.
#   powershell -File flac_mf_encode.ps1 -Wav in.wav -OutFlac out.flac -Rate 44100 -Channels 2 -Bits 16
param([string]$Wav, [string]$OutFlac, [int]$Rate = 44100, [int]$Channels = 2, [int]$Bits = 16, [string]$Quality = "High")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFolder, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime]
$asTaskOp = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$asTaskProg = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1' })[0]
function Await($op, $type) { $t = $asTaskOp.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(-1) | Out-Null; return $t.Result }
$src = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Wav)) ([Windows.Storage.StorageFile])
$dir = Split-Path $OutFlac
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($dir)) ([Windows.Storage.StorageFolder])
$dst = Await ($folder.CreateFileAsync((Split-Path $OutFlac -Leaf), [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
$q = [Windows.Media.MediaProperties.AudioEncodingQuality]::$Quality
$profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateFlac($q)
$profile.Audio.SampleRate = $Rate
$profile.Audio.ChannelCount = $Channels
$profile.Audio.BitsPerSample = $Bits
$tc = New-Object Windows.Media.Transcoding.MediaTranscoder
$prep = Await ($tc.PrepareFileTranscodeAsync($src, $dst, $profile)) ([Windows.Media.Transcoding.PrepareTranscodeResult])
if (-not $prep.CanTranscode) { "cannot transcode: " + $prep.FailureReason; exit 1 }
$action = $prep.TranscodeAsync()
$t = $asTaskProg.MakeGenericMethod([double]).Invoke($null, @($action))
$t.Wait(-1) | Out-Null
"encoded " + (Get-Item $OutFlac).Length + " bytes"
