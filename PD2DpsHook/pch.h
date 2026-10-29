// pch.h - precompiled header. Only rarely changing system/library headers belong here.
#ifndef PCH_H
#define PCH_H

#include "framework.h"

#include <windowsx.h>
#include <objidl.h>      // required by gdiplus.h when WIN32_LEAN_AND_MEAN is set
#include <gdiplus.h>     // logo loading (BMP/PNG/JPG) and scaling
#include <shellapi.h>    // ShellExecuteW (open log folder)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <atomic>

#endif // PCH_H
