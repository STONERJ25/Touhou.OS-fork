/*
 * Hand-written UEFI definitions: just the parts of the spec the loader calls.
 * Table layouts are checked against the spec's byte offsets with static
 * asserts, so a miscounted padding field fails the build instead of the boot.
 */
#ifndef LOADER_UEFI_H
#define LOADER_UEFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef UINT64    UINTN;
typedef UINT16    CHAR16;
typedef UINT8     BOOLEAN;

typedef UINTN     EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))

#define EFI_ERR(n)               (0x8000000000000000ULL | (n))
#define EFI_SUCCESS              0
#define EFI_LOAD_ERROR           EFI_ERR(1)
#define EFI_INVALID_PARAMETER    EFI_ERR(2)
#define EFI_UNSUPPORTED          EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE      EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL     EFI_ERR(5)
#define EFI_NOT_READY            EFI_ERR(6)
#define EFI_OUT_OF_RESOURCES     EFI_ERR(9)
#define EFI_NOT_FOUND            EFI_ERR(14)
#define EFI_ERROR(s)             (((INT64)(s)) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- memory ---- */

enum { AllocateAnyPages = 0, AllocateMaxAddress = 1, AllocateAddress = 2 };

enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
};

/* The stride between descriptors is DescriptorSize from GetMemoryMap, never sizeof(this). */
typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- text output ---- */

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

/* ---- boot services ---- */

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(UINT32 Type, UINT32 MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
                                      UINTN *DescriptorSize, UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(UINT32 PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
} EFI_BOOT_SERVICES;

_Static_assert(offsetof(EFI_BOOT_SERVICES, AllocatePages)    == 0x28,  "AllocatePages offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, GetMemoryMap)     == 0x38,  "GetMemoryMap offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, HandleProtocol)   == 0x98,  "HandleProtocol offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, ExitBootServices) == 0xE8,  "ExitBootServices offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, Stall)            == 0xF8,  "Stall offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, SetWatchdogTimer) == 0x100, "SetWatchdogTimer offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, LocateProtocol)   == 0x140, "LocateProtocol offset");

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    void *GetVariable;
    void *GetNextVariableName;
    void *SetVariable;
    void *GetNextHighMonotonicCount;
    void *ResetSystem;
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    UINT32                           Pad;
    EFI_HANDLE                       ConsoleInHandle;
    void                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    void                            *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConOut)               == 0x40, "ConOut offset");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, BootServices)         == 0x60, "BootServices offset");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConfigurationTable)   == 0x70, "ConfigurationTable offset");

/* ---- loaded image / file system ---- */

typedef struct {
    UINT32            Revision;
    UINT32            Pad;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE        DeviceHandle;
    void             *FilePath;
    void             *Reserved;
    UINT32            LoadOptionsSize;
    UINT32            Pad2;
    void             *LoadOptions;
    void             *ImageBase;
    UINT64            ImageSize;
    UINT32            ImageCodeType;
    UINT32            ImageDataType;
    void             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

_Static_assert(offsetof(EFI_LOADED_IMAGE_PROTOCOL, DeviceHandle) == 0x18, "DeviceHandle offset");

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName,
                              UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, void *Buffer);
    void *SetInfo;
    void *Flush;
};

_Static_assert(offsetof(EFI_FILE_PROTOCOL, Read)    == 0x20, "File.Read offset");
_Static_assert(offsetof(EFI_FILE_PROTOCOL, GetInfo) == 0x40, "File.GetInfo offset");

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
};

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT64   Size;
    UINT64   FileSize;
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[1];
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

/* ---- graphics output ---- */

#define PixelRedGreenBlueReserved8BitPerColor 0
#define PixelBlueGreenRedReserved8BitPerColor 1
#define PixelBitMask                          2
#define PixelBltOnly                          3

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32            Version;
    UINT32            HorizontalResolution;
    UINT32            VerticalResolution;
    UINT32            PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32            PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber, UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ---- GUIDs ---- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x81, 0x88 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

#endif /* LOADER_UEFI_H */
