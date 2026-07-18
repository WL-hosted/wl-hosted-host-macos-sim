#include "transport_usb.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#define USB_RX_CHUNK_SIZE 512u
#define USB_RX_POLL_TIMEOUT_MS 500u
#define USB_TX_TIMEOUT_MS 2000u
#define USB_OPEN_RETRY_INTERVAL_MS 200u

#define FRAME_HEADER_SIZE 24u
#define FRAME_MAGIC_BYTE0 0x57u
#define FRAME_MAGIC_BYTE1 0x4cu
#define FRAME_PROTOCOL_MAJOR 1u
#define FRAME_FLAGS_MASK 0x03u

struct sim_usb_transport {
    sim_usb_config_t config;
    libusb_context *context;
    libusb_device_handle *handle;
    pthread_t rx_thread;
    atomic_bool running;
    uint8_t rx_buffer[FRAME_HEADER_SIZE + 4096u];
    size_t rx_length;
    uint8_t rx_chunk[USB_RX_CHUNK_SIZE];
};

static void sleep_ms(unsigned milliseconds) {
    usleep((useconds_t)milliseconds * 1000u);
}

static bool header_plausible(const uint8_t *header) {
    return header[0] == FRAME_MAGIC_BYTE0 && header[1] == FRAME_MAGIC_BYTE1 &&
           header[2] == FRAME_PROTOCOL_MAJOR &&
           header[3] == FRAME_HEADER_SIZE &&
           (header[5] & (uint8_t)~FRAME_FLAGS_MASK) == 0u;
}

static uint32_t payload_size_of(const uint8_t *header) {
    return (uint32_t)header[6] | ((uint32_t)header[7] << 8);
}

static void consume(sim_usb_transport_t *transport, size_t count) {
    transport->rx_length -= count;
    if (transport->rx_length != 0u) {
        memmove(
            transport->rx_buffer,
            transport->rx_buffer + count,
            transport->rx_length
        );
    }
}

static void feed(
    sim_usb_transport_t *transport, const uint8_t *data, size_t size
) {
    if (transport->rx_length + size > sizeof(transport->rx_buffer)) {
        /* Peer exceeded the negotiated frame size; resynchronize. */
        transport->rx_length = 0u;
        return;
    }
    memcpy(transport->rx_buffer + transport->rx_length, data, size);
    transport->rx_length += size;

    while (transport->rx_length >= FRAME_HEADER_SIZE) {
        uint32_t frame_size;
        if (!header_plausible(transport->rx_buffer)) {
            consume(transport, 1u);
            continue;
        }
        frame_size = FRAME_HEADER_SIZE + payload_size_of(transport->rx_buffer);
        if (frame_size > transport->config.max_frame_size) {
            consume(transport, 1u);
            continue;
        }
        if (transport->rx_length < frame_size)
            break;
        if (transport->config.on_frame != NULL) {
            transport->config.on_frame(
                transport->config.callback_context,
                transport->rx_buffer,
                frame_size
            );
        }
        consume(transport, frame_size);
    }
}

static void *rx_main(void *argument) {
    sim_usb_transport_t *transport = argument;
    while (atomic_load(&transport->running)) {
        int transferred = 0;
        int status = libusb_bulk_transfer(
            transport->handle,
            transport->config.endpoint_in,
            transport->rx_chunk,
            (int)sizeof(transport->rx_chunk),
            &transferred,
            USB_RX_POLL_TIMEOUT_MS
        );
        if (status == LIBUSB_ERROR_TIMEOUT)
            continue;
        if (status != 0) {
            fprintf(
                stderr,
                "host-sim: usb bulk read failed: %s\n",
                libusb_error_name(status)
            );
            if (atomic_load(&transport->running) &&
                transport->config.on_lost != NULL) {
                transport->config.on_lost(transport->config.callback_context);
            }
            /* The core restarts the transport for recovery; idle until the
             * pending close joins this thread. */
            while (atomic_load(&transport->running))
                sleep_ms(100u);
            break;
        }
        feed(transport, transport->rx_chunk, (size_t)transferred);
    }
    return NULL;
}

static libusb_device_handle *open_matching_device(
    libusb_context *context, uint16_t vendor_id, uint16_t product_id
) {
    libusb_device **devices = NULL;
    libusb_device_handle *handle = NULL;
    ssize_t count = libusb_get_device_list(context, &devices);
    ssize_t index;

    if (count < 0)
        return NULL;
    for (index = 0; index < count; ++index) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[index], &descriptor) != 0)
            continue;
        if (descriptor.idVendor != vendor_id ||
            descriptor.idProduct != product_id)
            continue;
        if (libusb_open(devices[index], &handle) != 0)
            handle = NULL;
        break;
    }
    libusb_free_device_list(devices, 1);
    return handle;
}

int sim_usb_open(
    sim_usb_transport_t **transport, const sim_usb_config_t *config
) {
    sim_usb_transport_t *created;
    uint32_t waited_ms = 0u;
    int thread_status;

    if (transport == NULL || config == NULL || config->max_frame_size == 0u ||
        config->max_frame_size > 4096u + FRAME_HEADER_SIZE)
        return -1;

    created = calloc(1u, sizeof(*created));
    if (created == NULL)
        return -1;
    created->config = *config;

    if (libusb_init(&created->context) != 0) {
        free(created);
        return -1;
    }

    while (created->handle == NULL && waited_ms <= config->open_retry_ms) {
        created->handle = open_matching_device(
            created->context, config->vendor_id, config->product_id
        );
        if (created->handle == NULL) {
            sleep_ms(USB_OPEN_RETRY_INTERVAL_MS);
            waited_ms += USB_OPEN_RETRY_INTERVAL_MS;
        }
    }
    if (created->handle == NULL) {
        fprintf(
            stderr,
            "host-sim: usb device %04x:%04x not found\n",
            config->vendor_id,
            config->product_id
        );
        libusb_exit(created->context);
        free(created);
        return -1;
    }

    if (libusb_kernel_driver_active(
            created->handle, config->interface_number
        ) == 1) {
        (void)libusb_detach_kernel_driver(
            created->handle, config->interface_number
        );
    }
    if (libusb_claim_interface(created->handle, config->interface_number) !=
        0) {
        fprintf(stderr, "host-sim: usb claim interface failed\n");
        libusb_close(created->handle);
        libusb_exit(created->context);
        free(created);
        return -1;
    }

    atomic_store(&created->running, true);
    thread_status = pthread_create(&created->rx_thread, NULL, rx_main, created);
    if (thread_status != 0) {
        atomic_store(&created->running, false);
        libusb_release_interface(created->handle, config->interface_number);
        libusb_close(created->handle);
        libusb_exit(created->context);
        free(created);
        return -1;
    }

    *transport = created;
    return 0;
}

void sim_usb_close(sim_usb_transport_t *transport) {
    if (transport == NULL)
        return;
    atomic_store(&transport->running, false);
    pthread_join(transport->rx_thread, NULL);
    if (transport->handle != NULL) {
        (void)libusb_release_interface(
            transport->handle, transport->config.interface_number
        );
        libusb_close(transport->handle);
    }
    libusb_exit(transport->context);
    free(transport);
}

int sim_usb_write(
    sim_usb_transport_t *transport, const uint8_t *frame, size_t size
) {
    size_t written = 0u;
    if (transport == NULL || transport->handle == NULL)
        return -1;
    while (written < size) {
        int transferred = 0;
        int status = libusb_bulk_transfer(
            transport->handle,
            transport->config.endpoint_out,
            (uint8_t *)(frame + written),
            (int)(size - written),
            &transferred,
            USB_TX_TIMEOUT_MS
        );
        if (status != 0) {
            fprintf(
                stderr,
                "host-sim: usb bulk write failed: %s\n",
                libusb_error_name(status)
            );
            return -1;
        }
        written += (size_t)transferred;
    }
    return 0;
}
