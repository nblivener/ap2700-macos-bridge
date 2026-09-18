#ifndef APUSB_PROTOCOL_H
#define APUSB_PROTOCOL_H

#include <stdint.h>

/*
 * APUSB bridge protocol, version 1
 *
 * The transport is a TCP byte stream.  Every message begins with the 24-byte
 * header below, followed immediately by payload_length bytes.  All integer
 * fields, including the USB setup packet's 16-bit fields, are little-endian.
 * Implementations should encode the fields explicitly rather than sending a C
 * structure directly.
 *
 * Requests set status to zero.  Responses copy the request opcode.  Response
 * status is the signed libusb result encoded in the uint32_t field (negative
 * libusb error code on failure).  The maximum payload is 4 MiB.
 *
 * The first request on a connection must be AUTH.  Its payload is exactly the
 * 32 ASCII hexadecimal bytes supplied to the server in APUSB_TOKEN.  A client
 * may then issue:
 *
 *   OPEN       empty payload.  On success the payload is the raw 18-byte USB
 *              device descriptor followed by the complete raw configuration
 *              descriptor for configuration index zero.
 *   CLOSE      empty payload.  Releases interface zero and closes the device.
 *   CONTROL    8-byte USB setup packet, uint32_t timeout_ms, then exactly
 *              wLength bytes for an OUT request.  An IN request has no bytes
 *              after timeout_ms and returns transferred bytes as payload.
 *              status is libusb_control_transfer's return value.
 *   BULK       arg0 is endpoint 0x02 or 0x86.  Payload starts with uint32_t
 *              timeout_ms and uint32_t requested_length.  OUT requests append
 *              exactly requested_length bytes; IN requests append nothing.
 *              Response arg0 is actual_length, including partial data returned
 *              with a timeout; IN data is returned in the response payload.
 *   CLEAR_HALT empty payload; arg0 is endpoint 0x02 or 0x86.
 *   SET_ALT    empty payload; arg0 is the alternate setting for interface zero.
 *
 * A timeout of zero is clamped to a finite value by the server.  Values above
 * APUSB_MAX_TIMEOUT_MS are clamped to that limit.
 */

#define APUSB_MAGIC UINT32_C(0x42555041)
#define APUSB_VERSION UINT32_C(1)
#define APUSB_HEADER_SIZE 24u
#define APUSB_MAX_PAYLOAD (4u * 1024u * 1024u)
#define APUSB_MAX_DESCRIPTOR_SIZE 65535u
#define APUSB_TOKEN_SIZE 32u
#define APUSB_MAX_TIMEOUT_MS 30000u
#define APUSB_DEFAULT_TIMEOUT_MS 5000u

enum apusb_opcode {
    APUSB_OP_AUTH = 1,
    APUSB_OP_OPEN = 2,
    APUSB_OP_CLOSE = 3,
    APUSB_OP_CONTROL = 4,
    APUSB_OP_BULK = 5,
    APUSB_OP_CLEAR_HALT = 6,
    APUSB_OP_SET_ALT = 7
};

struct apusb_header {
    uint32_t magic;
    uint32_t version;
    uint32_t opcode;
    uint32_t status;
    uint32_t arg0;
    uint32_t payload_length;
};

#endif
