#ifndef WLH_HOST_SIM_TRANSPORT_USB_H
#define WLH_HOST_SIM_TRANSPORT_USB_H

#include <stddef.h>
#include <stdint.h>

/*
 * USB bulk transport for the WL-hosted wire protocol (USB binding profile
 * espressif.esp32s3.coreboard.usb-wifi). The byte stream on the bulk
 * endpoints carries raw WL-hosted frames; USB packet boundaries have no
 * frame semantics, so the receiver reassembles frames with the 24-byte
 * frame header (magic, header size, payload size).
 */

typedef struct sim_usb_transport sim_usb_transport_t;

typedef void (*sim_usb_frame_fn)(
    void *context, const uint8_t *frame, size_t size
);
typedef void (*sim_usb_lost_fn)(void *context);

typedef struct sim_usb_config {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t interface_number;
    uint8_t endpoint_out;
    uint8_t endpoint_in;
    size_t max_frame_size;
    /* Total budget for waiting for a matching device to appear. */
    uint32_t open_retry_ms;
    sim_usb_frame_fn on_frame;
    sim_usb_lost_fn on_lost;
    void *callback_context;
} sim_usb_config_t;

/* Opens the first device matching vendor_id:product_id, claims the
 * interface and starts the receive thread. Blocks up to open_retry_ms
 * waiting for the device. Returns 0 on success. */
int sim_usb_open(
    sim_usb_transport_t **transport, const sim_usb_config_t *config
);

/* Stops the receive thread, releases the interface and closes the device. */
void sim_usb_close(sim_usb_transport_t *transport);

/* Blocking bulk write of one complete frame. Returns 0 on success. */
int sim_usb_write(
    sim_usb_transport_t *transport, const uint8_t *frame, size_t size
);

#endif
