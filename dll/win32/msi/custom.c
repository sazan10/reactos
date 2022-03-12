/*
 * Custom Action processing for the Microsoft Installer (msi.dll)
 *
 * Copyright 2005 Aric Stewart for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"
#include "wine/asm.h"

#define COBJMACROS

#include <stdarg.h>
#include <stdio.h>
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "msidefs.h"
#include "winuser.h"
#include "objbase.h"
#include "oleauto.h"

#include "msipriv.h"
#include "winemsi_s.h"
#include "wine/heap.h"
#include "wine/debug.h"
#include "wine/unicode.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(msi);

#define CUSTOM_ACTION_TYPE_MASK 0x3F

typedef struct tagMSIRUNNINGACTION
{
    struct list entry;
    HANDLE handle;
    BOOL   process;
    LPWSTR name;
} MSIRUNNINGACTION;

typedef UINT (WINAPI *MsiCustomActionEntryPoint)( MSIHANDLE );

static CRITICAL_SECTION msi_custom_action_cs;
static CRITICAL_SECTION_DEBUG msi_custom_action_cs_debug =
{
    0, 0, &msi_custom_action_cs,
    { &msi_custom_action_cs_debug.ProcessLocksList,
      &msi_custom_action_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": msi_custom_action_cs") }
};
static CRITICAL_SECTION msi_custom_action_cs = { &msi_custom_action_cs_debug, -1, 0, 0, 0, 0 };

static struct list msi_pending_custom_actions = LIST_INIT( msi_pending_custom_actions );

void  __RPC_FAR * __RPC_USER MIDL_user_allocate(SIZE_T len)
{
    return heap_alloc(len);
}

void __RPC_USER MIDL_user_free(void __RPC_FAR * ptr)
{
    heap_free(ptr);
}

UINT msi_schedule_action( MSIPACKAGE *package, UINT script, const WCHAR *action )
{
    UINT count;
    WCHAR **newbuf = NULL;

    if (script >= SCRIPT_MAX)
    {
        FIXME("Unknown script requested %u\n", script);
        return ERROR_FUNCTION_FAILED;
    }
    TRACE("Scheduling action %s in script %u\n", debugstr_w(action), script);

    count = package->script_actions_count[script];
    package->script_actions_count[script]++;
    if (count != 0) newbuf = msi_realloc( package->script_actions[script],
                                          package->script_actions_count[script] * sizeof(WCHAR *) );
    else newbuf = msi_alloc( sizeof(WCHAR *) );

    newbuf[count] = strdupW( action );
    package->script_actions[script] = newbuf;
    return ERROR_SUCCESS;
}

UINT msi_register_unique_action( MSIPACKAGE *package, const WCHAR *action )
{
    UINT count;
    WCHAR **newbuf = NULL;

    TRACE("Registering %s as unique action\n", debugstr_w(action));

    count = package->unique_actions_count;
    package->unique_actions_count++;
    if (count != 0) newbuf = msi_realloc( package->unique_actions,
                                          package->unique_actions_count * sizeof(WCHAR *) );
    else newbuf = msi_alloc( sizeof(WCHAR *) );

    newbuf[count] = strdupW( action );
    package->unique_actions = newbuf;
    return ERROR_SUCCESS;
}

BOOL msi_action_is_unique( const MSIPACKAGE *package, const WCHAR *action )
{
    UINT i;

    for (i = 0; i < package->unique_actions_count; i++)
    {
        if (!strcmpW( package->unique_actions[i], action )) return TRUE;
    }
    return FALSE;
}

static BOOL check_execution_scheduling_options(MSIPACKAGE *package, LPCWSTR action, UINT options)
{
    if ((options & msidbCustomActionTypeClientRepeat) ==
            msidbCustomActionTypeClientRepeat)
    {
        if (!(package->InWhatSequence & SEQUENCE_UI &&
            package->InWhatSequence & SEQUENCE_EXEC))
        {
            TRACE("Skipping action due to dbCustomActionTypeClientRepeat option.\n");
            return FALSE;
        }
    }
    else if (options & msidbCustomActionTypeFirstSequence)
    {
        if (package->InWhatSequence & SEQUENCE_UI &&
            package->InWhatSequence & SEQUENCE_EXEC )
        {
            TRACE("Skipping action due to msidbCustomActionTypeFirstSequence option.\n");
            return FALSE;
        }
    }
    else if (options & msidbCustomActionTypeOncePerProcess)
    {
        if (msi_action_is_unique(package, action))
        {
            TRACE("Skipping action due to msidbCustomActionTypeOncePerProcess option.\n");
            return FALSE;
        }
        else
            msi_register_unique_action(package, action);
    }

    return TRUE;
}

/* stores the following properties before the action:
 *
 *    [CustomActionData<=>UserSID<=>ProductCode]Action
 */
static LPWSTR msi_get_deferred_action(LPCWSTR action, LPCWSTR actiondata,
                                      LPCWSTR usersid, LPCWSTR prodcode)
{
    LPWSTR deferred;
    DWORD len;

    static const WCHAR format[] = {
            '[','%','s','<','=','>','%','s','<','=','>','%','s',']','%','s',0
    };

    if (!actiondata)
        return strdupW(action);

    len = lstrlenW(action) + lstrlenW(actiondata) +
          lstrlenW(usersid) + lstrlenW(prodcode) +
          lstrlenW(format) - 7;
    deferred = msi_alloc(len * sizeof(WCHAR));

    sprintfW(deferred, format, actiondata, usersid, prodcode, action);
    return deferred;
}

static void set_deferred_action_props( MSIPACKAGE *package, const WCHAR *deferred_data )
{
    static const WCHAR sep[] = {'<','=','>',0};
    const WCHAR *end, *beg = deferred_data + 1;

    end = strstrW(beg, sep);
    msi_set_property( package->db, szCustomActionData, beg, end - beg );
    beg = end + 3;

    end = strstrW(beg, sep);
    msi_set_property( package->db, szUserSID, beg, end - beg );
    beg = end + 3;

    end = strchrW(beg, ']');
    msi_set_property( package->db, szProductCode, beg, end - beg );
}

WCHAR *msi_create_temp_file( MSIDATABASE *db )
{
    WCHAR *ret;

    if (!db->tempfolder)
    {
        WCHAR tmp[MAX_PATH];
        UINT len = sizeof(tmp)/sizeof(tmp[0]);

        if (msi_get_property( db, szTempFolder, tmp, &len ) ||
            GetFileAttributesW( tmp ) != FILE_ATTRIBUTE_DIRECTORY)
        {
            GetTempPathW( MAX_PATH, tmp );
        }
        if (!(db->tempfolder = strdupW( tmp ))) return NULL;
    }

    if ((ret = msi_alloc( (strlenW( db->tempfolder ) + 20) * sizeof(WCHAR) )))
    {
        if (!GetTempFileNameW( db->tempfolder, szMsi, 0, ret ))
        {
            msi_free( ret );
            return NULL;
        }
    }

    return ret;
}

static MSIBINARY *create_temp_binary( MSIPACKAGE *package, LPCWSTR source, BOOL dll )
{
    static const WCHAR query[] = {
        'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ',
        '`','B','i' ,'n','a','r','y','`',' ','W','H','E','R','E',' ',
        '`','N','a','m','e','`',' ','=',' ','\'','%','s','\'',0};
    MSIRECORD *row;
    MSIBINARY *binary = NULL;
    HANDLE file;
    CHAR buffer[1024];
    WCHAR *tmpfile;
    DWORD sz, write;
    UINT r;

    if (!(tmpfile = msi_create_temp_file( package->db ))) return NULL;

    if (!(row = MSI_QueryGetRecord( package->db, query, source ))) goto error;
    if (!(binary = msi_alloc_zero( sizeof(MSIBINARY) ))) goto error;

    file = CreateFileW( tmpfile, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) goto error;

    do
    {
        sz = sizeof(buffer);
        r = MSI_RecordReadStream( row, 2, buffer, &sz );
        if (r != ERROR_SUCCESS)
        {
            ERR("Failed to get stream\n");
            break;
        }
        WriteFile( file, buffer, sz, &write, NULL );
    } while (sz == sizeof buffer);

    CloseHandle( file );
    if (r != ERROR_SUCCESS) goto error;

    /* keep a reference to prevent the dll from being unloaded */
    if (dll && !(binary->module = LoadLibraryW( tmpfile )))
    {
        ERR( "failed to load dll %s (%u)\n", debugstr_w( tmpfile ), GetLastError() );
    }
    binary->source = strdupW( source );
    binary->tmpfile = tmpfile;
    list_add_tail( &package->binaries, &binary->entry );

    msiobj_release( &row->hdr );
    return binary;

error:
    if (row) msiobj_release( &row->hdr );
    DeleteFileW( tmpfile );
    msi_free( tmpfile );
    msi_free( binary );
    return NULL;
}

static MSIBINARY *get_temp_binary( MSIPACKAGE *package, LPCWSTR source, BOOL dll )
{
    MSIBINARY *binary;

    LIST_FOR_EACH_ENTRY( binary, &package->binaries, MSIBINARY, entry )
    {
        if (!strcmpW( binary->source, source ))
            return binary;
    }

    return create_temp_binary( package, source, dll );
}

static void file_running_action(MSIPACKAGE* package, HANDLE Handle,
                                BOOL process, LPCWSTR name)
{
    MSIRUNNINGACTION *action;

    action = msi_alloc( sizeof(MSIRUNNINGACTION) );

    action->handle = Handle;
    action->process = process;
    action->name = strdupW(name);

    list_add_tail( &package->RunningActions, &action->entry );
}

static UINT custom_get_process_return( HANDLE process )
{
    DWORD rc = 0;

    GetExitCodeProcess( process, &rc );
    TRACE("exit code is %u\n", rc);
    if (rc != 0)
        return ERROR_FUNCTION_FAILED;
    return ERROR_SUCCESS;
}

static UINT custom_get_thread_return( MSIPACKAGE *package, HANDLE thread )
{
    DWORD rc = 0;

    GetExitCodeThread( thread, &rc );

    switch (rc)
    {
    case ERROR_FUNCTION_NOT_CALLED:
    case ERROR_SUCCESS:
    case ERROR_INSTALL_USEREXIT:
    case ERROR_INSTALL_FAILURE:
        return rc;
    case ERROR_NO_MORE_ITEMS:
        return ERROR_SUCCESS;
    case ERROR_INSTALL_SUSPEND:
        ACTION_ForceReboot( package );
        return ERROR_SUCCESS;
    default:
        ERR("Invalid Return Code %d\n",rc);
        return ERROR_INSTALL_FAILURE;
    }
}

static UINT wait_process_handle(MSIPACKAGE* package, UINT type,
                           HANDLE ProcessHandle, LPCWSTR name)
{
    UINT rc = ERROR_SUCCESS;

    if (!(type & msidbCustomActionTypeAsync))
    {
        TRACE("waiting for %s\n", debugstr_w(name));

        msi_dialog_check_messages(ProcessHandle);

        if (!(type & msidbCustomActionTypeContinue))
            rc = custom_get_process_return(ProcessHandle);

        CloseHandle(ProcessHandle);
    }
    else
    {
        TRACE("%s running in background\n", debugstr_w(name));

        if (!(type & msidbCustomActionTypeContinue))
            file_running_action(package, ProcessHandle, TRUE, name);
        else
            CloseHandle(ProcessHandle);
    }

    return rc;
}

typedef struct _msi_custom_action_info {
    struct list entry;
    LONG refs;
    MSIPACKAGE *package;
    LPWSTR source;
    LPWSTR target;
    HANDLE handle;
    LPWSTR action;
    INT type;
    GUID guid;
} msi_custom_action_info;

static void release_custom_action_data( msi_custom_action_info *info )
{
    EnterCriticalSection( &msi_custom_action_cs );

    if (!--info->refs)
    {
        list_remove( &info->entry );
        if (info->handle)
            CloseHandle( info->handle );
        msi_free( info->action );
        msi_free( info->source );
        msi_free( info->target );
        msiobj_release( &info->package->hdr );
        msi_free( info );
    }

    LeaveCriticalSection( &msi_custom_action_cs );
}

/* must be called inside msi_custom_action_cs if info is in the pending custom actions list */
static void addref_custom_action_data( msi_custom_action_info *info )
{
    info->refs++;
 }

static UINT wait_thread_handle( msi_custom_action_info *info )
{
    UINT rc = ERROR_SUCCESS;

    if (!(info->type & msidbCustomActionTypeAsync))
    {
        TRACE("waiting for %s\n", debugstr_w( info->action ));

        msi_dialog_check_messages( info->handle );

        if (!(info->type & msidbCustomActionTypeContinue))
            rc = custom_get_thread_return( info->package, info->handle );

        release_custom_action_data( info );
    }
    else
    {
        TRACE("%s running in background\n", debugstr_w( info->action ));
    }

    return rc;
}

static msi_custom_action_info *find_action_by_guid( const GUID *guid )
{
    msi_custom_action_info *info;
    BOOL found = FALSE;

    EnterCriticalSection( &msi_custom_action_cs );

    LIST_FOR_EACH_ENTRY( info, &msi_pending_custom_actions, msi_custom_action_info, entry )
    {
        if (IsEqualGUID( &info->guid, guid ))
        {
            addref_custom_action_data( info );
            found = TRUE;
            break;
        }
    }

    LeaveCriticalSection( &msi_custom_action_cs );

    if (!found)
        return NULL;

    return info;
}

static void handle_msi_break(LPCSTR target)
{
    char format[] = "To debug your custom action, attach your debugger to "
                    "process %i (0x%X) and press OK";
    char val[MAX_PATH];
    char msg[100];

    if (!GetEnvironmentVariableA("MsiBreak", val, MAX_PATH))
        return;

    if (strcasecmp(val, target))
        return;

    sprintf(msg, format, GetCurrentProcessId(), GetCurrentProcessId());
    MessageBoxA(NULL, msg, "Windows Installer", MB_OK);
    DebugBreak();
}

static WCHAR ncalrpcW[] = {'n','c','a','l','r','p','c',0};
static WCHAR endpoint_lrpcW[] = {'m','s','i',0};

UINT __wine_msi_call_dll_function(const GUID *guid)
{
    MsiCustomActionEntryPoint fn;
    MSIHANDLE remote_package = 0;
    RPC_WSTR binding_str;
    MSIHANDLE hPackage;
    RPC_STATUS status;
    LPWSTR dll = NULL;
    LPSTR proc = NULL;
    HANDLE hModule;
    HANDLE thread;
    INT type;
    UINT r;

    TRACE("%s\n", debugstr_guid( guid ));

    status = RpcStringBindingComposeW(NULL, ncalrpcW, NULL, endpoint_lrpcW, NULL, &binding_str);
    if (status != RPC_S_OK)
    {
        ERR("RpcStringBindingCompose failed: %#x\n", status);
        return status;
    }
    status = RpcBindingFromStringBindingW(binding_str, &rpc_handle);
    if (status != RPC_S_OK)
    {
        ERR("RpcBindingFromStringBinding failed: %#x\n", status);
        return status;
    }
    RpcStringFreeW(&binding_str);

    /* We need this to unmarshal streams, and some apps expect it to be present. */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    r = remote_GetActionInfo(guid, &type, &dll, &proc, &remote_package);
    if (r != ERROR_SUCCESS)
        return r;

    hModule = LoadLibraryW( dll );
    if (!hModule)
    {
        ERR( "failed to load dll %s (%u)\n", debugstr_w( dll ), GetLastError() );
        return ERROR_SUCCESS;
    }

    fn = (MsiCustomActionEntryPoint) GetProcAddress( hModule, proc );
    if (fn)
    {
        hPackage = alloc_msi_remote_handle( remote_package );
        if (hPackage)
        {
            TRACE("calling %s\n", debugstr_a(proc));
            handle_msi_break(proc);

            __TRY
            {
                thread = CreateThread(NULL, 0, (void *)fn, (void *)(ULONG_PTR) hPackage, 0, NULL);
                WaitForSingleObject(thread, INFINITE);
                GetExitCodeThread(thread, &r);
            }
            __EXCEPT_PAGE_FAULT
            {
                ERR("Custom action (%s:%s) caused a page fault: %08x\n",
                    debugstr_w(dll), debugstr_a(proc), GetExceptionCode());
                r = ERROR_SUCCESS;
            }
            __ENDTRY;

            MsiCloseHandle( hPackage );
        }
        else
            ERR("failed to create handle for %x\n", remote_package );
    }
    else
        ERR("GetProcAddress(%s) failed\n", debugstr_a(proc));

    FreeLibrary(hModule);

    MsiCloseHandle(hPackage);
    midl_user_free(dll);
    midl_user_free(proc);

    CoUninitialize();

    RpcBindingFree(&rpc_handle);

    return r;
}

static DWORD WINAPI DllThread( LPVOID arg )
{
    static const WCHAR msiexecW[] = {'\\','m','s','i','e','x','e','c','.','e','x','e',0};
    static const WCHAR argsW[] = {' ','-','E','m','b','e','d','d','i','n','g',' ',0};
    msi_custom_action_info *info;
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW si = {0};
    WCHAR buffer[MAX_PATH], cmdline[MAX_PATH + 60];
    RPC_STATUS status;
    GUID *guid = arg;
    void *cookie;
    BOOL wow64;
    DWORD arch;
    DWORD rc;

    TRACE("custom action (%x) started\n", GetCurrentThreadId() );

    CoInitializeEx(NULL, COINIT_MULTITHREADED); /* needed to marshal streams */

    status = RpcServerUseProtseqEpW(ncalrpcW, RPC_C_PROTSEQ_MAX_REQS_DEFAULT, endpoint_lrpcW, NULL);
    if (status != RPC_S_OK)
    {
        ERR("RpcServerUseProtseqEp failed: %#x\n", status);
        return status;
    }

    status = RpcServerRegisterIfEx((RPC_IF_HANDLE)s_IWineMsiRemote_v0_0_s_ifspec, NULL, NULL,
        RPC_IF_AUTOLISTEN, RPC_C_LISTEN_MAX_CALLS_DEFAULT, NULL);
    if (status != RPC_S_OK)
    {
        ERR("RpcServerRegisterIfEx failed: %#x\n", status);
        return status;
    }

    info = find_action_by_guid(guid);
    GetBinaryTypeW(info->source, &arch);

    if (sizeof(void *) == 8 && arch == SCS_32BIT_BINARY)
        GetSystemWow64DirectoryW(buffer, MAX_PATH - sizeof(msiexecW)/sizeof(WCHAR));
    else
        GetSystemDirectoryW(buffer, MAX_PATH - sizeof(msiexecW)/sizeof(WCHAR));
    strcatW(buffer, msiexecW);
    strcpyW(cmdline, buffer);
    strcatW(cmdline, argsW);
    StringFromGUID2(guid, cmdline + strlenW(cmdline), 39);

    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64 && arch == SCS_64BIT_BINARY)
    {
        Wow64DisableWow64FsRedirection(&cookie);
        CreateProcessW(buffer, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        Wow64RevertWow64FsRedirection(cookie);
    }
    else
        CreateProcessW(buffer, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    status = RpcServerUnregisterIf((RPC_IF_HANDLE)s_IWineMsiRemote_v0_0_s_ifspec, NULL, FALSE);
    if (status != RPC_S_OK)
    {
        ERR("RpcServerUnregisterIf failed: %#x\n", status);
        return status;
    }

    CoUninitialize();

    TRACE("custom action (%x) returned %i\n", GetCurrentThreadId(), rc );

    MsiCloseAllHandles();
    return rc;
}

static msi_custom_action_info *do_msidbCustomActionTypeDll(
    MSIPACKAGE *package, INT type, LPCWSTR source, LPCWSTR target, LPCWSTR action )
{
    msi_custom_action_info *info;

    info = msi_alloc( sizeof *info );
    if (!info)
        return NULL;

    msiobj_addref( &package->hdr );
    info->refs = 2; /* 1 for our caller and 1 for thread we created */
    info->package = package;
    info->type = type;
    info->target = strdupW( target );
    info->source = strdupW( source );
    info->action = strdupW( action );
    CoCreateGuid( &info->guid );

    EnterCriticalSection( &msi_custom_action_cs );
    list_add_tail( &msi_pending_custom_actions, &info->entry );
    LeaveCriticalSection( &msi_custom_action_cs );

    info->handle = CreateThread( NULL, 0, DllThread, &info->guid, 0, NULL );
    if (!info->handle)
    {
        /* release both references */
        release_custom_action_data( info );
        release_custom_action_data( info );
        return NULL;
    }

    return info;
}

static UINT HANDLE_CustomType1( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                INT type, const WCHAR *action )
{
    msi_custom_action_info *info;
    MSIBINARY *binary;

    if (!(binary = get_temp_binary( package, source, TRUE )))
        return ERROR_FUNCTION_FAILED;

    TRACE("Calling function %s from %s\n", debugstr_w(target), debugstr_w(binary->tmpfile));

    info = do_msidbCustomActionTypeDll( package, type, binary->tmpfile, target, action );
    return wait_thread_handle( info );
}

static HANDLE execute_command( const WCHAR *app, WCHAR *arg, const WCHAR *dir )
{
    static const WCHAR dotexeW[] = {'.','e','x','e',0};
    STARTUPINFOW si;
    PROCESS_INFORMATION info;
    WCHAR *exe = NULL, *cmd = NULL, *p;
    BOOL ret;

    if (app)
    {
        int len_arg = 0;
        DWORD len_exe;

        if (!(exe = msi_alloc( MAX_PATH * sizeof(WCHAR) ))) return INVALID_HANDLE_VALUE;
        len_exe = SearchPathW( NULL, app, dotexeW, MAX_PATH, exe, NULL );
        if (len_exe >= MAX_PATH)
        {
            msi_free( exe );
            if (!(exe = msi_alloc( len_exe * sizeof(WCHAR) ))) return INVALID_HANDLE_VALUE;
            len_exe = SearchPathW( NULL, app, dotexeW, len_exe, exe, NULL );
        }
        if (!len_exe)
        {
            ERR("can't find executable %u\n", GetLastError());
            msi_free( exe );
            return INVALID_HANDLE_VALUE;
        }

        if (arg) len_arg = strlenW( arg );
        if (!(cmd = msi_alloc( (len_exe + len_arg + 4) * sizeof(WCHAR) )))
        {
            msi_free( exe );
            return INVALID_HANDLE_VALUE;
        }
        p = cmd;
        if (strchrW( exe, ' ' ))
        {
            *p++ = '\"';
            memcpy( p, exe, len_exe * sizeof(WCHAR) );
            p += len_exe;
            *p++ = '\"';
            *p = 0;
        }
        else
        {
            strcpyW( p, exe );
            p += len_exe;
        }
        if (arg)
        {
            *p++ = ' ';
            memcpy( p, arg, len_arg * sizeof(WCHAR) );
            p[len_arg] = 0;
        }
    }
    memset( &si, 0, sizeof(STARTUPINFOW) );
    ret = CreateProcessW( exe, exe ? cmd : arg, NULL, NULL, FALSE, 0, NULL, dir, &si, &info );
    msi_free( cmd );
    msi_free( exe );
    if (!ret)
    {
        ERR("unable to execute command %u\n", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    CloseHandle( info.hThread );
    return info.hProcess;
}

static UINT HANDLE_CustomType2( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                INT type, const WCHAR *action )
{
    MSIBINARY *binary;
    HANDLE handle;
    WCHAR *arg;

    if (!(binary = get_temp_binary( package, source, FALSE ))) return ERROR_FUNCTION_FAILED;

    deformat_string( package, target, &arg );
    TRACE("exe %s arg %s\n", debugstr_w(binary->tmpfile), debugstr_w(arg));

    handle = execute_command( binary->tmpfile, arg, szCRoot );
    msi_free( arg );
    if (handle == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    return wait_process_handle( package, type, handle, action );
}

static UINT HANDLE_CustomType17( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                 INT type, const WCHAR *action )
{
    msi_custom_action_info *info;
    MSIFILE *file;

    TRACE("%s %s\n", debugstr_w(source), debugstr_w(target));

    file = msi_get_loaded_file( package, source );
    if (!file)
    {
        ERR("invalid file key %s\n", debugstr_w( source ));
        return ERROR_FUNCTION_FAILED;
    }

    info = do_msidbCustomActionTypeDll( package, type, file->TargetPath, target, action );
    return wait_thread_handle( info );
}

static UINT HANDLE_CustomType18( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                 INT type, const WCHAR *action )
{
    MSIFILE *file;
    HANDLE handle;
    WCHAR *arg;

    if (!(file = msi_get_loaded_file( package, source ))) return ERROR_FUNCTION_FAILED;

    deformat_string( package, target, &arg );
    TRACE("exe %s arg %s\n", debugstr_w(file->TargetPath), debugstr_w(arg));

    handle = execute_command( file->TargetPath, arg, szCRoot );
    msi_free( arg );
    if (handle == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    return wait_process_handle( package, type, handle, action );
}

static UINT HANDLE_CustomType19( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                 INT type, const WCHAR *action )
{
    static const WCHAR query[] = {
      'S','E','L','E','C','T',' ','`','M','e','s','s','a','g','e','`',' ',
      'F','R','O','M',' ','`','E','r','r','o','r','`',' ',
      'W','H','E','R','E',' ','`','E','r','r','o','r','`',' ','=',' ',
      '%','s',0
    };
    MSIRECORD *row = 0;
    LPWSTR deformated = NULL;

    deformat_string( package, target, &deformated );

    /* first try treat the error as a number */
    row = MSI_QueryGetRecord( package->db, query, deformated );
    if( row )
    {
        LPCWSTR error = MSI_RecordGetString( row, 1 );
        if ((package->ui_level & INSTALLUILEVEL_MASK) != INSTALLUILEVEL_NONE)
            MessageBoxW( NULL, error, NULL, MB_OK );
        msiobj_release( &row->hdr );
    }
    else if ((package->ui_level & INSTALLUILEVEL_MASK) != INSTALLUILEVEL_NONE)
        MessageBoxW( NULL, deformated, NULL, MB_OK );

    msi_free( deformated );

    return ERROR_INSTALL_FAILURE;
}

static UINT HANDLE_CustomType23( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                 INT type, const WCHAR *action )
{
    static const WCHAR msiexecW[] = {'m','s','i','e','x','e','c',0};
    static const WCHAR paramsW[] = {'/','q','b',' ','/','i',' '};
    WCHAR *dir, *arg, *p;
    UINT len_src, len_dir, len_tgt, len = sizeof(paramsW)/sizeof(paramsW[0]);
    HANDLE handle;

    if (!(dir = msi_dup_property( package->db, szOriginalDatabase ))) return ERROR_OUTOFMEMORY;
    if (!(p = strrchrW( dir, '\\' )) && !(p = strrchrW( dir, '/' )))
    {
        msi_free( dir );
        return ERROR_FUNCTION_FAILED;
    }
    *p = 0;
    len_dir = p - dir;
    len_src = strlenW( source );
    len_tgt = strlenW( target );
    if (!(arg = msi_alloc( (len + len_dir + len_src + len_tgt + 5) * sizeof(WCHAR) )))
    {
        msi_free( dir );
        return ERROR_OUTOFMEMORY;
    }
    memcpy( arg, paramsW, sizeof(paramsW) );
    arg[len++] = '"';
    memcpy( arg + len, dir, len_dir * sizeof(WCHAR) );
    len += len_dir;
    arg[len++] = '\\';
    memcpy( arg + len, source, len_src * sizeof(WCHAR) );
    len += len_src;
    arg[len++] = '"';
    arg[len++] = ' ';
    strcpyW( arg + len, target );

    TRACE("installing %s concurrently\n", debugstr_w(source));

    handle = execute_command( msiexecW, arg, dir );
    msi_free( dir );
    msi_free( arg );
    if (handle == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    return wait_process_handle( package, type, handle, action );
}

static UINT HANDLE_CustomType50( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                 INT type, const WCHAR *action )
{
    WCHAR *exe, *arg;
    HANDLE handle;

    if (!(exe = msi_dup_property( package->db, source ))) return ERROR_SUCCESS;

    deformat_string( package, target, &arg );
    TRACE("exe %s arg %s\n", debugstr_w(exe), debugstr_w(arg));

    handle = execute_command( exe, arg, szCRoot );
    msi_free( exe );
    msi_free( arg );
    if (handle == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    return wait_process_handle( package, type, handle, action );
}

static UINT HANDLE_CustomType34( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                 INT type, const WCHAR *action )
{
    const WCHAR *workingdir = NULL;
    HANDLE handle;
    WCHAR *cmd;

    if (source)
    {
        workingdir = msi_get_target_folder( package, source );
        if (!workingdir) return ERROR_FUNCTION_FAILED;
    }
    deformat_string( package, target, &cmd );
    if (!cmd) return ERROR_FUNCTION_FAILED;

    TRACE("cmd %s dir %s\n", debugstr_w(cmd), debugstr_w(workingdir));

    handle = execute_command( NULL, cmd, workingdir );
    msi_free( cmd );
    if (handle == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    return wait_process_handle( package, type, handle, action );
}

static DWORD ACTION_CallScript( const GUID *guid )
{
    msi_custom_action_info *info;
    MSIHANDLE hPackage;
    UINT r = ERROR_FUNCTION_FAILED;

    info = find_action_by_guid( guid );
    if (!info)
    {
        ERR("failed to find action %s\n", debugstr_guid( guid) );
        return ERROR_FUNCTION_FAILED;
    }

    TRACE("function %s, script %s\n", debugstr_w( info->target ), debugstr_w( info->source ) );

    hPackage = alloc_msihandle( &info->package->hdr );
    if (hPackage)
    {
        r = call_script( hPackage, info->type, info->source, info->target, info->action );
        TRACE("script returned %u\n", r);
        MsiCloseHandle( hPackage );
    }
    else
        ERR("failed to create handle for %p\n", info->package );

    release_custom_action_data( info );
    return r;
}

static DWORD WINAPI ScriptThread( LPVOID arg )
{
    LPGUID guid = arg;
    DWORD rc;

    TRACE("custom action (%x) started\n", GetCurrentThreadId() );

    rc = ACTION_CallScript( guid );

    TRACE("custom action (%x) returned %i\n", GetCurrentThreadId(), rc );

    MsiCloseAllHandles();
    return rc;
}

static msi_custom_action_info *do_msidbCustomActionTypeScript(
    MSIPACKAGE *package, INT type, LPCWSTR script, LPCWSTR function, LPCWSTR action )
{
    msi_custom_action_info *info;

    info = msi_alloc( sizeof *info );
    if (!info)
        return NULL;

    msiobj_addref( &package->hdr );
    info->refs = 2; /* 1 for our caller and 1 for thread we created */
    info->package = package;
    info->type = type;
    info->target = strdupW( function );
    info->source = strdupW( script );
    info->action = strdupW( action );
    CoCreateGuid( &info->guid );

    EnterCriticalSection( &msi_custom_action_cs );
    list_add_tail( &msi_pending_custom_actions, &info->entry );
    LeaveCriticalSection( &msi_custom_action_cs );

    info->handle = CreateThread( NULL, 0, ScriptThread, &info->guid, 0, NULL );
    if (!info->handle)
    {
        /* release both references */
        release_custom_action_data( info );
        release_custom_action_data( info );
        return NULL;
    }

    return info;
}

static UINT HANDLE_CustomType37_38( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                    INT type, const WCHAR *action )
{
    msi_custom_action_info *info;

    TRACE("%s %s\n", debugstr_w(source), debugstr_w(target));

    info = do_msidbCustomActionTypeScript( package, type, target, NULL, action );
    return wait_thread_handle( info );
}

static UINT HANDLE_CustomType5_6( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                  INT type, const WCHAR *action )
{
    static const WCHAR query[] = {
        'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ',
        '`','B','i' ,'n','a','r','y','`',' ','W','H','E','R','E',' ',
        '`','N','a','m','e','`',' ','=',' ','\'','%','s','\'',0};
    MSIRECORD *row = NULL;
    msi_custom_action_info *info;
    CHAR *buffer = NULL;
    WCHAR *bufferw = NULL;
    DWORD sz = 0;
    UINT r;

    TRACE("%s %s\n", debugstr_w(source), debugstr_w(target));

    row = MSI_QueryGetRecord(package->db, query, source);
    if (!row)
        return ERROR_FUNCTION_FAILED;

    r = MSI_RecordReadStream(row, 2, NULL, &sz);
    if (r != ERROR_SUCCESS) goto done;

    buffer = msi_alloc( sz + 1 );
    if (!buffer)
    {
       r = ERROR_FUNCTION_FAILED;
       goto done;
    }

    r = MSI_RecordReadStream(row, 2, buffer, &sz);
    if (r != ERROR_SUCCESS)
        goto done;

    buffer[sz] = 0;
    bufferw = strdupAtoW(buffer);
    if (!bufferw)
    {
        r = ERROR_FUNCTION_FAILED;
        goto done;
    }

    info = do_msidbCustomActionTypeScript( package, type, bufferw, target, action );
    r = wait_thread_handle( info );

done:
    msi_free(bufferw);
    msi_free(buffer);
    msiobj_release(&row->hdr);
    return r;
}

static UINT HANDLE_CustomType21_22( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                    INT type, const WCHAR *action )
{
    msi_custom_action_info *info;
    MSIFILE *file;
    HANDLE hFile;
    DWORD sz, szHighWord = 0, read;
    CHAR *buffer=NULL;
    WCHAR *bufferw=NULL;
    BOOL bRet;
    UINT r;

    TRACE("%s %s\n", debugstr_w(source), debugstr_w(target));

    file = msi_get_loaded_file(package, source);
    if (!file)
    {
        ERR("invalid file key %s\n", debugstr_w(source));
        return ERROR_FUNCTION_FAILED;
    }

    hFile = CreateFileW(file->TargetPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return ERROR_FUNCTION_FAILED;

    sz = GetFileSize(hFile, &szHighWord);
    if (sz == INVALID_FILE_SIZE || szHighWord != 0)
    {
        CloseHandle(hFile);
        return ERROR_FUNCTION_FAILED;
    }
    buffer = msi_alloc( sz + 1 );
    if (!buffer)
    {
        CloseHandle(hFile);
        return ERROR_FUNCTION_FAILED;
    }
    bRet = ReadFile(hFile, buffer, sz, &read, NULL);
    CloseHandle(hFile);
    if (!bRet)
    {
        r = ERROR_FUNCTION_FAILED;
        goto done;
    }
    buffer[read] = 0;
    bufferw = strdupAtoW(buffer);
    if (!bufferw)
    {
        r = ERROR_FUNCTION_FAILED;
        goto done;
    }
    info = do_msidbCustomActionTypeScript( package, type, bufferw, target, action );
    r = wait_thread_handle( info );

done:
    msi_free(bufferw);
    msi_free(buffer);
    return r;
}

static UINT HANDLE_CustomType53_54( MSIPACKAGE *package, const WCHAR *source, const WCHAR *target,
                                    INT type, const WCHAR *action )
{
    msi_custom_action_info *info;
    WCHAR *prop;

    TRACE("%s %s\n", debugstr_w(source), debugstr_w(target));

    prop = msi_dup_property( package->db, source );
    if (!prop) return ERROR_SUCCESS;

    info = do_msidbCustomActionTypeScript( package, type, prop, NULL, action );
    msi_free(prop);
    return wait_thread_handle( info );
}

static BOOL action_type_matches_script( UINT type, UINT script )
{
    switch (script)
    {
    case SCRIPT_NONE:
    case SCRIPT_INSTALL:
        return !(type & msidbCustomActionTypeCommit) && !(type & msidbCustomActionTypeRollback);
    case SCRIPT_COMMIT:
        return (type & msidbCustomActionTypeCommit);
    case SCRIPT_ROLLBACK:
        return (type & msidbCustomActionTypeRollback);
    default:
        ERR("unhandled script %u\n", script);
    }
    return FALSE;
}

static UINT defer_custom_action( MSIPACKAGE *package, const WCHAR *action, UINT type )
{
    WCHAR *actiondata = msi_dup_property( package->db, action );
    WCHAR *usersid = msi_dup_property( package->db, szUserSID );
    WCHAR *prodcode = msi_dup_property( package->db, szProductCode );
    WCHAR *deferred = msi_get_deferred_action( action, actiondata, usersid, prodcode );

    if (!deferred)
    {
        msi_free( actiondata );
        msi_free( usersid );
        msi_free( prodcode );
        return ERROR_OUTOFMEMORY;
    }
    if (type & msidbCustomActionTypeCommit)
    {
        TRACE("deferring commit action\n");
        msi_schedule_action( package, SCRIPT_COMMIT, deferred );
    }
    else if (type & msidbCustomActionTypeRollback)
    {
        TRACE("deferring rollback action\n");
        msi_schedule_action( package, SCRIPT_ROLLBACK, deferred );
    }
    else
    {
        TRACE("deferring install action\n");
        msi_schedule_action( package, SCRIPT_INSTALL, deferred );
    }

    msi_free( actiondata );
    msi_free( usersid );
    msi_free( prodcode );
    msi_free( deferred );
    return ERROR_SUCCESS;
}

UINT ACTION_CustomAction( MSIPACKAGE *package, LPCWSTR action, UINT script )
{
    static const WCHAR query[] = {
        'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ',
        '`','C','u','s','t','o','m','A','c','t','i','o','n','`',' ','W','H','E','R','E',' ',
        '`','A','c','t','i' ,'o','n','`',' ','=',' ','\'','%','s','\'',0};
    UINT rc = ERROR_SUCCESS;
    MSIRECORD *row;
    UINT type;
    const WCHAR *source, *target, *ptr, *deferred_data = NULL;
    WCHAR *deformated = NULL;
    int len;

    /* deferred action: [properties]Action */
    if ((ptr = strrchrW(action, ']')))
    {
        deferred_data = action;
        action = ptr + 1;
    }

    row = MSI_QueryGetRecord( package->db, query, action );
    if (!row)
        return ERROR_FUNCTION_NOT_CALLED;

    type = MSI_RecordGetInteger(row,2);
    source = MSI_RecordGetString(row,3);
    target = MSI_RecordGetString(row,4);

    TRACE("Handling custom action %s (%x %s %s)\n",debugstr_w(action),type,
          debugstr_w(source), debugstr_w(target));

    /* handle some of the deferred actions */
    if (type & msidbCustomActionTypeTSAware)
        FIXME("msidbCustomActionTypeTSAware not handled\n");

    if (type & msidbCustomActionTypeInScript)
    {
        if (type & msidbCustomActionTypeNoImpersonate)
            WARN("msidbCustomActionTypeNoImpersonate not handled\n");

        if (!action_type_matches_script( type, script ))
        {
            rc = defer_custom_action( package, action, type );
            goto end;
        }
        else
        {
            LPWSTR actiondata = msi_dup_property( package->db, action );

            if (type & msidbCustomActionTypeInScript)
                package->scheduled_action_running = TRUE;

            if (type & msidbCustomActionTypeCommit)
                package->commit_action_running = TRUE;

            if (type & msidbCustomActionTypeRollback)
                package->rollback_action_running = TRUE;

            if (deferred_data)
                set_deferred_action_props(package, deferred_data);
            else if (actiondata)
                msi_set_property( package->db, szCustomActionData, actiondata, -1 );
            else
                msi_set_property( package->db, szCustomActionData, szEmpty, -1 );

            msi_free(actiondata);
        }
    }
    else if (!check_execution_scheduling_options(package,action,type))
    {
        rc = ERROR_SUCCESS;
        goto end;
    }

    switch (type & CUSTOM_ACTION_TYPE_MASK)
    {
        case 1: /* DLL file stored in a Binary table stream */
            rc = HANDLE_CustomType1(package,source,target,type,action);
            break;
        case 2: /* EXE file stored in a Binary table stream */
            rc = HANDLE_CustomType2(package,source,target,type,action);
            break;
        case 18: /*EXE file installed with package */
            rc = HANDLE_CustomType18(package,source,target,type,action);
            break;
        case 19: /* Error that halts install */
            rc = HANDLE_CustomType19(package,source,target,type,action);
            break;
        case 17:
            rc = HANDLE_CustomType17(package,source,target,type,action);
            break;
        case 23: /* installs another package in the source tree */
            deformat_string(package,target,&deformated);
            rc = HANDLE_CustomType23(package,source,deformated,type,action);
            msi_free(deformated);
            break;
        case 50: /*EXE file specified by a property value */
            rc = HANDLE_CustomType50(package,source,target,type,action);
            break;
        case 34: /*EXE to be run in specified directory */
            rc = HANDLE_CustomType34(package,source,target,type,action);
            break;
        case 35: /* Directory set with formatted text. */
            deformat_string(package,target,&deformated);
            MSI_SetTargetPathW(package, source, deformated);
            msi_free(deformated);
            break;
        case 51: /* Property set with formatted text. */
            if (!source)
                break;

            len = deformat_string( package, target, &deformated );
            rc = msi_set_property( package->db, source, deformated, len );
            if (rc == ERROR_SUCCESS && !strcmpW( source, szSourceDir ))
                msi_reset_folders( package, TRUE );
            msi_free(deformated);
            break;
    case 37: /* JScript/VBScript text stored in target column. */
    case 38:
        rc = HANDLE_CustomType37_38(package,source,target,type,action);
        break;
    case 5:
    case 6: /* JScript/VBScript file stored in a Binary table stream. */
        rc = HANDLE_CustomType5_6(package,source,target,type,action);
        break;
    case 21: /* JScript/VBScript file installed with the product. */
    case 22:
        rc = HANDLE_CustomType21_22(package,source,target,type,action);
        break;
    case 53: /* JScript/VBScript text specified by a property value. */
    case 54:
        rc = HANDLE_CustomType53_54(package,source,target,type,action);
        break;
    default:
        FIXME("unhandled action type %u (%s %s)\n", type & CUSTOM_ACTION_TYPE_MASK,
              debugstr_w(source), debugstr_w(target));
    }

end:
    package->scheduled_action_running = FALSE;
    package->commit_action_running = FALSE;
    package->rollback_action_running = FALSE;
    msiobj_release(&row->hdr);
    return rc;
}

void ACTION_FinishCustomActions(const MSIPACKAGE* package)
{
    struct list *item;
    HANDLE *wait_handles;
    unsigned int handle_count, i;
    msi_custom_action_info *info, *cursor;

    while ((item = list_head( &package->RunningActions )))
    {
        MSIRUNNINGACTION *action = LIST_ENTRY( item, MSIRUNNINGACTION, entry );

        list_remove( &action->entry );

        TRACE("waiting for %s\n", debugstr_w( action->name ) );
        msi_dialog_check_messages( action->handle );

        CloseHandle( action->handle );
        msi_free( action->name );
        msi_free( action );
    }

    EnterCriticalSection( &msi_custom_action_cs );

    handle_count = list_count( &msi_pending_custom_actions );
    wait_handles = msi_alloc( handle_count * sizeof(HANDLE) );

    handle_count = 0;
    LIST_FOR_EACH_ENTRY_SAFE( info, cursor, &msi_pending_custom_actions, msi_custom_action_info, entry )
    {
        if (info->package == package )
        {
            if (DuplicateHandle(GetCurrentProcess(), info->handle, GetCurrentProcess(), &wait_handles[handle_count], SYNCHRONIZE, FALSE, 0))
                handle_count++;
        }
    }

    LeaveCriticalSection( &msi_custom_action_cs );

    for (i = 0; i < handle_count; i++)
    {
        msi_dialog_check_messages( wait_handles[i] );
        CloseHandle( wait_handles[i] );
    }
    msi_free( wait_handles );

    EnterCriticalSection( &msi_custom_action_cs );
    LIST_FOR_EACH_ENTRY_SAFE( info, cursor, &msi_pending_custom_actions, msi_custom_action_info, entry )
    {
        if (info->package == package) release_custom_action_data( info );
    }
    LeaveCriticalSection( &msi_custom_action_cs );
}

UINT __cdecl s_remote_GetActionInfo(const GUID *guid, int *type, LPWSTR *dll, LPSTR *func, MSIHANDLE *hinst)
{
    msi_custom_action_info *info;

    info = find_action_by_guid(guid);
    if (!info)
        return ERROR_INVALID_DATA;

    *type = info->type;
    *hinst = alloc_msihandle(&info->package->hdr);
    *dll = strdupW(info->source);
    *func = strdupWtoA(info->target);

    release_custom_action_data(info);
    return ERROR_SUCCESS;
}
