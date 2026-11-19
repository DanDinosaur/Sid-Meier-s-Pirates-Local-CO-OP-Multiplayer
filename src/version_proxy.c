// version.dll proxy: forwards every export to the real System32\version.dll
#include <windows.h>
static HMODULE real;
static FARPROC P(const char*n){ if(!real){ char p[MAX_PATH]; GetSystemDirectoryA(p,MAX_PATH); lstrcatA(p,"\\version.dll"); real=LoadLibraryA(p);} return GetProcAddress(real,n);}
#define FWD(name, args, call) typedef DWORD (WINAPI *t_##name) args; DWORD WINAPI px_##name args { static t_##name fp_; if(!fp_) fp_=(t_##name)P(#name); return fp_ call; }
FWD(GetFileVersionInfoA,(LPCSTR a,DWORD b,DWORD c,LPVOID d),(a,b,c,d))
FWD(GetFileVersionInfoW,(LPCWSTR a,DWORD b,DWORD c,LPVOID d),(a,b,c,d))
FWD(GetFileVersionInfoSizeA,(LPCSTR a,LPDWORD b),(a,b))
FWD(GetFileVersionInfoSizeW,(LPCWSTR a,LPDWORD b),(a,b))
FWD(GetFileVersionInfoExA,(DWORD f,LPCSTR a,DWORD b,DWORD c,LPVOID d),(f,a,b,c,d))
FWD(GetFileVersionInfoExW,(DWORD f,LPCWSTR a,DWORD b,DWORD c,LPVOID d),(f,a,b,c,d))
FWD(GetFileVersionInfoSizeExA,(DWORD f,LPCSTR a,LPDWORD b),(f,a,b))
FWD(GetFileVersionInfoSizeExW,(DWORD f,LPCWSTR a,LPDWORD b),(f,a,b))
FWD(GetFileVersionInfoByHandle,(DWORD a,HANDLE b,LPVOID c,DWORD d),(a,b,c,d))
FWD(VerQueryValueA,(LPCVOID a,LPCSTR b,LPVOID*c,PUINT d),(a,b,c,d))
FWD(VerQueryValueW,(LPCVOID a,LPCWSTR b,LPVOID*c,PUINT d),(a,b,c,d))
FWD(VerFindFileA,(DWORD a,LPCSTR b,LPCSTR c,LPCSTR d,LPSTR e,PUINT f,LPSTR g,PUINT h),(a,b,c,d,e,f,g,h))
FWD(VerFindFileW,(DWORD a,LPCWSTR b,LPCWSTR c,LPCWSTR d,LPWSTR e,PUINT f,LPWSTR g,PUINT h),(a,b,c,d,e,f,g,h))
FWD(VerInstallFileA,(DWORD a,LPCSTR b,LPCSTR c,LPCSTR d,LPCSTR e,LPCSTR f,LPSTR g,PUINT h),(a,b,c,d,e,f,g,h))
FWD(VerInstallFileW,(DWORD a,LPCWSTR b,LPCWSTR c,LPCWSTR d,LPCWSTR e,LPCWSTR f,LPWSTR g,PUINT h),(a,b,c,d,e,f,g,h))
FWD(VerLanguageNameA,(DWORD a,LPSTR b,DWORD c),(a,b,c))
FWD(VerLanguageNameW,(DWORD a,LPWSTR b,DWORD c),(a,b,c))
