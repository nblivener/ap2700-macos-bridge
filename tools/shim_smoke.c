/* Hardware smoke test: enumeration, standard USB descriptor read, close.
 * Never sends AP/vendor commands or bulk transfers. Run through Wine with the
 * native helper active, APUSB_PORT/APUSB_TOKEN set, beside apbridge.dll.
 */
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HDEVINFO (WINAPI *GetClassFn)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *EnumFn)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *DetailFn)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *DestroyFn)(HDEVINFO);
typedef HANDLE (WINAPI *OpenFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *IoctlFn)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseFn)(HANDLE);
typedef BOOL (WINAPI *ResultFn)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s (Win32 error %lu)\n", what, (unsigned long)GetLastError());
    exit(1);
}

/* Avoid function-pointer casts with incompatible declared signatures. */
#define LOAD(var, type, symbol) \
    type var; do { FARPROC address = GetProcAddress(dll, symbol); \
    if (!address) { fail(symbol); } \
    memcpy(&var, &address, sizeof(var)); } while (0)

int main(void) {
    const GUID ap = {0x21a66589, 0xff79, 0x47c0, {0x8e,0x90,0x28,0x07,0x71,0x2a,0x11,0x23}};
    HMODULE dll = LoadLibraryA("apbridge.dll");
    if (!dll) fail("load apbridge.dll");
    LOAD(getclass, GetClassFn, "SetupDiGetClassDevsA");
    LOAD(enumerate, EnumFn, "SetupDiEnumDeviceInterfaces");
    LOAD(detail, DetailFn, "SetupDiGetDeviceInterfaceDetailA");
    LOAD(destroy, DestroyFn, "SetupDiDestroyDeviceInfoList");
    LOAD(open_device, OpenFn, "CreateFileA");
    LOAD(ioctl, IoctlFn, "DeviceIoControl");
    LOAD(close_device, CloseFn, "CloseHandle");
    LOAD(result, ResultFn, "GetOverlappedResult");
    HDEVINFO set = getclass(&ap, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) fail("enumeration set");
    SP_DEVICE_INTERFACE_DATA iface = {0};
    iface.cbSize = sizeof(iface);
    if (!enumerate(set, NULL, &ap, 0, &iface)) fail("enumerate AP interface");
    DWORD required = 0;
    if (detail(set, &iface, NULL, 0, &required, NULL) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        fail("detail sizing convention");
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A path = calloc(1, required);
    if (!path) fail("allocation");
    path->cbSize = sizeof(*path);
    if (!detail(set, &iface, path, required, NULL, NULL)) fail("interface path");
    printf("Interface: %s\n", path->DevicePath);
    HANDLE dev = open_device(path->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (dev == INVALID_HANDLE_VALUE) fail("open USB bridge");
    free(path);
    if (enumerate(set, NULL, &ap, 1, &iface) || GetLastError() != ERROR_NO_MORE_ITEMS)
        fail("enumeration end");
    if (!destroy(set)) fail("destroy set");

    unsigned char buffer[38 + 18] = {0};
    buffer[0] = 0x80; /* standard device IN */
    buffer[1] = 6;    /* GET_DESCRIPTOR */
    buffer[3] = 1;    /* DEVICE descriptor */
    buffer[6] = 18;
    buffer[8] = 2;    /* legacy Cypress seconds */
    buffer[30] = 38;
    buffer[34] = 18;
    DWORD got = 0;
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) fail("event");
    BOOL ok = ioctl(dev, 0x220020, buffer, sizeof(buffer), buffer, sizeof(buffer), &got, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) fail("descriptor control transfer");
    if (WaitForSingleObject(ov.hEvent, 5000) != WAIT_OBJECT_0) fail("completion event");
    if (!result(dev, &ov, &got, FALSE)) fail("overlapped result");
    if (got != sizeof(buffer) || buffer[38] != 18 || buffer[39] != 1 ||
        buffer[46] != 0xe4 || buffer[47] != 0x13 || buffer[48] != 3 || buffer[49] != 0)
        fail("descriptor identity/byte count");
    DWORD nt = 0, usbd = 0;
    memcpy(&nt, buffer+14, 4); memcpy(&usbd, buffer+18, 4);
    if (nt || usbd) fail("USB status");
    printf("PASS: hardware descriptor 13e4:0003, packed header, %lu bytes, completion event and result\n", (unsigned long)got);
    CloseHandle(ov.hEvent);
    if (!close_device(dev)) fail("close bridge");
    FreeLibrary(dll);
    return 0;
}
