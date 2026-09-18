#include "apusb_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libusb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define APUSB_VENDOR_ID 0x13e4u
#define APUSB_PRODUCT_ID 0x0003u
#define APUSB_INTERFACE 0
#define APUSB_EP_OUT 0x02u
#define APUSB_EP_IN 0x86u

static bool trace_transfers;

struct request {
    uint32_t opcode;
    uint32_t arg0;
    uint32_t payload_length;
    uint8_t *payload;
};

struct session {
    int fd;
    bool authenticated;
    libusb_device_handle *device;
};

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* Returns 1 on success, 0 on orderly EOF, and -1 on an I/O error. */
static int read_full(int fd, void *buffer, size_t length)
{
    uint8_t *p = buffer;

    while (length != 0) {
        ssize_t count = read(fd, p, length);
        if (count > 0) {
            p += (size_t)count;
            length -= (size_t)count;
            continue;
        }
        if (count == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 1;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *p = buffer;

    while (length != 0) {
        ssize_t count = write(fd, p, length);
        if (count > 0) {
            p += (size_t)count;
            length -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int send_response(struct session *session, uint32_t opcode, int status,
                         uint32_t arg0, const void *payload,
                         uint32_t payload_length)
{
    uint8_t header[APUSB_HEADER_SIZE];

    put_le32(header + 0, APUSB_MAGIC);
    put_le32(header + 4, APUSB_VERSION);
    put_le32(header + 8, opcode);
    put_le32(header + 12, (uint32_t)status);
    put_le32(header + 16, arg0);
    put_le32(header + 20, payload_length);

    if (write_full(session->fd, header, sizeof(header)) != 0)
        return -1;
    if (payload_length != 0 &&
        write_full(session->fd, payload, payload_length) != 0)
        return -1;
    return 0;
}

/* Returns 1 for a request, 0 for EOF, -1 for I/O, and -2 for bad framing. */
static int receive_request(struct session *session, struct request *request)
{
    uint8_t header[APUSB_HEADER_SIZE];
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    int result;

    memset(request, 0, sizeof(*request));
    result = read_full(session->fd, header, sizeof(header));
    if (result <= 0)
        return result;

    magic = get_le32(header + 0);
    version = get_le32(header + 4);
    request->opcode = get_le32(header + 8);
    status = get_le32(header + 12);
    request->arg0 = get_le32(header + 16);
    request->payload_length = get_le32(header + 20);

    if (magic != APUSB_MAGIC || version != APUSB_VERSION || status != 0 ||
        request->payload_length > APUSB_MAX_PAYLOAD)
        return -2;

    if (request->payload_length != 0) {
        request->payload = malloc(request->payload_length);
        if (request->payload == NULL)
            return -1;
        result = read_full(session->fd, request->payload,
                           request->payload_length);
        if (result != 1) {
            free(request->payload);
            request->payload = NULL;
            return result == 0 ? -2 : -1;
        }
    }
    return 1;
}

static bool valid_token(const char *token)
{
    size_t i;

    if (token == NULL || strlen(token) != APUSB_TOKEN_SIZE)
        return false;
    for (i = 0; i < APUSB_TOKEN_SIZE; ++i) {
        char c = token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static bool token_equal(const uint8_t *provided, const char *expected)
{
    unsigned int difference = 0;
    size_t i;

    for (i = 0; i < APUSB_TOKEN_SIZE; ++i)
        difference |= (unsigned int)(provided[i] ^ (uint8_t)expected[i]);
    return difference == 0;
}

static unsigned int clamp_timeout(uint32_t timeout_ms)
{
    if (timeout_ms == 0)
        return APUSB_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > APUSB_MAX_TIMEOUT_MS)
        return APUSB_MAX_TIMEOUT_MS;
    return timeout_ms;
}

static void close_device(struct session *session)
{
    if (session->device != NULL) {
        (void)libusb_release_interface(session->device, APUSB_INTERFACE);
        libusb_close(session->device);
        session->device = NULL;
    }
}

static int find_and_open_device(libusb_context *context,
                                libusb_device_handle **handle)
{
    libusb_device **devices = NULL;
    ssize_t count;
    ssize_t i;
    int result = LIBUSB_ERROR_NO_DEVICE;

    *handle = NULL;
    count = libusb_get_device_list(context, &devices);
    if (count < 0)
        return (int)count;

    for (i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        int descriptor_result;

        descriptor_result =
            libusb_get_device_descriptor(devices[i], &descriptor);
        if (descriptor_result != LIBUSB_SUCCESS)
            continue;
        if (descriptor.idVendor != APUSB_VENDOR_ID ||
            descriptor.idProduct != APUSB_PRODUCT_ID)
            continue;

        result = libusb_open(devices[i], handle);
        break;
    }
    libusb_free_device_list(devices, 1);
    return result;
}

static int read_raw_descriptors(libusb_device_handle *device, uint8_t **payload,
                                uint32_t *payload_length)
{
    uint8_t device_descriptor[18];
    uint8_t config_header[9];
    uint8_t *descriptors;
    uint16_t config_length;
    int result;
    unsigned int timeout = APUSB_DEFAULT_TIMEOUT_MS;

    *payload = NULL;
    *payload_length = 0;

    result = libusb_control_transfer(
        device, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD |
                    LIBUSB_RECIPIENT_DEVICE,
        LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_DEVICE << 8), 0,
        device_descriptor, (uint16_t)sizeof(device_descriptor), timeout);
    if (result < 0)
        return result;
    if ((size_t)result != sizeof(device_descriptor))
        return LIBUSB_ERROR_IO;

    result = libusb_control_transfer(
        device, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD |
                    LIBUSB_RECIPIENT_DEVICE,
        LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_CONFIG << 8), 0,
        config_header, (uint16_t)sizeof(config_header), timeout);
    if (result < 0)
        return result;
    if ((size_t)result != sizeof(config_header))
        return LIBUSB_ERROR_IO;

    config_length = get_le16(config_header + 2);
    if (config_length < sizeof(config_header) ||
        config_length > APUSB_MAX_DESCRIPTOR_SIZE)
        return LIBUSB_ERROR_IO;

    descriptors = malloc(sizeof(device_descriptor) + config_length);
    if (descriptors == NULL)
        return LIBUSB_ERROR_NO_MEM;
    memcpy(descriptors, device_descriptor, sizeof(device_descriptor));

    result = libusb_control_transfer(
        device, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD |
                    LIBUSB_RECIPIENT_DEVICE,
        LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_CONFIG << 8), 0,
        descriptors + sizeof(device_descriptor), config_length, timeout);
    if (result < 0) {
        free(descriptors);
        return result;
    }
    if (result != config_length) {
        free(descriptors);
        return LIBUSB_ERROR_IO;
    }

    *payload = descriptors;
    *payload_length = (uint32_t)sizeof(device_descriptor) + config_length;
    return LIBUSB_SUCCESS;
}

static int handle_auth(struct session *session, const struct request *request,
                       const char *token)
{
    int status = LIBUSB_ERROR_ACCESS;

    if (!session->authenticated && request->arg0 == 0 &&
        request->payload_length == APUSB_TOKEN_SIZE &&
        token_equal(request->payload, token)) {
        session->authenticated = true;
        status = LIBUSB_SUCCESS;
    }
    fprintf(stderr, "auth status=%d\n", status);
    if (send_response(session, request->opcode, status, 0, NULL, 0) != 0)
        return -1;
    return status == LIBUSB_SUCCESS ? 0 : -1;
}

static int handle_open(struct session *session, const struct request *request,
                       libusb_context *context)
{
    uint8_t *descriptors = NULL;
    uint32_t descriptor_length = 0;
    int status;

    if (request->payload_length != 0 || request->arg0 != 0)
        return -2;
    if (session->device != NULL) {
        status = LIBUSB_ERROR_BUSY;
    } else {
        status = find_and_open_device(context, &session->device);
        if (status == LIBUSB_SUCCESS) {
            status = libusb_claim_interface(session->device, APUSB_INTERFACE);
            if (status == LIBUSB_SUCCESS)
                status = read_raw_descriptors(session->device, &descriptors,
                                              &descriptor_length);
            if (status != LIBUSB_SUCCESS)
                close_device(session);
        }
    }

    fprintf(stderr, "open status=%d descriptors=%u\n", status,
            descriptor_length);
    if (send_response(session, request->opcode, status, 0, descriptors,
                      descriptor_length) != 0) {
        free(descriptors);
        return -1;
    }
    free(descriptors);
    return 0;
}

static int handle_close(struct session *session, const struct request *request)
{
    if (request->payload_length != 0 || request->arg0 != 0)
        return -2;
    close_device(session);
    fprintf(stderr, "close status=0\n");
    return send_response(session, request->opcode, LIBUSB_SUCCESS, 0, NULL, 0);
}

static int handle_control(struct session *session,
                          const struct request *request)
{
    uint8_t request_type;
    uint8_t usb_request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    uint32_t timeout_ms;
    uint8_t *data;
    bool direction_in;
    int status;
    uint32_t response_length = 0;

    if (request->arg0 != 0 || request->payload_length < 12)
        return -2;
    request_type = request->payload[0];
    usb_request = request->payload[1];
    value = get_le16(request->payload + 2);
    index = get_le16(request->payload + 4);
    length = get_le16(request->payload + 6);
    timeout_ms = get_le32(request->payload + 8);
    direction_in = (request_type & LIBUSB_ENDPOINT_IN) != 0;

    if ((direction_in && request->payload_length != 12) ||
        (!direction_in && request->payload_length != 12u + length))
        return -2;

    if (session->device == NULL) {
        status = LIBUSB_ERROR_NO_DEVICE;
        fprintf(stderr, "control dir=%s length=%u status=%d\n",
                direction_in ? "in" : "out", (unsigned int)length, status);
        return send_response(session, request->opcode, status, 0, NULL, 0);
    }

    if (direction_in) {
        data = length == 0 ? NULL : malloc(length);
        if (length != 0 && data == NULL)
            status = LIBUSB_ERROR_NO_MEM;
        else
            status = libusb_control_transfer(
                session->device, request_type, usb_request, value, index, data,
                length, clamp_timeout(timeout_ms));
        if (status > 0)
            response_length = (uint32_t)status;
    } else {
        data = request->payload + 12;
        status = libusb_control_transfer(
            session->device, request_type, usb_request, value, index, data,
            length, clamp_timeout(timeout_ms));
    }

    if (trace_transfers || status < 0)
        fprintf(stderr, "control dir=%s length=%u status=%d\n",
            direction_in ? "in" : "out", (unsigned int)length, status);
    if (send_response(session, request->opcode, status, 0,
                      direction_in ? data : NULL, response_length) != 0) {
        if (direction_in)
            free(data);
        return -1;
    }
    if (direction_in)
        free(data);
    return 0;
}

static bool valid_bulk_endpoint(uint32_t endpoint)
{
    return endpoint == APUSB_EP_OUT || endpoint == APUSB_EP_IN;
}

static int handle_bulk(struct session *session, const struct request *request)
{
    uint32_t timeout_ms;
    uint32_t requested_length;
    bool direction_in;
    uint8_t *data;
    int actual_length = 0;
    int status;
    uint32_t response_length = 0;

    if (!valid_bulk_endpoint(request->arg0) || request->payload_length < 8)
        return -2;
    timeout_ms = get_le32(request->payload);
    requested_length = get_le32(request->payload + 4);
    direction_in = request->arg0 == APUSB_EP_IN;

    if (requested_length > APUSB_MAX_PAYLOAD ||
        (direction_in && request->payload_length != 8) ||
        (!direction_in && request->payload_length != 8u + requested_length))
        return -2;

    if (session->device == NULL) {
        status = LIBUSB_ERROR_NO_DEVICE;
        fprintf(stderr, "bulk ep=0x%02x requested=%u actual=0 status=%d\n",
                (unsigned int)request->arg0, requested_length, status);
        return send_response(session, request->opcode, status, 0, NULL, 0);
    }

    if (direction_in) {
        data = requested_length == 0 ? NULL : malloc(requested_length);
        if (requested_length != 0 && data == NULL) {
            status = LIBUSB_ERROR_NO_MEM;
        } else {
            status = libusb_bulk_transfer(
                session->device, (unsigned char)request->arg0, data,
                (int)requested_length, &actual_length,
                clamp_timeout(timeout_ms));
        }
        if (actual_length > 0)
            response_length = (uint32_t)actual_length;
    } else {
        data = request->payload + 8;
        status = libusb_bulk_transfer(
            session->device, (unsigned char)request->arg0, data,
            (int)requested_length, &actual_length, clamp_timeout(timeout_ms));
    }

    if (trace_transfers || status < 0)
        fprintf(stderr, "bulk ep=0x%02x requested=%u actual=%d status=%d\n",
            (unsigned int)request->arg0, requested_length, actual_length,
            status);
    if (send_response(session, request->opcode, status,
                      actual_length > 0 ? (uint32_t)actual_length : 0,
                      direction_in ? data : NULL, response_length) != 0) {
        if (direction_in)
            free(data);
        return -1;
    }
    if (direction_in)
        free(data);
    return 0;
}

static int handle_clear_halt(struct session *session,
                             const struct request *request)
{
    int status;

    if (request->payload_length != 0 || !valid_bulk_endpoint(request->arg0))
        return -2;
    if (session->device == NULL)
        status = LIBUSB_ERROR_NO_DEVICE;
    else
        status = libusb_clear_halt(session->device,
                                   (unsigned char)request->arg0);
    fprintf(stderr, "clear_halt ep=0x%02x status=%d\n",
            (unsigned int)request->arg0, status);
    return send_response(session, request->opcode, status, 0, NULL, 0);
}

static int handle_set_alt(struct session *session,
                          const struct request *request)
{
    int status;

    if (request->payload_length != 0 || request->arg0 > INT32_MAX)
        return -2;
    if (session->device == NULL)
        status = LIBUSB_ERROR_NO_DEVICE;
    else
        status = libusb_set_interface_alt_setting(
            session->device, APUSB_INTERFACE, (int)request->arg0);
    fprintf(stderr, "set_alt alt=%u status=%d\n", request->arg0, status);
    return send_response(session, request->opcode, status, 0, NULL, 0);
}

static int dispatch_request(struct session *session,
                            const struct request *request,
                            libusb_context *context, const char *token)
{
    if (!session->authenticated) {
        if (request->opcode != APUSB_OP_AUTH)
            return -2;
        return handle_auth(session, request, token);
    }

    switch (request->opcode) {
    case APUSB_OP_OPEN:
        return handle_open(session, request, context);
    case APUSB_OP_CLOSE:
        return handle_close(session, request);
    case APUSB_OP_CONTROL:
        return handle_control(session, request);
    case APUSB_OP_BULK:
        return handle_bulk(session, request);
    case APUSB_OP_CLEAR_HALT:
        return handle_clear_halt(session, request);
    case APUSB_OP_SET_ALT:
        return handle_set_alt(session, request);
    default:
        return -2;
    }
}

/*
 * An authenticated client may sit idle between operations for an arbitrary
 * length of time.  Once any byte of the next header arrives, read_full still
 * applies the socket's finite receive timeout to the rest of the message.
 */
static int wait_for_authenticated_request(const struct session *session)
{
    fd_set read_fds;
    int result;

    if (!session->authenticated)
        return 0;

    for (;;) {
        FD_ZERO(&read_fds);
        FD_SET(session->fd, &read_fds);
        result = select(session->fd + 1, &read_fds, NULL, NULL, NULL);
        if (result > 0)
            return 0;
        if (result < 0 && errno == EINTR)
            continue;
        return -1;
    }
}

static void serve_client(int client_fd, libusb_context *context,
                         const char *token)
{
    struct session session;

    memset(&session, 0, sizeof(session));
    session.fd = client_fd;
    fprintf(stderr, "client connected\n");

    for (;;) {
        struct request request;
        int result;

        if (wait_for_authenticated_request(&session) != 0) {
            fprintf(stderr, "client wait error: %s\n", strerror(errno));
            break;
        }
        result = receive_request(&session, &request);

        if (result != 1) {
            if (result == -2)
                fprintf(stderr, "client protocol error\n");
            else if (result == -1)
                fprintf(stderr, "client I/O error: %s\n", strerror(errno));
            break;
        }
        result = dispatch_request(&session, &request, context, token);
        free(request.payload);
        if (result != 0) {
            if (result == -2)
                fprintf(stderr, "client invalid request\n");
            break;
        }
    }

    close_device(&session);
    (void)close(client_fd);
    fprintf(stderr, "client disconnected\n");
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > 65535)
        return -1;
    *port = (uint16_t)value;
    return 0;
}

static int create_listener(uint16_t port)
{
    struct sockaddr_in address;
    int listener;
    int enabled = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return -1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0) {
        (void)close(listener);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        (void)close(listener);
        return -1;
    }
    return listener;
}

static int configure_client_socket(int client_fd)
{
    int enabled = 1;
    struct timeval timeout;

    timeout.tv_sec = 35;
    timeout.tv_usec = 0;
    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) != 0 ||
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *token;
    uint16_t port;
    libusb_context *context = NULL;
    int listener;
    int result;

    const char *trace = getenv("APUSB_TRACE");
    trace_transfers = trace != NULL && strcmp(trace, "0") != 0;

    if (argc != 2 || parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "usage: %s PORT\n", argv[0]);
        return EXIT_FAILURE;
    }
    token = getenv("APUSB_TOKEN");
    if (!valid_token(token)) {
        fprintf(stderr,
                "APUSB_TOKEN must be exactly 32 ASCII hexadecimal characters\n");
        return EXIT_FAILURE;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "failed to ignore SIGPIPE: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    result = libusb_init(&context);
    if (result != LIBUSB_SUCCESS) {
        fprintf(stderr, "libusb init failed: %s\n", libusb_error_name(result));
        return EXIT_FAILURE;
    }
    listener = create_listener(port);
    if (listener < 0) {
        fprintf(stderr, "listen on 127.0.0.1:%u failed: %s\n",
                (unsigned int)port, strerror(errno));
        libusb_exit(context);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "listening on 127.0.0.1:%u\n", (unsigned int)port);
    for (;;) {
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "accept failed: %s\n", strerror(errno));
            break;
        }
        if (configure_client_socket(client_fd) != 0) {
            fprintf(stderr, "client socket setup failed: %s\n",
                    strerror(errno));
            (void)close(client_fd);
            continue;
        }
        serve_client(client_fd, context, token);
    }

    (void)close(listener);
    libusb_exit(context);
    return EXIT_FAILURE;
}
