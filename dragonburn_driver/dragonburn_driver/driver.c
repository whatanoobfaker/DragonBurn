#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>

#pragma warning(disable: 4201)
#pragma warning(disable: 4214)

/* =================================================================
 * Constants
 * ================================================================= */

#define DRIVER_NAME           L"\\Driver\\dragonburn_driver"
#define DRIVER_DEVICE_NAME    L"\\Device\\dragonburn_driver"
#define DRIVER_SYMBOLIC_LINK  L"\\DosDevices\\dragonburn_driver"

/* Require read/write access on the handle — not FILE_ANY_ACCESS. */
#define IOCTL_READ_MEMORY     CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_GET_MODULE_BASE CTL_CODE(0x8000, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_PING            CTL_CODE(0x8000, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SET_TARGET_PID  CTL_CODE(0x8000, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define PING_MAGIC            0xDEADBEEFCAFEBABEULL
#define MAX_READ_SIZE         0x10000

#define UM_LOW   0x10000ULL
#define UM_HIGH  0x7FFFFFFFFFFFULL

/* =================================================================
 * Structures
 * ================================================================= */

#pragma pack(push, 1)
typedef struct _READ_MEMORY_REQUEST {
    ULONG   target_pid;
    ULONG64 source_address;
    ULONG   read_size;
    ULONG   padding;
} READ_MEMORY_REQUEST, * PREAD_MEMORY_REQUEST;
#pragma pack(pop)

typedef struct _MODULE_BASE_REQUEST {
    ULONG   target_pid;
    WCHAR   module_name[256];
    ULONG64 base_address;
    ULONG   module_size;
    ULONG   padding;
} MODULE_BASE_REQUEST, * PMODULE_BASE_REQUEST;

typedef struct _PING_RESPONSE {
    ULONG64 magic;
} PING_RESPONSE, * PPING_RESPONSE;

typedef struct _SET_TARGET_REQUEST {
    ULONG target_pid;
    ULONG padding;
} SET_TARGET_REQUEST, * PSET_TARGET_REQUEST;

/* =================================================================
 * Global client binding (single authorized usermode process)
 * ================================================================= */

static volatile LONG g_client_pid = 0;   /* 0 = unbound */
static volatile LONG g_target_pid = 0;   /* 0 = no target registered */
static PDEVICE_OBJECT g_device_object = NULL;

/* =================================================================
 * Undocumented exports
 * ================================================================= */

NTSYSAPI NTSTATUS NTAPI MmCopyVirtualMemory(
    IN PEPROCESS SourceProcess, IN PVOID SourceAddress,
    IN PEPROCESS TargetProcess, OUT PVOID TargetAddress,
    IN SIZE_T BufferSize, IN KPROCESSOR_MODE PreviousMode,
    OUT PSIZE_T ReturnSize);

NTSYSAPI PVOID NTAPI PsGetProcessPeb(IN PEPROCESS Process);

NTSYSAPI NTSTATUS NTAPI IoCreateDriver(
    IN PUNICODE_STRING DriverName,
    IN PDRIVER_INITIALIZE InitializationFunction);

/* =================================================================
 * PEB structures
 * ================================================================= */

typedef struct _PEB_LDR_DATA2 {
    ULONG Length; UCHAR Initialized; PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA2, * PPEB_LDR_DATA2;

typedef struct _LDR_DATA_TABLE_ENTRY2 {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase; PVOID EntryPoint; ULONG SizeOfImage;
    UNICODE_STRING FullDllName; UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY2, * PLDR_DATA_TABLE_ENTRY2;

typedef struct _PEB2 {
    UCHAR Reserved1[2]; UCHAR BeingDebugged; UCHAR Reserved2[1];
    PVOID Reserved3[2]; PPEB_LDR_DATA2 Ldr;
} PEB2, * PPEB2;

/* =================================================================
 * Helpers
 * ================================================================= */

static __forceinline BOOLEAN IsValidUserAddress(ULONG64 Address, ULONG Size)
{
    if (Address < UM_LOW)                   return FALSE;
    if (Address > UM_HIGH)                  return FALSE;
    if (Address + Size < Address)           return FALSE;
    if (Address + Size > UM_HIGH)           return FALSE;
    return TRUE;
}

static BOOLEAN IsProcessAlive(HANDLE Pid)
{
    PEPROCESS proc = NULL;
    NTSTATUS status;

    if (!Pid)
        return FALSE;

    status = PsLookupProcessByProcessId(Pid, &proc);
    if (!NT_SUCCESS(status))
        return FALSE;

    if (PsGetProcessExitStatus(proc) != STATUS_PENDING) {
        ObDereferenceObject(proc);
        return FALSE;
    }

    ObDereferenceObject(proc);
    return TRUE;
}

static BOOLEAN IsAuthorizedCaller(VOID)
{
    LONG bound = InterlockedCompareExchange(&g_client_pid, 0, 0);
    HANDLE caller = PsGetCurrentProcessId();

    if (bound == 0)
        return FALSE;

    if ((LONG)(ULONG_PTR)caller != bound)
        return FALSE;

    return TRUE;
}

static BOOLEAN IsAllowedTarget(ULONG Pid)
{
    LONG target = InterlockedCompareExchange(&g_target_pid, 0, 0);
    if (target == 0)
        return FALSE;
    return (LONG)Pid == target;
}

/* =================================================================
 * Read process memory
 * ================================================================= */

static NTSTATUS KernelReadProcessMemory(
    ULONG   TargetPid,
    ULONG64 SourceAddress,
    PVOID   DestBuffer,
    SIZE_T  Size,
    PSIZE_T BytesRead)
{
    NTSTATUS  status;
    PEPROCESS proc = NULL;
    SIZE_T    bytes = 0;

    if (BytesRead) *BytesRead = 0;

    if (!DestBuffer || Size == 0)
        return STATUS_INVALID_PARAMETER;

    if (!IsValidUserAddress(SourceAddress, (ULONG)Size))
        return STATUS_ACCESS_VIOLATION;

    status = PsLookupProcessByProcessId(
        (HANDLE)(ULONG_PTR)TargetPid, &proc);
    if (!NT_SUCCESS(status))
        return status;

    if (PsGetProcessExitStatus(proc) != STATUS_PENDING) {
        ObDereferenceObject(proc);
        return STATUS_PROCESS_IS_TERMINATING;
    }

    status = MmCopyVirtualMemory(
        proc,
        (PVOID)(ULONG_PTR)SourceAddress,
        PsGetCurrentProcess(),
        DestBuffer,
        Size,
        KernelMode,
        &bytes);

    ObDereferenceObject(proc);

    if (BytesRead) *BytesRead = bytes;
    return status; /* Propagate real failure — never fake SUCCESS */
}

/* =================================================================
 * Get module base
 * ================================================================= */

static NTSTATUS KernelGetModuleBase(
    ULONG    TargetPid,
    PCWSTR   ModuleName,
    PULONG64 OutBase,
    PULONG   OutSize)
{
    NTSTATUS   status = STATUS_NOT_FOUND;
    PEPROCESS  proc = NULL;
    KAPC_STATE apc;
    int        count = 0;

    *OutBase = 0;
    *OutSize = 0;

    status = PsLookupProcessByProcessId(
        (HANDLE)(ULONG_PTR)TargetPid, &proc);
    if (!NT_SUCCESS(status)) return status;

    if (PsGetProcessExitStatus(proc) != STATUS_PENDING) {
        ObDereferenceObject(proc);
        return STATUS_PROCESS_IS_TERMINATING;
    }

    KeStackAttachProcess(proc, &apc);

    __try {
        PPEB2 peb = (PPEB2)PsGetProcessPeb(proc);
        if (!peb || !peb->Ldr || !peb->Ldr->Initialized) {
            status = STATUS_UNSUCCESSFUL;
            __leave;
        }

        {
            LIST_ENTRY* head = &peb->Ldr->InLoadOrderModuleList;
            LIST_ENTRY* cur = head->Flink;
            UNICODE_STRING target;

            RtlInitUnicodeString(&target, ModuleName);

            while (cur != head && count < 2000) {
                PLDR_DATA_TABLE_ENTRY2 entry = CONTAINING_RECORD(
                    cur, LDR_DATA_TABLE_ENTRY2, InLoadOrderLinks);

                if (entry->BaseDllName.Buffer &&
                    entry->BaseDllName.Length > 0 &&
                    entry->DllBase)
                {
                    if (RtlCompareUnicodeString(
                        &entry->BaseDllName, &target, TRUE) == 0)
                    {
                        *OutBase = (ULONG64)entry->DllBase;
                        *OutSize = entry->SizeOfImage;
                        status = STATUS_SUCCESS;
                        __leave;
                    }
                }
                cur = cur->Flink;
                count++;
            }
        }
        status = STATUS_NOT_FOUND;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    return status;
}

/* =================================================================
 * IRP handlers
 * ================================================================= */

NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DevObj);

    if (stack->MajorFunction == IRP_MJ_CREATE) {
        HANDLE caller = PsGetCurrentProcessId();
        LONG bound = InterlockedCompareExchange(&g_client_pid, 0, 0);

        if (bound != 0 && (LONG)(ULONG_PTR)caller != bound) {
            if (IsProcessAlive((HANDLE)(ULONG_PTR)bound)) {
                status = STATUS_ACCESS_DENIED;
                DbgPrint("[MemReader] CREATE denied: bound to pid %ld, caller %p\n",
                    bound, caller);
            } else {
                /* Previous client died — allow rebind */
                InterlockedExchange(&g_client_pid, (LONG)(ULONG_PTR)caller);
                InterlockedExchange(&g_target_pid, 0);
                DbgPrint("[MemReader] Rebound client to pid %p\n", caller);
            }
        } else if (bound == 0) {
            InterlockedExchange(&g_client_pid, (LONG)(ULONG_PTR)caller);
            InterlockedExchange(&g_target_pid, 0);
            DbgPrint("[MemReader] Bound client pid %p\n", caller);
        }
    } else if (stack->MajorFunction == IRP_MJ_CLOSE) {
        HANDLE caller = PsGetCurrentProcessId();
        LONG bound = InterlockedCompareExchange(&g_client_pid, 0, 0);
        if (bound != 0 && (LONG)(ULONG_PTR)caller == bound) {
            InterlockedExchange(&g_client_pid, 0);
            InterlockedExchange(&g_target_pid, 0);
            DbgPrint("[MemReader] Client pid %p closed — unbound\n", caller);
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    NTSTATUS           status = STATUS_SUCCESS;
    ULONG              bytesRet = 0;
    PIO_STACK_LOCATION stack;
    PVOID              buf;
    ULONG              inLen, outLen, code;

    UNREFERENCED_PARAMETER(DevObj);

    stack = IoGetCurrentIrpStackLocation(Irp);
    buf = Irp->AssociatedIrp.SystemBuffer;
    inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    code = stack->Parameters.DeviceIoControl.IoControlCode;

    if (!IsAuthorizedCaller()) {
        status = STATUS_ACCESS_DENIED;
        goto complete;
    }

    switch (code) {

    case IOCTL_PING:
    {
        if (outLen < sizeof(PING_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        ((PPING_RESPONSE)buf)->magic = PING_MAGIC;
        bytesRet = sizeof(PING_RESPONSE);
        break;
    }

    case IOCTL_SET_TARGET_PID:
    {
        SET_TARGET_REQUEST req;

        if (inLen < sizeof(SET_TARGET_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        RtlCopyMemory(&req, buf, sizeof(SET_TARGET_REQUEST));

        if (req.target_pid == 0 || !IsProcessAlive((HANDLE)(ULONG_PTR)req.target_pid)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        InterlockedExchange(&g_target_pid, (LONG)req.target_pid);
        DbgPrint("[MemReader] Target pid set to %lu\n", req.target_pid);
        bytesRet = 0;
        break;
    }

    case IOCTL_READ_MEMORY:
    {
        READ_MEMORY_REQUEST req;
        SIZE_T br = 0;

        if (inLen < sizeof(READ_MEMORY_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        RtlCopyMemory(&req, buf, sizeof(READ_MEMORY_REQUEST));

        if (req.read_size == 0 || req.read_size > MAX_READ_SIZE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (outLen < req.read_size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (!IsAllowedTarget(req.target_pid)) {
            status = STATUS_ACCESS_DENIED;
            break;
        }

        status = KernelReadProcessMemory(
            req.target_pid,
            req.source_address,
            buf,
            (SIZE_T)req.read_size,
            &br);

        if (NT_SUCCESS(status)) {
            bytesRet = (ULONG)br;
            /* Partial copy is still a failure for our protocol */
            if (br != (SIZE_T)req.read_size) {
                status = STATUS_PARTIAL_COPY;
                bytesRet = (ULONG)br;
            }
        } else {
            bytesRet = 0;
        }
        break;
    }

    case IOCTL_GET_MODULE_BASE:
    {
        ULONG64 baseAddr = 0;
        ULONG   modSize = 0;

        if (inLen < sizeof(MODULE_BASE_REQUEST) ||
            outLen < sizeof(MODULE_BASE_REQUEST))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (!IsAllowedTarget(((PMODULE_BASE_REQUEST)buf)->target_pid)) {
            status = STATUS_ACCESS_DENIED;
            break;
        }

        ((PMODULE_BASE_REQUEST)buf)->module_name[255] = L'\0';

        status = KernelGetModuleBase(
            ((PMODULE_BASE_REQUEST)buf)->target_pid,
            ((PMODULE_BASE_REQUEST)buf)->module_name,
            &baseAddr,
            &modSize);

        if (NT_SUCCESS(status)) {
            ((PMODULE_BASE_REQUEST)buf)->base_address = baseAddr;
            ((PMODULE_BASE_REQUEST)buf)->module_size = modSize;
            bytesRet = sizeof(MODULE_BASE_REQUEST);
        } else {
            ((PMODULE_BASE_REQUEST)buf)->base_address = 0;
            ((PMODULE_BASE_REQUEST)buf)->module_size = 0;
            bytesRet = 0;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

complete:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesRet;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS UnsupportedDispatch(PDEVICE_OBJECT device_obj, PIRP irp) {
    UNREFERENCED_PARAMETER(device_obj);

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return irp->IoStatus.Status;
}

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, DRIVER_SYMBOLIC_LINK);
    IoDeleteSymbolicLink(&symLink);

    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    g_device_object = NULL;
    InterlockedExchange(&g_client_pid, 0);
    InterlockedExchange(&g_target_pid, 0);
    DbgPrint("[MemReader] Unloaded\n");
}

/* =================================================================
 * Entry
 * ================================================================= */

NTSTATUS RealDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint("[MemReader] === RealDriverEntry START ===\n");

    UNICODE_STRING devName, symLink;
    PDEVICE_OBJECT devObj = NULL;
    NTSTATUS status;

    RtlInitUnicodeString(&devName, DRIVER_DEVICE_NAME);
    RtlInitUnicodeString(&symLink, DRIVER_SYMBOLIC_LINK);

    IoDeleteSymbolicLink(&symLink);

    /* Exclusive=TRUE: only one usermode handle at a time */
    status = IoCreateDevice(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        TRUE,
        &devObj
    );

    DbgPrint("[MemReader] IoCreateDevice = 0x%08X\n", status);

    if (status == STATUS_OBJECT_NAME_COLLISION) {
        DbgPrint("[MemReader] ERROR: Device name collision - driver already loaded?\n");
        return status;
    }

    if (!NT_SUCCESS(status)) {
        DbgPrint("[MemReader] ERROR: IoCreateDevice failed with 0x%08X\n", status);
        return status;
    }

    g_device_object = devObj;

    status = IoCreateSymbolicLink(&symLink, &devName);
    DbgPrint("[MemReader] IoCreateSymbolicLink = 0x%08X\n", status);

    if (!NT_SUCCESS(status)) {
        DbgPrint("[MemReader] ERROR: SymLink creation failed\n");
        IoDeleteDevice(devObj);
        g_device_object = NULL;
        return status;
    }

    devObj->Flags |= DO_BUFFERED_IO;

    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = UnsupportedDispatch;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;

    /* SCM loads can unload; kdmapper ignores this but it is correct for service path */
    DriverObject->DriverUnload = DriverUnload;

    InterlockedExchange(&g_client_pid, 0);
    InterlockedExchange(&g_target_pid, 0);

    devObj->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("[MemReader] === Driver fully initialized (exclusive + client bind) ===\n");
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING drvName;
    RtlInitUnicodeString(&drvName, DRIVER_NAME);

    return IoCreateDriver(&drvName, &RealDriverEntry);
}
