#include <Windows.h>
#include <stdlib.h>
#include <process.h>
#include <io.h>
#include "host.h"
#include "../debuglog.h"

static int pin_current_process_to_p_cores()
{
    DWORD len = 0;
    BYTE* buf = NULL, * p, * end;
    BYTE max_eff = 0;
    KAFFINITY p_mask = 0;
    WORD group = 0xFFFF;
    int found_any = 0;

    //get required size
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return -1;
        }
    }

    buf = (BYTE*)malloc(len);
    if (!buf) {
        return -1;
    }

    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf);
        return -1;
    }

    //find highest EfficiencyClass among cores
    p = buf;
    end = buf + len;
    while (p < end) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;

        if (info->Relationship == RelationProcessorCore) {
            BYTE eff = info->Processor.EfficiencyClass;
            if (eff > max_eff) {
                max_eff = eff;
            }
        }

        p += info->Size;
    }

    //EfficiencyClass is meaningless on this system, bail out... either pre-Win10 or a non-hybrid CPU
    if (max_eff == 0) {
        free(buf);
        return -1;
    }

    //OR logical processors for all top-tier cores
    p = buf;
    while (p < end) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;

        if (info->Relationship == RelationProcessorCore) {
            if (info->Processor.EfficiencyClass == max_eff) {
                GROUP_AFFINITY gm = info->Processor.GroupMask[0];

                //this simple version expects all chosen cores in one group
                if (!found_any) {
                    group = gm.Group;
                    found_any = 1;
                }
                else if (gm.Group != group) {
                    free(buf);
                    return -2; //multi-group system; need SetThreadGroupAffinity path
                }

                p_mask |= gm.Mask;
            }
        }

        p += info->Size;
    }

    free(buf);

    if (!found_any || p_mask == 0) {
        return -1;
    }

    if (SetProcessAffinityMask(GetCurrentProcess(), p_mask) == 0) {
        return -1;
    }

    debug_log(DEBUG_DETAIL, "[HOST_WIN] Pinned CPUs: ");
    {
        DWORD i;
        int first = 1;
        for (i = 0; i < sizeof(KAFFINITY) * 8; i++) {
            if (p_mask & (((KAFFINITY)1) << i)) {
                if (!first) debug_log(DEBUG_DETAIL, ", ");
                debug_log(DEBUG_DETAIL, "%lu", (unsigned long)i);
                first = 0;
            }
        }
        debug_log(DEBUG_DETAIL, "\r\n");
    }

    return 0;
}

void host_get_local_time(HOST_TIME_t* timedata) {
	SYSTEMTIME system_time;

	GetLocalTime(&system_time);
	timedata->year = system_time.wYear;
	timedata->month = (uint8_t)system_time.wMonth;
	timedata->day = (uint8_t)system_time.wDay;
	timedata->dayofweek = (uint8_t)system_time.wDayOfWeek;
	timedata->hour = (uint8_t)system_time.wHour;
	timedata->minute = (uint8_t)system_time.wMinute;
	timedata->second = (uint8_t)system_time.wSecond;
	timedata->milliseconds = system_time.wMilliseconds;
}

int host_get_terminal_columns(FILE* stream) {
	int fd;
	int width;
	HANDLE handle;
	CONSOLE_SCREEN_BUFFER_INFO info;

	if (stream == NULL) {
		return -1;
	}

	fd = _fileno(stream);
	if ((fd < 0) || !_isatty(fd)) {
		return -1;
	}

	handle = (HANDLE)_get_osfhandle(fd);
	if ((handle == INVALID_HANDLE_VALUE) || (handle == NULL)) {
		return -1;
	}

	if (!GetConsoleScreenBufferInfo(handle, &info)) {
		return -1;
	}

	width = (int)(info.srWindow.Right - info.srWindow.Left + 1);
	if (width <= 0) {
		width = (int)info.dwSize.X;
	}

	return (width > 0) ? width : -1;
}

int host_fseek64(FILE* file, uint64_t offset, int origin) {
	return _fseeki64(file, (__int64)offset, origin);
}

uint64_t host_ftell64(FILE* file) {
	__int64 offset = _ftelli64(file);

	if (offset < 0) {
		return UINT64_MAX;
	}

	return (uint64_t)offset;
}

int host_create_thread(HOST_THREAD_FUNC_t thread_func, void* param) {
	if (_beginthread((void(__cdecl*)(void*))thread_func, 0, param) == (uintptr_t)-1L) {
		return -1;
	}

	return 0;
}

void host_end_thread(void) {
	_endthread();
}

void host_set_thread_priority(int priority) {
	switch (priority) {
	case HOST_THREAD_PRIORITY_LOWEST:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
		break;
	case HOST_THREAD_PRIORITY_LOWER:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
		break;
	case HOST_THREAD_PRIORITY_LOW:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		break;
	case HOST_THREAD_PRIORITY_NORMAL:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
		break;
	case HOST_THREAD_PRIORITY_HIGH:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		break;
	case HOST_THREAD_PRIORITY_HIGHER:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		break;
	case HOST_THREAD_PRIORITY_HIGHEST:
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		break;
	}
}

int host_do_startup_tasks() {
    if (pin_current_process_to_p_cores()) {
        debug_log(DEBUG_DETAIL, "[HOST_WIN] No performance CPU cores found. Host is either pre-Win10 or has a non-hybrid CPU. Not setting affinity.\r\n");
    }
    else {
        debug_log(DEBUG_DETAIL, "[HOST_WIN] Set process CPU affinity to highest-performance cores\r\n");
    }

    //return 0 even if the P-core pin failed, otherwise main thinks we want to abort execution
    return 0;
}

void host_sleep(uint32_t ms) {
	Sleep((DWORD)ms);
}
