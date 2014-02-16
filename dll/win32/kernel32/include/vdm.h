/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS System Libraries
 * FILE:            dll/win32/kernel32/include/vdm.h
 * PURPOSE:         Virtual DOS Machines (VDM) Support Definitions
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

#pragma once

/* CONSTANTS ******************************************************************/

typedef enum _VDM_ENTRY_CODE
{
    VdmEntryUndo,
    VdmEntryUpdateProcess,
    VdmEntryUpdateControlCHandler
} VDM_ENTRY_CODE;

//
// Undo States
//
#define VDM_UNDO_PARTIAL    0x01
#define VDM_UNDO_FULL       0x02
#define VDM_UNDO_REUSE      0x04
#define VDM_UNDO_COMPLETED  0x08

/* STRUCTURES *****************************************************************/

typedef struct _GET_NEXT_VDM_COMMAND_DATA
{
    ULONG iTask;
    ULONG dwUnused;
    ULONG dwExitCode;
    ULONG dwCodePage;
    HANDLE hStdIn;
    HANDLE hStdOut;
    HANDLE hStdErr;
    LPSTR lpCmdLine;
    LPSTR lpAppName;
    LPSTR lpPifFile;
    LPSTR lpCurDirectory;
    LPSTR lpEnv;
    ULONG dwEnvLen;
    STARTUPINFOA StartupInfo;
    LPSTR lpDesktop;
    ULONG dwDesktopLen;
    LPSTR lpTitle;
    ULONG dwTitleLen;
    LPVOID lpReserved;
    ULONG dwReservedLen;
    USHORT wCmdLen;
    USHORT wAppLen;
    USHORT wPifLen;
    USHORT wCurDirectoryLen;
    USHORT wVDMState;
    USHORT wCurrentDrive;
    BOOLEAN fComingFromBat;
} GET_NEXT_VDM_COMMAND_DATA, *PGET_NEXT_VDM_COMMAND_DATA;

/* FUNCTION PROTOTYPES ********************************************************/

BOOL
NTAPI
BaseDestroyVDMEnvironment(
    IN PANSI_STRING AnsiEnv,
    IN PUNICODE_STRING UnicodeEnv
);

BOOL
WINAPI
BaseGetVdmConfigInfo(
    IN LPCWSTR CommandLineReserved,
    IN ULONG DosSeqId,
    IN ULONG BinaryType,
    IN PUNICODE_STRING CmdLineString,
    OUT PULONG VdmSize
);

BOOL
NTAPI
BaseCreateVDMEnvironment(
    IN PWCHAR lpEnvironment,
    IN PANSI_STRING AnsiEnv,
    IN PUNICODE_STRING UnicodeEnv
);

BOOL
WINAPI
BaseUpdateVDMEntry(
    IN ULONG UpdateIndex,
    IN OUT PHANDLE WaitHandle,
    IN ULONG IndexInfo,
    IN ULONG BinaryType
);

BOOL
WINAPI
BaseCheckForVDM(
    IN HANDLE ProcessHandle,
    OUT LPDWORD ExitCode
);

NTSTATUS
WINAPI
BaseCheckVDM(
    IN ULONG BinaryType,
    IN PCWCH ApplicationName,
    IN PCWCH CommandLine,
    IN PCWCH CurrentDirectory,
    IN PANSI_STRING AnsiEnvironment,
    IN PBASE_API_MESSAGE ApiMessage,
    IN OUT PULONG iTask,
    IN DWORD CreationFlags,
    IN LPSTARTUPINFOW StartupInfo,
    IN HANDLE hUserToken OPTIONAL
);

/* EOF */
