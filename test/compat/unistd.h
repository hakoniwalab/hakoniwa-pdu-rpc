#pragma once

#ifdef _WIN32
#include <direct.h>
#include <cstdlib>
#include <windows.h>

inline int chdir(const char* path)
{
    return _chdir(path);
}

inline char* getcwd(char* buffer, int max_length)
{
    return _getcwd(buffer, max_length);
}

inline int usleep(unsigned int usec)
{
    ::Sleep(static_cast<DWORD>((usec + 999U) / 1000U));
    return 0;
}

#ifndef strdup
#define strdup _strdup
#endif
#endif
