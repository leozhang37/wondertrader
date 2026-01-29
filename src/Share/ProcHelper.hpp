#pragma once
#ifdef _WIN32
#include <windows.h>
#include <Psapi.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <signal.h>
#include <errno.h>
#endif
#endif

#include "fmtlib.h"

class ProcHelper
{
public:
	static bool isProcAlive(uint32_t pid)
	{
#ifdef _WIN32
		DWORD aProcesses[2048], cbNeeded, cProcesses;
		unsigned int i;
		if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded))
			return false;

		cProcesses = cbNeeded / sizeof(DWORD);
		for(i = 0; i < cProcesses; i++)
		{
			if (aProcesses[i] == pid)
				return true;
		}

		return false;
#elif defined(__APPLE__)
		// macOS: use kill(pid, 0) to check if process exists
		if (kill(pid, 0) == 0)
			return true;
		return (errno == EPERM);
#else
		// Linux: check if /proc/[PID] exists
		const char* path = fmtutil::format("/proc/{}", pid);
		return access(path, 0) == 0;
#endif
	}
};