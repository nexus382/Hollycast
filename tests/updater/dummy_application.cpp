// Portions Copyright 2026 The Hollycast Authors
#include <windows.h>
#include <fstream>
int main() {
#ifdef UPDATE_PAYLOAD
    std::ofstream("new-started").put('1');
#ifdef UPDATE_STARTUP_FAILURE
    return 23;
#endif
#else
    std::ofstream("old-started", std::ios::app).put('1');
    for (int i = 0; i < 2400 && GetFileAttributesW(L"exit") == INVALID_FILE_ATTRIBUTES; ++i) Sleep(100);
#endif
    return 0;
}
