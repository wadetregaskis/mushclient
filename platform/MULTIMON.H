// platform/MULTIMON.H — multimonitor API stub for non-Windows builds
#pragma once
#ifndef _WIN32
// MULTIMON.H provides multi-monitor stubs when _WIN32_WINNT < 0x0500.
// Not needed on non-Windows; this file exists only to satisfy the #include.
#endif
