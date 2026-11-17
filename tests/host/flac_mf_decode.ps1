# flac_mf_decode.ps1: decodes a FLAC file with Windows' own decoder (Media Foundation, the "Microsoft FLAC Audio Decoder") to raw PCM: an implementation of the
# format that has nothing to do with this project, used by tests/host/flac_reference_check.py to check what the test encoder writes. Windows 10 (1607) or later.
#   powershell -File flac_mf_decode.ps1 -In file.flac -Out file.raw        (prints "native NchHHHz Nbit; decoded ...")
#   powershell -File flac_mf_decode.ps1 -Dir folder                        (every *.flac in it, to NAME.mf.raw; a file Windows refuses gets NAME.mf.err; prints a count)
# The COM interfaces are declared by hand (the whole vtable of IMFAttributes in every interface that derives from it, because the CLR does not flatten them for us
# in a way that works here); only the calls the decoder needs are given real signatures.
param([string]$In, [string]$Out, [string]$Dir)
$ErrorActionPreference = "Stop"
$code = @"
using System;
using System.IO;
using System.Runtime.InteropServices;

[ComImport, Guid("2cd2d921-c447-44a7-a13c-4adabfc247e3"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMFAttributes {
    void GetItem(); void GetItemType(); void CompareItem(); void Compare();
    void GetUINT32(ref Guid key, out uint value);
    void GetUINT64(); void GetDouble(); void GetGUID(); void GetStringLength(); void GetString(); void GetAllocatedString(); void GetBlobSize(); void GetBlob(); void GetAllocatedBlob(); void GetUnknown();
    void SetItem(); void DeleteItem(); void DeleteAllItems();
    void SetUINT32(ref Guid key, uint value);
    void SetUINT64(); void SetDouble();
    void SetGUID(ref Guid key, ref Guid value);
    void SetString(); void SetBlob(); void SetUnknown(); void LockStore(); void UnlockStore(); void GetCount(); void GetItemByIndex(); void CopyAllItems();
}

[ComImport, Guid("44ae0fa8-ea31-4109-8d2e-4cae4997c555"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMFMediaType {
    void GetItem(); void GetItemType(); void CompareItem(); void Compare();
    void GetUINT32(ref Guid key, out uint value);
    void GetUINT64(); void GetDouble(); void GetGUID(); void GetStringLength(); void GetString(); void GetAllocatedString(); void GetBlobSize(); void GetBlob(); void GetAllocatedBlob(); void GetUnknown();
    void SetItem(); void DeleteItem(); void DeleteAllItems();
    void SetUINT32(ref Guid key, uint value);
    void SetUINT64(); void SetDouble();
    void SetGUID(ref Guid key, ref Guid value);
    void SetString(); void SetBlob(); void SetUnknown(); void LockStore(); void UnlockStore(); void GetCount(); void GetItemByIndex(); void CopyAllItems();
    void GetMajorType(); void IsCompressedFormat(); void IsEqual(); void GetRepresentation(); void FreeRepresentation();
}

[ComImport, Guid("045fa593-8799-42b8-bc8d-8968c6453507"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMFMediaBuffer {
    void Lock(out IntPtr buffer, out int maxLength, out int currentLength);
    void Unlock();
    void GetCurrentLength(out int length);
    void SetCurrentLength(int length);
    void GetMaxLength(out int length);
}

[ComImport, Guid("c40a00f2-b93a-4d80-ae8c-5a1c634f58e4"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMFSample {
    void GetItem(); void GetItemType(); void CompareItem(); void Compare();
    void GetUINT32(ref Guid key, out uint value);
    void GetUINT64(); void GetDouble(); void GetGUID(); void GetStringLength(); void GetString(); void GetAllocatedString(); void GetBlobSize(); void GetBlob(); void GetAllocatedBlob(); void GetUnknown();
    void SetItem(); void DeleteItem(); void DeleteAllItems();
    void SetUINT32(ref Guid key, uint value);
    void SetUINT64(); void SetDouble();
    void SetGUID(ref Guid key, ref Guid value);
    void SetString(); void SetBlob(); void SetUnknown(); void LockStore(); void UnlockStore(); void GetCount(); void GetItemByIndex(); void CopyAllItems();
    void GetSampleFlags(); void SetSampleFlags(); void GetSampleTime(); void SetSampleTime(); void GetSampleDuration(); void SetSampleDuration(); void GetBufferCount(); void GetBufferByIndex();
    void ConvertToContiguousBuffer(out IMFMediaBuffer buffer);
}

[ComImport, Guid("70ae66f2-c809-4e4f-8915-bdcb406b7993"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMFSourceReader {
    void GetStreamSelection(uint index, out bool selected);
    void SetStreamSelection(uint index, bool selected);
    void GetNativeMediaType(uint stream, uint index, out IMFMediaType type);
    void GetCurrentMediaType(uint stream, out IMFMediaType type);
    void SetCurrentMediaType(uint stream, IntPtr reserved, IMFMediaType type);
    void SetCurrentPosition();
    void ReadSample(uint stream, uint flags, out uint actualStream, out uint streamFlags, out long timestamp, out IMFSample sample);
    void Flush(uint stream);
}

public static class MF {
    [DllImport("mfplat.dll")] static extern int MFStartup(uint version, uint flags);
    [DllImport("mfplat.dll")] static extern int MFCreateMediaType(out IMFMediaType type);
    [DllImport("mfreadwrite.dll", CharSet = CharSet.Unicode)] static extern int MFCreateSourceReaderFromURL(string url, IntPtr attrs, out IMFSourceReader reader);

    static Guid MAJOR = new Guid("48eba18e-f8c9-4687-bf11-0a74c9f96a8f");
    static Guid SUBTYPE = new Guid("f7e34c9a-42e8-4714-b74b-cb29d72c35e5");
    static Guid AUDIO = new Guid("73647561-0000-0010-8000-00AA00389B71");
    static Guid PCM = new Guid("00000001-0000-0010-8000-00AA00389B71");
    static Guid CHANNELS = new Guid("37e48bf5-645e-4c5b-89de-ada9e29b696a");
    static Guid RATE = new Guid("5faeeae7-0290-4c31-9e8a-c534f68d9dba");
    static Guid BITS = new Guid("f2deb57f-40fa-4764-aa33-ed4f2d1ff669");

    static uint U32(IMFMediaType t, Guid key) { uint v; try { t.GetUINT32(ref key, out v); } catch (Exception) { v = 0xFFFFFFFF; } return v; }
    static void Check(int hr, string what) { if (hr < 0) throw new Exception(what + " failed: 0x" + hr.ToString("X8")); }

    public static string Decode(string input, string output, uint wantBits) {
        string step = "start";
        try {
            step = "MFStartup"; Check(MFStartup(0x00020070, 0), "MFStartup");
            IMFSourceReader reader;
            step = "create reader"; Check(MFCreateSourceReaderFromURL(input, IntPtr.Zero, out reader), "MFCreateSourceReaderFromURL");
            IMFMediaType native;
            step = "native type"; reader.GetNativeMediaType(0xFFFFFFFD, 0, out native);
            uint nch = U32(native, CHANNELS), nrate = U32(native, RATE), nbits = U32(native, BITS);
            IMFMediaType want;
            step = "create type"; Check(MFCreateMediaType(out want), "MFCreateMediaType");
            step = "set major"; want.SetGUID(ref MAJOR, ref AUDIO);
            step = "set subtype"; want.SetGUID(ref SUBTYPE, ref PCM);
            if (wantBits != 0) { step = "set bits"; want.SetUINT32(ref BITS, wantBits); }
            step = "SetCurrentMediaType"; reader.SetCurrentMediaType(0xFFFFFFFD, IntPtr.Zero, want);
            IMFMediaType cur;
            step = "GetCurrentMediaType"; reader.GetCurrentMediaType(0xFFFFFFFD, out cur);
            uint ch = U32(cur, CHANNELS), rate = U32(cur, RATE), bits = U32(cur, BITS);
            using (var f = new FileStream(output, FileMode.Create)) {
                for (;;) {
                    uint stream, flags; long ts; IMFSample sample;
                    step = "ReadSample"; reader.ReadSample(0xFFFFFFFD, 0, out stream, out flags, out ts, out sample);
                    if ((flags & 2) != 0) break;
                    if (sample == null) continue;
                    IMFMediaBuffer buf;
                    step = "contiguous"; sample.ConvertToContiguousBuffer(out buf);
                    IntPtr p; int max, len;
                    step = "lock"; buf.Lock(out p, out max, out len);
                    byte[] bytes = new byte[len];
                    Marshal.Copy(p, bytes, 0, len);
                    buf.Unlock();
                    f.Write(bytes, 0, len);
                }
            }
            return "native " + nch + "ch " + nrate + "Hz " + nbits + "bit; decoded " + ch + "ch " + rate + "Hz " + bits + "bit";
        } catch (Exception e) {
            return "FAILED at step '" + step + "': " + e.Message;
        }
    }
}
"@
Add-Type -TypeDefinition $code -Language CSharp
if ($Dir) {
    $n = 0; $refused = 0
    foreach ($f in (Get-ChildItem "$Dir\*.flac")) {
        $r = [MF]::Decode($f.FullName, ($f.FullName -replace '\.flac$', '.mf.raw'), 0)
        if ($r -like "FAILED*") { $refused++; Set-Content -Path ($f.FullName -replace '\.flac$', '.mf.err') -Value $r }
        $n++
    }
    "decoded $n files, $refused refused by Windows"
} else {
    [MF]::Decode($In, $Out, 0)
}
