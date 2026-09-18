#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APUSB_MAGIC 0x42555041u
#define APUSB_VERSION 1u
#define APUSB_MAX_PAYLOAD (4u * 1024u * 1024u)
#define APUSB_HEADER_SIZE 24u

#define OP_AUTH 1u
#define OP_OPEN 2u
#define OP_CLOSE 3u
#define OP_CONTROL 4u
#define OP_BULK 5u
#define OP_CLEAR_HALT 6u
#define OP_SET_ALT 7u

#define IOCTL_GET_DRIVER_VERSION 0x00220000u
#define IOCTL_GET_USBDI_VERSION 0x00220004u
#define IOCTL_GET_ALT_INTERFACE 0x00220008u
#define IOCTL_SELECT_INTERFACE 0x0022000cu
#define IOCTL_GET_ADDRESS 0x00220010u
#define IOCTL_GET_POWER_STATE 0x00220018u
#define IOCTL_SET_POWER_STATE 0x0022001cu
#define IOCTL_EP0_CONTROL 0x00220020u
#define IOCTL_NON_EP0_BUFFERED 0x00220024u
#define IOCTL_CYCLE_PORT 0x00220028u
#define IOCTL_RESET_PIPE 0x0022002cu
#define IOCTL_RESET_PARENT_PORT 0x00220030u
#define IOCTL_GET_TRANSFER_SIZE 0x00220034u
#define IOCTL_SET_TRANSFER_SIZE 0x00220038u
#define IOCTL_GET_DEVICE_NAME 0x0022003cu
#define IOCTL_GET_FRIENDLY_NAME 0x00220040u
#define IOCTL_ABORT_PIPE 0x00220044u
#define IOCTL_NON_EP0_DIRECT 0x0022004bu
#define IOCTL_GET_DEVICE_SPEED 0x0022004cu

#define USB_STATUS_SUCCESS 0u
#define USB_STATUS_STALL_PID 0xc0000004u
#define USB_STATUS_DEV_NOT_RESPONDING 0xc0000005u
#define USB_STATUS_DATA_OVERRUN 0xc0000008u
#define USB_STATUS_XACT_ERROR 0xc0000011u
#define USB_STATUS_CANCELED 0xc0010000u

#define NT_STATUS_SUCCESS 0u
#define NT_STATUS_UNSUCCESSFUL 0xc0000001u
#define NT_STATUS_INVALID_PARAMETER 0xc000000du
#define NT_STATUS_NO_MEMORY 0xc0000017u
#define NT_STATUS_DEVICE_NOT_CONNECTED 0xc000009du
#define NT_STATUS_IO_TIMEOUT 0xc00000b5u
#define NT_STATUS_CANCELLED 0xc0000120u

#define HANDLE_KIND_ENUM 1
#define HANDLE_KIND_DEVICE 2

static const GUID k_ap_guid = {
    0x21a66589u, 0xff79u, 0x47c0u,
    {0x8e, 0x90, 0x28, 0x07, 0x71, 0x2a, 0x11, 0x23}
};
static const char k_device_path[] = "\\\\?\\apusb#vid_13e4&pid_0003#0";

#pragma pack(push, 1)
typedef struct single_transfer {
    uint8_t setup_or_iso[12];
    uint8_t reserved;
    uint8_t endpoint;
    uint32_t nt_status;
    uint32_t usb_status;
    uint32_t iso_packet_offset;
    uint32_t iso_packet_length;
    uint32_t buffer_offset;
    uint32_t buffer_length;
} single_transfer;

typedef struct transfer_size_info {
    uint8_t endpoint;
    uint32_t transfer_size;
} transfer_size_info;
#pragma pack(pop)

_Static_assert(sizeof(single_transfer) == 38u, "SINGLE_TRANSFER must use one-byte packing");

typedef struct rpc_result {
    int32_t status;
    uint32_t arg0;
    uint8_t *payload;
    uint32_t payload_length;
} rpc_result;

typedef struct handle_entry {
    HANDLE token;
    int kind;
    SOCKET socket_fd;
    uint8_t alt_setting;
    uint32_t transfer_size[256];
    LPOVERLAPPED last_overlapped;
    DWORD last_overlapped_error;
    DWORD last_overlapped_count;
    struct handle_entry *next;
} handle_entry;

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static handle_entry *g_handles;
static HMODULE g_setupapi;
static int g_trace;

typedef HDEVINFO (WINAPI *setup_get_class_devs_fn)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *setup_enum_interfaces_fn)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD,
                                                PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *setup_get_detail_fn)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                            PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD,
                                            PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *setup_destroy_fn)(HDEVINFO);

static setup_get_class_devs_fn g_real_get_class_devs;
static setup_enum_interfaces_fn g_real_enum_interfaces;
static setup_get_detail_fn g_real_get_detail;
static setup_destroy_fn g_real_destroy;

static BOOL CALLBACK initialize_state(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    WSADATA data;
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_lock);
    const char *trace = getenv("APUSB_TRACE");
    g_trace = trace != NULL && strcmp(trace, "0") != 0;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
    g_setupapi = LoadLibraryA("SETUPAPI.dll");
    if (g_setupapi != NULL) {
        g_real_get_class_devs = (setup_get_class_devs_fn)(uintptr_t)GetProcAddress(g_setupapi, "SetupDiGetClassDevsA");
        g_real_enum_interfaces = (setup_enum_interfaces_fn)(uintptr_t)GetProcAddress(g_setupapi, "SetupDiEnumDeviceInterfaces");
        g_real_get_detail = (setup_get_detail_fn)(uintptr_t)GetProcAddress(g_setupapi, "SetupDiGetDeviceInterfaceDetailA");
        g_real_destroy = (setup_destroy_fn)(uintptr_t)GetProcAddress(g_setupapi, "SetupDiDestroyDeviceInfoList");
    }
    return TRUE;
}

static void ensure_initialized(void)
{
    (void)InitOnceExecuteOnce(&g_init_once, initialize_state, NULL, NULL);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int send_all(SOCKET fd, const uint8_t *buf, uint32_t length)
{
    while (length != 0u) {
        int chunk = length > 0x7fffffffu ? 0x7fffffff : (int)length;
        int sent = send(fd, (const char *)buf, chunk, 0);
        if (sent <= 0) {
            return 0;
        }
        buf += (uint32_t)sent;
        length -= (uint32_t)sent;
    }
    return 1;
}

static int recv_all(SOCKET fd, uint8_t *buf, uint32_t length)
{
    while (length != 0u) {
        int chunk = length > 0x7fffffffu ? 0x7fffffff : (int)length;
        int got = recv(fd, (char *)buf, chunk, 0);
        if (got <= 0) {
            return 0;
        }
        buf += (uint32_t)got;
        length -= (uint32_t)got;
    }
    return 1;
}

static void free_rpc_result(rpc_result *result)
{
    free(result->payload);
    memset(result, 0, sizeof(*result));
}

static int rpc_call(SOCKET fd, uint32_t opcode, uint32_t arg0, const void *payload,
                    uint32_t payload_length, rpc_result *result)
{
    uint8_t header[APUSB_HEADER_SIZE];
    uint8_t response[APUSB_HEADER_SIZE];
    memset(result, 0, sizeof(*result));
    if (payload_length > APUSB_MAX_PAYLOAD) {
        SetLastError(ERROR_FILE_TOO_LARGE);
        return 0;
    }
    put_le32(header + 0, APUSB_MAGIC);
    put_le32(header + 4, APUSB_VERSION);
    put_le32(header + 8, opcode);
    put_le32(header + 12, 0u);
    put_le32(header + 16, arg0);
    put_le32(header + 20, payload_length);
    if (!send_all(fd, header, sizeof(header)) ||
        (payload_length != 0u && !send_all(fd, (const uint8_t *)payload, payload_length)) ||
        !recv_all(fd, response, sizeof(response))) {
        SetLastError(ERROR_CONNECTION_ABORTED);
        return 0;
    }
    if (get_le32(response + 0) != APUSB_MAGIC || get_le32(response + 4) != APUSB_VERSION ||
        get_le32(response + 8) != opcode || get_le32(response + 20) > APUSB_MAX_PAYLOAD) {
        SetLastError(ERROR_INVALID_DATA);
        return 0;
    }
    result->status = (int32_t)get_le32(response + 12);
    result->arg0 = get_le32(response + 16);
    result->payload_length = get_le32(response + 20);
    if (result->payload_length != 0u) {
        result->payload = (uint8_t *)malloc(result->payload_length);
        if (result->payload == NULL) {
            SetLastError(ERROR_OUTOFMEMORY);
            return 0;
        }
        if (!recv_all(fd, result->payload, result->payload_length)) {
            free_rpc_result(result);
            SetLastError(ERROR_CONNECTION_ABORTED);
            return 0;
        }
    }
    return 1;
}

static DWORD libusb_to_win32(int32_t status)
{
    switch (status) {
    case 0: return ERROR_SUCCESS;
    case -1: return ERROR_GEN_FAILURE;
    case -2: return ERROR_INVALID_PARAMETER;
    case -3: return ERROR_ACCESS_DENIED;
    case -4: return ERROR_DEVICE_NOT_CONNECTED;
    case -5: return ERROR_NOT_FOUND;
    case -6: return ERROR_BUSY;
    case -7: return ERROR_SEM_TIMEOUT;
    case -8: return ERROR_BUFFER_OVERFLOW;
    case -9: return ERROR_GEN_FAILURE;
    case -10: return ERROR_OPERATION_ABORTED;
    case -11: return ERROR_OUTOFMEMORY;
    case -12: return ERROR_NOT_SUPPORTED;
    default: return ERROR_GEN_FAILURE;
    }
}

static void set_transfer_status(single_transfer *transfer, int32_t status)
{
    transfer->nt_status = NT_STATUS_SUCCESS;
    transfer->usb_status = USB_STATUS_SUCCESS;
    if (status >= 0) {
        return;
    }
    switch (status) {
    case -2:
        transfer->nt_status = NT_STATUS_INVALID_PARAMETER;
        break;
    case -4:
        transfer->nt_status = NT_STATUS_DEVICE_NOT_CONNECTED;
        transfer->usb_status = USB_STATUS_DEV_NOT_RESPONDING;
        break;
    case -7:
        transfer->nt_status = NT_STATUS_IO_TIMEOUT;
        transfer->usb_status = USB_STATUS_DEV_NOT_RESPONDING;
        break;
    case -8:
        transfer->nt_status = NT_STATUS_UNSUCCESSFUL;
        transfer->usb_status = USB_STATUS_DATA_OVERRUN;
        break;
    case -9:
        transfer->nt_status = NT_STATUS_UNSUCCESSFUL;
        transfer->usb_status = USB_STATUS_STALL_PID;
        break;
    case -10:
        transfer->nt_status = NT_STATUS_CANCELLED;
        transfer->usb_status = USB_STATUS_CANCELED;
        break;
    case -11:
        transfer->nt_status = NT_STATUS_NO_MEMORY;
        break;
    default:
        transfer->nt_status = NT_STATUS_UNSUCCESSFUL;
        transfer->usb_status = USB_STATUS_XACT_ERROR;
        break;
    }
}

static handle_entry *find_handle_locked(HANDLE token, int kind)
{
    handle_entry *entry;
    for (entry = g_handles; entry != NULL; entry = entry->next) {
        if (entry->token == token && (kind == 0 || entry->kind == kind)) {
            return entry;
        }
    }
    return NULL;
}

static HANDLE add_handle_locked(int kind, SOCKET fd)
{
    handle_entry *entry;
    HANDLE token = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (token == NULL) {
        return INVALID_HANDLE_VALUE;
    }
    entry = (handle_entry *)calloc(1u, sizeof(*entry));
    if (entry == NULL) {
        (void)CloseHandle(token);
        SetLastError(ERROR_OUTOFMEMORY);
        return INVALID_HANDLE_VALUE;
    }
    entry->token = token;
    entry->kind = kind;
    entry->socket_fd = fd;
    entry->next = g_handles;
    g_handles = entry;
    return token;
}

static handle_entry *remove_handle_locked(HANDLE token)
{
    handle_entry **cursor = &g_handles;
    while (*cursor != NULL) {
        if ((*cursor)->token == token) {
            handle_entry *found = *cursor;
            *cursor = found->next;
            found->next = NULL;
            return found;
        }
        cursor = &(*cursor)->next;
    }
    return NULL;
}

static int guid_equal(const GUID *left, const GUID *right)
{
    return left != NULL && memcmp(left, right, sizeof(*left)) == 0;
}

static void log_guid(const GUID *guid)
{
    if (guid == NULL) {
        fprintf(stderr, "apbridge: SetupDiGetClassDevsA GUID=(null)\n");
        return;
    }
    fprintf(stderr,
            "apbridge: SetupDiGetClassDevsA GUID={%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n",
            (unsigned long)guid->Data1, (unsigned)guid->Data2, (unsigned)guid->Data3,
            (unsigned)guid->Data4[0], (unsigned)guid->Data4[1], (unsigned)guid->Data4[2],
            (unsigned)guid->Data4[3], (unsigned)guid->Data4[4], (unsigned)guid->Data4[5],
            (unsigned)guid->Data4[6], (unsigned)guid->Data4[7]);
}

static SOCKET connect_native_bridge(void)
{
    const char *port_text = getenv("APUSB_PORT");
    const char *token = getenv("APUSB_TOKEN");
    char *end = NULL;
    long port;
    SOCKET fd;
    struct sockaddr_in address;
    DWORD timeout_ms = 35000u;
    int no_delay = 1;
    rpc_result response;
    size_t i;

    if (port_text == NULL || token == NULL || strlen(token) != 32u) {
        SetLastError(ERROR_BAD_ENVIRONMENT);
        return INVALID_SOCKET;
    }
    for (i = 0; i < 32u; ++i) {
        unsigned char ch = (unsigned char)token[i];
        if (ch < 0x20u || ch > 0x7eu) {
            SetLastError(ERROR_BAD_ENVIRONMENT);
            return INVALID_SOCKET;
        }
    }
    port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port < 1 || port > 65535) {
        SetLastError(ERROR_BAD_ENVIRONMENT);
        return INVALID_SOCKET;
    }
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        SetLastError(ERROR_CONNECTION_REFUSED);
        return INVALID_SOCKET;
    }
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        closesocket(fd);
        SetLastError(ERROR_CONNECTION_REFUSED);
        return INVALID_SOCKET;
    }
    if (!rpc_call(fd, OP_AUTH, 0u, token, 32u, &response)) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    if (response.status != 0) {
        DWORD error = libusb_to_win32(response.status);
        free_rpc_result(&response);
        closesocket(fd);
        SetLastError(error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error);
        return INVALID_SOCKET;
    }
    free_rpc_result(&response);
    if (!rpc_call(fd, OP_OPEN, 0u, NULL, 0u, &response)) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    if (response.status != 0 || response.payload_length < 18u) {
        DWORD error = response.status == 0 ? ERROR_INVALID_DATA : libusb_to_win32(response.status);
        free_rpc_result(&response);
        closesocket(fd);
        SetLastError(error);
        return INVALID_SOCKET;
    }
    free_rpc_result(&response);
    return fd;
}

static int is_synthetic_path(LPCSTR path)
{
    return path != NULL && _stricmp(path, k_device_path) == 0;
}

static uint32_t win32_to_ntstatus(DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS: return NT_STATUS_SUCCESS;
    case ERROR_INVALID_PARAMETER: return NT_STATUS_INVALID_PARAMETER;
    case ERROR_OUTOFMEMORY: return NT_STATUS_NO_MEMORY;
    case ERROR_DEVICE_NOT_CONNECTED: return NT_STATUS_DEVICE_NOT_CONNECTED;
    case ERROR_SEM_TIMEOUT: return NT_STATUS_IO_TIMEOUT;
    case ERROR_OPERATION_ABORTED: return NT_STATUS_CANCELLED;
    default: return NT_STATUS_UNSUCCESSFUL;
    }
}

static void complete_overlapped(LPOVERLAPPED overlapped, DWORD error, DWORD count)
{
    if (overlapped == NULL) {
        return;
    }
    overlapped->Internal = (ULONG_PTR)win32_to_ntstatus(error);
    overlapped->InternalHigh = (ULONG_PTR)count;
    if (overlapped->hEvent != NULL) {
        (void)SetEvent(overlapped->hEvent);
    }
}

static BOOL finish_ioctl(LPDWORD bytes_returned, LPOVERLAPPED overlapped, DWORD error, DWORD count)
{
    if (bytes_returned != NULL) {
        *bytes_returned = count;
    }
    complete_overlapped(overlapped, error, count);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

static int write_u32_result(LPVOID output, DWORD output_size, uint32_t value, DWORD *count)
{
    if (output == NULL || output_size < 4u) {
        return 0;
    }
    memcpy(output, &value, sizeof(value));
    *count = 4u;
    return 1;
}

static int write_bytes_result(LPVOID output, DWORD output_size, const void *data, DWORD length,
                              DWORD *count)
{
    if (output == NULL || output_size < length) {
        return 0;
    }
    memcpy(output, data, length);
    *count = length;
    return 1;
}

static DWORD bounded_timeout_ms(uint32_t seconds)
{
    uint64_t milliseconds = (uint64_t)seconds * 1000u;
    if (milliseconds == 0u) {
        milliseconds = 10000u;
    }
    if (milliseconds > 30000u) {
        milliseconds = 30000u;
    }
    return (DWORD)milliseconds;
}

static DWORD do_control(handle_entry *entry, LPVOID input, DWORD input_size, LPVOID output,
                        DWORD output_size, DWORD *completed)
{
    single_transfer *transfer;
    single_transfer *output_transfer;
    uint32_t offset;
    uint32_t requested;
    uint32_t out_length;
    uint32_t payload_length;
    uint8_t *payload;
    int direction_in;
    rpc_result response;
    DWORD error;
    uint32_t actual;

    if (input == NULL || output == NULL || input_size < sizeof(single_transfer) ||
        output_size < sizeof(single_transfer)) {
        return ERROR_INVALID_PARAMETER;
    }
    transfer = (single_transfer *)input;
    output_transfer = (single_transfer *)output;
    if (output_transfer != transfer) {
        memcpy(output_transfer, transfer, sizeof(*output_transfer));
    }
    offset = transfer->buffer_offset;
    requested = transfer->buffer_length;
    if (offset < sizeof(single_transfer) || offset > input_size || offset > output_size ||
        requested > input_size - offset || requested > output_size - offset ||
        requested != (uint32_t)get_le16(transfer->setup_or_iso + 6)) {
        set_transfer_status(output_transfer, -2);
        return ERROR_INVALID_PARAMETER;
    }
    direction_in = (transfer->setup_or_iso[0] & 0x80u) != 0u;
    out_length = direction_in ? 0u : requested;
    if (out_length > APUSB_MAX_PAYLOAD - 12u) {
        set_transfer_status(output_transfer, -2);
        return ERROR_FILE_TOO_LARGE;
    }
    payload_length = 12u + out_length;
    payload = (uint8_t *)malloc(payload_length);
    if (payload == NULL) {
        set_transfer_status(output_transfer, -11);
        return ERROR_OUTOFMEMORY;
    }
    memcpy(payload, transfer->setup_or_iso, 8u);
    put_le32(payload + 8, bounded_timeout_ms(get_le32(transfer->setup_or_iso + 8)));
    if (out_length != 0u) {
        memcpy(payload + 12, (uint8_t *)input + offset, out_length);
    }
    if (!rpc_call(entry->socket_fd, OP_CONTROL, 0u, payload, payload_length, &response)) {
        free(payload);
        set_transfer_status(output_transfer, -4);
        return GetLastError();
    }
    free(payload);
    set_transfer_status(output_transfer, response.status);
    actual = response.status >= 0 ? (uint32_t)response.status : response.payload_length;
    if (actual > requested || response.payload_length > requested) {
        free_rpc_result(&response);
        set_transfer_status(output_transfer, -8);
        return ERROR_BUFFER_OVERFLOW;
    }
    if (direction_in && response.payload_length != 0u) {
        memcpy((uint8_t *)output + offset, response.payload, response.payload_length);
    }
    *completed = offset + actual;
    error = response.status < 0 ? libusb_to_win32(response.status) : ERROR_SUCCESS;
    free_rpc_result(&response);
    return error;
}

static DWORD do_bulk(handle_entry *entry, LPVOID input, DWORD input_size, LPVOID output,
                     DWORD output_size, int direct, DWORD *completed)
{
    single_transfer *transfer;
    uint32_t requested;
    uint32_t timeout_ms;
    uint32_t out_length;
    uint32_t payload_length;
    uint8_t *payload;
    int direction_in;
    rpc_result response;
    DWORD error;
    uint32_t actual;
    uint8_t *data_out;

    if (input == NULL || input_size < sizeof(single_transfer)) {
        return ERROR_INVALID_PARAMETER;
    }
    transfer = (single_transfer *)input;
    if (transfer->iso_packet_length != 0u || transfer->iso_packet_offset != 0u) {
        set_transfer_status(transfer, -12);
        return ERROR_NOT_SUPPORTED;
    }
    direction_in = (transfer->endpoint & 0x80u) != 0u;
    if (direct) {
        if (transfer->buffer_offset != 0u || transfer->buffer_length != 0u ||
            (output_size != 0u && output == NULL)) {
            set_transfer_status(transfer, -2);
            return ERROR_INVALID_PARAMETER;
        }
        requested = output_size;
        data_out = (uint8_t *)output;
    } else {
        uint32_t offset = transfer->buffer_offset;
        requested = transfer->buffer_length;
        if (output == NULL || output_size < sizeof(single_transfer) ||
            offset < sizeof(single_transfer) || offset > input_size || offset > output_size ||
            requested > input_size - offset || requested > output_size - offset) {
            set_transfer_status(transfer, -2);
            return ERROR_INVALID_PARAMETER;
        }
        data_out = (uint8_t *)output + offset;
    }
    if (requested > APUSB_MAX_PAYLOAD - 8u) {
        set_transfer_status(transfer, -2);
        return ERROR_FILE_TOO_LARGE;
    }
    timeout_ms = 10000u;
    out_length = direction_in ? 0u : requested;
    payload_length = 8u + out_length;
    payload = (uint8_t *)malloc(payload_length);
    if (payload == NULL) {
        set_transfer_status(transfer, -11);
        return ERROR_OUTOFMEMORY;
    }
    put_le32(payload + 0, timeout_ms);
    put_le32(payload + 4, requested);
    if (out_length != 0u) {
        if (direct) {
            memcpy(payload + 8, output, out_length);
        } else {
            memcpy(payload + 8, (uint8_t *)input + transfer->buffer_offset, out_length);
        }
    }
    if (!rpc_call(entry->socket_fd, OP_BULK, transfer->endpoint, payload, payload_length, &response)) {
        free(payload);
        set_transfer_status(transfer, -4);
        return GetLastError();
    }
    free(payload);
    set_transfer_status(transfer, response.status);
    actual = response.arg0;
    if (actual > requested || response.payload_length > requested ||
        (direction_in && response.payload_length < actual)) {
        free_rpc_result(&response);
        set_transfer_status(transfer, -8);
        return ERROR_BUFFER_OVERFLOW;
    }
    if (direction_in && actual != 0u) {
        memcpy(data_out, response.payload, actual);
    }
    *completed = direct ? actual : transfer->buffer_offset + actual;
    error = libusb_to_win32(response.status);
    free_rpc_result(&response);
    return error;
}

__declspec(dllexport) HDEVINFO WINAPI AP_SetupDiGetClassDevsA(const GUID *class_guid,
                                                              PCSTR enumerator, HWND parent,
                                                              DWORD flags)
{
    HDEVINFO result;
    ensure_initialized();
    log_guid(class_guid);
    if (!guid_equal(class_guid, &k_ap_guid)) {
        if (g_real_get_class_devs == NULL) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        return g_real_get_class_devs(class_guid, enumerator, parent, flags);
    }
    EnterCriticalSection(&g_lock);
    result = (HDEVINFO)add_handle_locked(HANDLE_KIND_ENUM, INVALID_SOCKET);
    LeaveCriticalSection(&g_lock);
    if (result != INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_SUCCESS);
    }
    return result;
}

__declspec(dllexport) BOOL WINAPI AP_SetupDiEnumDeviceInterfaces(
    HDEVINFO info_set, PSP_DEVINFO_DATA device_info, const GUID *interface_guid, DWORD index,
    PSP_DEVICE_INTERFACE_DATA interface_data)
{
    int fake;
    ensure_initialized();
    EnterCriticalSection(&g_lock);
    fake = find_handle_locked((HANDLE)info_set, HANDLE_KIND_ENUM) != NULL;
    LeaveCriticalSection(&g_lock);
    if (!fake) {
        if (g_real_enum_interfaces == NULL) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
        return g_real_enum_interfaces(info_set, device_info, interface_guid, index, interface_data);
    }
    if (index != 0u) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (interface_data == NULL || interface_data->cbSize != sizeof(*interface_data)) {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    interface_data->InterfaceClassGuid = k_ap_guid;
    interface_data->Flags = SPINT_ACTIVE;
    interface_data->Reserved = 0;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI AP_SetupDiGetDeviceInterfaceDetailA(
    HDEVINFO info_set, PSP_DEVICE_INTERFACE_DATA interface_data,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail, DWORD detail_size, PDWORD required_size,
    PSP_DEVINFO_DATA device_info)
{
    DWORD needed = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) +
                           sizeof(k_device_path));
    int fake;
    (void)interface_data;
    (void)device_info;
    ensure_initialized();
    EnterCriticalSection(&g_lock);
    fake = find_handle_locked((HANDLE)info_set, HANDLE_KIND_ENUM) != NULL;
    LeaveCriticalSection(&g_lock);
    if (!fake) {
        if (g_real_get_detail == NULL) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
        return g_real_get_detail(info_set, interface_data, detail, detail_size, required_size,
                                 device_info);
    }
    if (required_size != NULL) {
        *required_size = needed;
    }
    if (detail == NULL || detail_size < needed) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (detail->cbSize != 5u && detail->cbSize != sizeof(*detail)) {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    memcpy((uint8_t *)detail + offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath),
           k_device_path, sizeof(k_device_path));
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI AP_SetupDiDestroyDeviceInfoList(HDEVINFO info_set)
{
    handle_entry *entry;
    ensure_initialized();
    EnterCriticalSection(&g_lock);
    entry = find_handle_locked((HANDLE)info_set, HANDLE_KIND_ENUM) != NULL
                ? remove_handle_locked((HANDLE)info_set)
                : NULL;
    LeaveCriticalSection(&g_lock);
    if (entry == NULL) {
        if (g_real_destroy == NULL) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
        return g_real_destroy(info_set);
    }
    (void)CloseHandle(entry->token);
    free(entry);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__declspec(dllexport) HANDLE WINAPI AP_CreateFileA(LPCSTR file_name, DWORD desired_access,
                                                   DWORD share_mode,
                                                   LPSECURITY_ATTRIBUTES attributes,
                                                   DWORD creation_disposition, DWORD flags,
                                                   HANDLE template_file)
{
    SOCKET fd;
    HANDLE token;
    ensure_initialized();
    if (!is_synthetic_path(file_name)) {
        return CreateFileA(file_name, desired_access, share_mode, attributes, creation_disposition,
                           flags, template_file);
    }
    fd = connect_native_bridge();
    if (fd == INVALID_SOCKET) {
        return INVALID_HANDLE_VALUE;
    }
    EnterCriticalSection(&g_lock);
    token = add_handle_locked(HANDLE_KIND_DEVICE, fd);
    LeaveCriticalSection(&g_lock);
    if (token == INVALID_HANDLE_VALUE) {
        closesocket(fd);
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(ERROR_SUCCESS);
    return token;
}

__declspec(dllexport) BOOL WINAPI AP_CloseHandle(HANDLE object)
{
    handle_entry *entry;
    rpc_result response;
    ensure_initialized();
    EnterCriticalSection(&g_lock);
    entry = remove_handle_locked(object);
    if (entry != NULL && entry->kind == HANDLE_KIND_DEVICE) {
        if (rpc_call(entry->socket_fd, OP_CLOSE, 0u, NULL, 0u, &response)) {
            free_rpc_result(&response);
        }
        closesocket(entry->socket_fd);
    }
    LeaveCriticalSection(&g_lock);
    if (entry == NULL) {
        return CloseHandle(object);
    }
    (void)CloseHandle(entry->token);
    free(entry);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI AP_GetOverlappedResult(HANDLE object, LPOVERLAPPED overlapped,
                                                         LPDWORD transferred, BOOL wait)
{
    handle_entry *entry;
    DWORD error;
    (void)wait;
    ensure_initialized();
    EnterCriticalSection(&g_lock);
    entry = find_handle_locked(object, HANDLE_KIND_DEVICE);
    if (entry == NULL) {
        LeaveCriticalSection(&g_lock);
        return GetOverlappedResult(object, overlapped, transferred, wait);
    }
    if (overlapped == NULL || transferred == NULL) {
        LeaveCriticalSection(&g_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (entry->last_overlapped != overlapped) {
        LeaveCriticalSection(&g_lock);
        SetLastError(ERROR_IO_INCOMPLETE);
        return FALSE;
    }
    error = entry->last_overlapped_error;
    *transferred = entry->last_overlapped_count;
    LeaveCriticalSection(&g_lock);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

__declspec(dllexport) BOOL WINAPI AP_DeviceIoControl(HANDLE device, DWORD code, LPVOID input,
                                                     DWORD input_size, LPVOID output,
                                                     DWORD output_size, LPDWORD bytes_returned,
                                                     LPOVERLAPPED overlapped)
{
    handle_entry *entry;
    DWORD count = 0u;
    DWORD error = ERROR_SUCCESS;
    rpc_result response;
    uint8_t endpoint;
    transfer_size_info size_info;
    const char *name;
    size_t name_length;

    ensure_initialized();
    EnterCriticalSection(&g_lock);
    entry = find_handle_locked(device, HANDLE_KIND_DEVICE);
    if (entry == NULL) {
        LeaveCriticalSection(&g_lock);
        return DeviceIoControl(device, code, input, input_size, output, output_size, bytes_returned,
                               overlapped);
    }
    if (overlapped != NULL && overlapped->hEvent != NULL) {
        (void)ResetEvent(overlapped->hEvent);
    }
    if (g_trace) fprintf(stderr, "apbridge: ioctl code=0x%08lx in=%lu out=%lu overlapped=%u\n",
            (unsigned long)code, (unsigned long)input_size, (unsigned long)output_size,
            overlapped != NULL ? 1u : 0u);
    switch (code) {
    case IOCTL_GET_DRIVER_VERSION:
        if (!write_u32_result(output, output_size, 0x03040293u, &count)) error = ERROR_INSUFFICIENT_BUFFER;
        break;
    case IOCTL_GET_USBDI_VERSION:
        if (!write_u32_result(output, output_size, 0x00000600u, &count)) error = ERROR_INSUFFICIENT_BUFFER;
        break;
    case IOCTL_GET_ALT_INTERFACE:
        if (!write_bytes_result(output, output_size, &entry->alt_setting, 1u, &count)) error = ERROR_INSUFFICIENT_BUFFER;
        break;
    case IOCTL_SELECT_INTERFACE:
        if (input == NULL || input_size < 1u) {
            error = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!rpc_call(entry->socket_fd, OP_SET_ALT, *(const uint8_t *)input, NULL, 0u, &response)) {
            error = GetLastError();
            break;
        }
        error = libusb_to_win32(response.status);
        if (error == ERROR_SUCCESS) entry->alt_setting = *(const uint8_t *)input;
        free_rpc_result(&response);
        break;
    case IOCTL_GET_ADDRESS: {
        uint8_t address = 1u;
        if (!write_bytes_result(output, output_size, &address, 1u, &count)) error = ERROR_INSUFFICIENT_BUFFER;
        break;
    }
    case IOCTL_GET_POWER_STATE:
        if (!write_u32_result(output, output_size, 1u, &count)) error = ERROR_INSUFFICIENT_BUFFER;
        break;
    case IOCTL_SET_POWER_STATE:
        error = ERROR_NOT_SUPPORTED;
        break;
    case IOCTL_EP0_CONTROL:
        error = do_control(entry, input, input_size, output, output_size, &count);
        break;
    case IOCTL_NON_EP0_BUFFERED:
        error = do_bulk(entry, input, input_size, output, output_size, 0, &count);
        break;
    case IOCTL_NON_EP0_DIRECT:
        error = do_bulk(entry, input, input_size, output, output_size, 1, &count);
        break;
    case IOCTL_CYCLE_PORT:
    case IOCTL_RESET_PARENT_PORT:
        error = ERROR_NOT_SUPPORTED;
        break;
    case IOCTL_RESET_PIPE:
        if (input == NULL || input_size < 1u) {
            error = ERROR_INVALID_PARAMETER;
            break;
        }
        endpoint = *(const uint8_t *)input;
        if (!rpc_call(entry->socket_fd, OP_CLEAR_HALT, endpoint, NULL, 0u, &response)) {
            error = GetLastError();
            break;
        }
        error = libusb_to_win32(response.status);
        free_rpc_result(&response);
        break;
    case IOCTL_ABORT_PIPE:
        if (input == NULL || input_size < 1u) error = ERROR_INVALID_PARAMETER;
        break;
    case IOCTL_GET_TRANSFER_SIZE:
        if (input == NULL || input_size < sizeof(size_info) || output == NULL ||
            output_size < sizeof(size_info)) {
            error = ERROR_INVALID_PARAMETER;
            break;
        }
        memcpy(&size_info, input, sizeof(size_info));
        size_info.transfer_size = entry->transfer_size[size_info.endpoint];
        if (size_info.transfer_size == 0u) size_info.transfer_size = 4096u;
        memcpy(output, &size_info, sizeof(size_info));
        count = sizeof(size_info);
        break;
    case IOCTL_SET_TRANSFER_SIZE:
        if (input == NULL || input_size < sizeof(size_info)) {
            error = ERROR_INVALID_PARAMETER;
            break;
        }
        memcpy(&size_info, input, sizeof(size_info));
        if (size_info.transfer_size == 0u || size_info.transfer_size > APUSB_MAX_PAYLOAD) {
            error = ERROR_INVALID_PARAMETER;
            break;
        }
        entry->transfer_size[size_info.endpoint] = size_info.transfer_size;
        break;
    case IOCTL_GET_DEVICE_NAME:
    case IOCTL_GET_FRIENDLY_NAME:
        name = code == IOCTL_GET_DEVICE_NAME ? "APIB USB Device" : "Audio Precision AP2700 USB";
        name_length = strlen(name) + 1u;
        if (name_length > UINT32_MAX ||
            !write_bytes_result(output, output_size, name, (DWORD)name_length, &count)) {
            error = ERROR_INSUFFICIENT_BUFFER;
        }
        break;
    case IOCTL_GET_DEVICE_SPEED:
        if (!write_u32_result(output, output_size, 2u, &count)) error = ERROR_INSUFFICIENT_BUFFER;
        break;
    default:
        error = ERROR_NOT_SUPPORTED;
        break;
    }
    if (overlapped != NULL) {
        entry->last_overlapped = overlapped;
        entry->last_overlapped_error = error;
        entry->last_overlapped_count = count;
    }
    if (g_trace || error) fprintf(stderr, "apbridge: ioctl done code=0x%08lx error=%lu count=%lu\n",
            (unsigned long)code, (unsigned long)error, (unsigned long)count);
    LeaveCriticalSection(&g_lock);
    return finish_ioctl(bytes_returned, overlapped, error, count);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
