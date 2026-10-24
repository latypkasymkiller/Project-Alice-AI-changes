
#ifdef _WIN64
// WINDOWS implementations go here

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include "simple_fs_win.cpp"
#include "window_win.cpp"
#include "sound_win.cpp"
#include "opengl_wrapper_win.cpp"

#ifdef ALICE_EXTRA_ENTRY_POINT
#include "entry_point_win_extra.cpp"
#endif

#ifndef ALICE_NO_ENTRY_POINT
#include "entry_point_win.cpp"
#endif

#elif defined(__ANDROID__)
// ANDROID implementations go here
// simple_fs_nix.cpp e reaproveitado (so usa POSIX puro: dirent/fcntl/mmap/stat/unistd,
// tudo disponivel na libc do NDK); o Storage Access Framework para acessar os arquivos
// do Victoria 2 copiados pelo usuario fica pra Fase 5, nao bloqueia a compilacao.
#include "simple_fs_nix.cpp"
#include "window_android.cpp"
#include "sound_android.cpp"
#include "opengl_wrapper_android.cpp"

#ifndef ALICE_NO_ENTRY_POINT
#include "entry_point_android.cpp"
#endif

#else
// LINUX implementations go here

#include "simple_fs_nix.cpp"
#include "window_nix.cpp"
#include "sound_nix.cpp"
#include "opengl_wrapper_nix.cpp"

#ifndef ALICE_NO_ENTRY_POINT
#include "entry_point_nix.cpp"
#endif

#endif
