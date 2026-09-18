/* Read-only descriptor probe: no interface claim, reset, or vendor commands. */
#include <libusb.h>
#include <stdio.h>

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device **devices = NULL;
    int status = libusb_init(&ctx);
    if (status < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(status));
        return 1;
    }
    ssize_t count = libusb_get_device_list(ctx, &devices);
    if (count < 0) {
        fprintf(stderr, "enumeration: %s\n", libusb_error_name((int)count));
        libusb_exit(ctx);
        return 1;
    }
    int matches = 0;
    int failed = 0;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor d;
        status = libusb_get_device_descriptor(devices[i], &d);
        if (status < 0) {
            fprintf(stderr, "device descriptor: %s\n", libusb_error_name(status));
            failed = 1;
            continue;
        }
        if (d.idVendor != 0x13e4 || d.idProduct != 0x0003)
            continue;
        ++matches;
        printf("APIB USB %04x:%04x bus=%u address=%u USB=%04x class=%02x configurations=%u\n",
               d.idVendor, d.idProduct, libusb_get_bus_number(devices[i]),
               libusb_get_device_address(devices[i]), d.bcdUSB,
               d.bDeviceClass, d.bNumConfigurations);
        for (unsigned c = 0; c < d.bNumConfigurations; ++c) {
            struct libusb_config_descriptor *cfg = NULL;
            status = libusb_get_config_descriptor(devices[i], (uint8_t)c, &cfg);
            if (status < 0) {
                fprintf(stderr, "configuration %u: %s\n", c, libusb_error_name(status));
                failed = 1;
                continue;
            }
            printf("  configuration=%u interfaces=%u\n", cfg->bConfigurationValue, cfg->bNumInterfaces);
            for (unsigned f = 0; f < cfg->bNumInterfaces; ++f) {
                const struct libusb_interface *iface = &cfg->interface[f];
                for (int a = 0; a < iface->num_altsetting; ++a) {
                    const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
                    printf("    interface=%u alternate=%u class=%02x subclass=%02x protocol=%02x\n",
                           alt->bInterfaceNumber, alt->bAlternateSetting, alt->bInterfaceClass,
                           alt->bInterfaceSubClass, alt->bInterfaceProtocol);
                    for (unsigned e = 0; e < alt->bNumEndpoints; ++e) {
                        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                        static const char *types[] = {"control", "isochronous", "bulk", "interrupt"};
                        printf("      endpoint=0x%02x direction=%s type=%s max_packet=%u interval=%u\n",
                               ep->bEndpointAddress, (ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
                               types[ep->bmAttributes & 3], ep->wMaxPacketSize, ep->bInterval);
                    }
                }
            }
            libusb_free_config_descriptor(cfg);
        }
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(ctx);
    if (!matches)
        puts("No 13e4:0003 adapter visible to macOS. Connect it to the Mac, not the VM, and rerun.");
    /* 2 means absent, not a proven USB failure. */
    return failed ? 1 : matches ? 0 : 2;
}
