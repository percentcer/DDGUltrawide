; Export thunks: each jumps to the matching function in the real System32 version.dll.
; g_origFuncs is filled in by proxy.cpp before the game can call any of these.

EXTERN g_origFuncs:QWORD

THUNK MACRO name, idx
name PROC
    jmp qword ptr [g_origFuncs + idx*8]
name ENDP
ENDM

.code
THUNK Thunk_GetFileVersionInfoA, 0
THUNK Thunk_GetFileVersionInfoByHandle, 1
THUNK Thunk_GetFileVersionInfoExA, 2
THUNK Thunk_GetFileVersionInfoExW, 3
THUNK Thunk_GetFileVersionInfoSizeA, 4
THUNK Thunk_GetFileVersionInfoSizeExA, 5
THUNK Thunk_GetFileVersionInfoSizeExW, 6
THUNK Thunk_GetFileVersionInfoSizeW, 7
THUNK Thunk_GetFileVersionInfoW, 8
THUNK Thunk_VerFindFileA, 9
THUNK Thunk_VerFindFileW, 10
THUNK Thunk_VerInstallFileA, 11
THUNK Thunk_VerInstallFileW, 12
THUNK Thunk_VerLanguageNameA, 13
THUNK Thunk_VerLanguageNameW, 14
THUNK Thunk_VerQueryValueA, 15
THUNK Thunk_VerQueryValueW, 16

END
