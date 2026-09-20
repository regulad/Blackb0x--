//
//  bootkit.c
//  Blackb0x
//
//  checkm8 soft-DFU image boot for AppleTV3,2 (CPID 0x8947 / S5L8947X).
//
//  PROVENANCE
//  ----------
//  Copied and trimmed from NyanSatan's checkm8_bootkit
//  (https://github.com/NyanSatan/checkm8_bootkit) -- specifically the
//  flattened single-file `libbootkit` (libbootkit.c/.h + config.h + payload.h
//  + payload/payload.S) that the original Blackb0x app carried under
//  Blackb0x/Libraries/libbootkit/, which is itself derived from that project.
//  checkm8_bootkit boots an arbitrary iBoot over ipwndfu's custom "exec" USB
//  protocol on 32-bit checkm8 platforms. It declares no license of its own;
//  this is retained under the same research-tool terms as the rest of this
//  project's exploit code (see checkm8.h / SHAtter.h), with attribution.
//
//  WHAT WAS KEPT: exactly what is needed to boot an already-decrypted iBSS on
//  AppleTV3,2 -- dfu_boot() and its helpers (validate_device, construct_command,
//  construct_payload, send_command, send_chunks, get_config), the CPID-0x8947
//  offset table, and the ARM trampoline payload.
//  WHAT WAS DROPPED: the interactive tool (main/batch), logging/ops/protocol
//  split of current upstream, save_command()/the `debug` dump path, and the
//  configs for other SoCs (0x8950/0x8747) this project has no device for.
//
//  WHY THIS EXISTS: the checkm8 payload our exploit installs
//  (checkm8_payload_8947) only executes an uploaded image when it is wrapped
//  in the "exec" usb_command_t followed by this device-specific boot
//  trampoline -- it ignores a raw image. Our earlier port sent AppleTV3,2's
//  iBSS through boot_client() (the AppleTV3,1 raw-upload path), so the iBSS
//  was uploaded but never run and the device fell back to its installed iBoot.
//  dfu_boot() builds the "exec" command the payload actually expects.
//

#include "bootkit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// ---------------------------------------------------------------------------
// Trampoline payload
// ---------------------------------------------------------------------------
//
// Assembled, position-independent ARM Thumb from checkm8_bootkit's
// payload/payload.S (reproduced below for transparency -- the blob is the
// deterministic `clang -mthumb -march=armv7-a` + `llvm-objcopy -O binary`
// output, so it is vendored directly rather than cross-compiled at build
// time). The trailing twelve 0xDEAD000N little-endian words are placeholders
// construct_payload() patches with the device's real iBoot function addresses
// before sending.
//
//   .syntax unified ; .text ; .code 16 ; _start:
//     LDR R0, LOADADDR ; LDR R1, IMAGEADDR ; LDR R2, IMAGESIZE
//     LDR R4, memmove ; BLX R4
//     LDR R4, platform_get_boot_trampoline ; BLX R4 ; MOV R5, R0
//     MOV R0, #0 ; LDR R4, platform_bootprep ; BLX R4
//     LDR R4, usb_quiesce_no_free ; BLX R4
//     LDR R4, timer_stop_all ; BLX R4
//     LDR R4, interrupt_mask_all ; BLX R4
//     LDR R4, clocks_quiesce ; BLX R4
//     LDR R4, enter_critical_section ; BLX R4
//     LDR R4, arch_cpu_quiesce ; BLX R4
//     LDR R0, LOADADDR ; MOV R1,#0 ; MOV R2,#0 ; MOV R3,#0 ; BLX R5
//     loop: B loop
//   LOADADDR .long 0xDEAD0001 ; IMAGEADDR 0xDEAD0002 ; IMAGESIZE 0xDEAD0003
//   memmove 0xDEAD0004 ; platform_get_boot_trampoline 0xDEAD0005
//   platform_bootprep 0xDEAD0006 ; usb_quiesce_no_free 0xDEAD0007
//   interrupt_mask_all 0xDEAD0008 ; timer_stop_all 0xDEAD0009
//   clocks_quiesce 0xDEAD000A ; enter_critical_section 0xDEAD000B
//   arch_cpu_quiesce 0xDEAD000C
static const unsigned char payload[] = {
    0xdf, 0xf8, 0x58, 0x00, 0xdf, 0xf8, 0x58, 0x10, 0xdf, 0xf8, 0x58, 0x20,
    0xdf, 0xf8, 0x58, 0x40, 0xa0, 0x47, 0xdf, 0xf8, 0x58, 0x40, 0xa0, 0x47,
    0x05, 0x46, 0x4f, 0xf0, 0x00, 0x00, 0xdf, 0xf8, 0x50, 0x40, 0xa0, 0x47,
    0xdf, 0xf8, 0x4c, 0x40, 0xa0, 0x47, 0xdf, 0xf8, 0x50, 0x40, 0xa0, 0x47,
    0xdf, 0xf8, 0x44, 0x40, 0xa0, 0x47, 0xdf, 0xf8, 0x48, 0x40, 0xa0, 0x47,
    0xdf, 0xf8, 0x44, 0x40, 0xa0, 0x47, 0xdf, 0xf8, 0x44, 0x40, 0xa0, 0x47,
    0xdf, 0xf8, 0x10, 0x00, 0x4f, 0xf0, 0x00, 0x01, 0x4f, 0xf0, 0x00, 0x02,
    0x4f, 0xf0, 0x00, 0x03, 0xa8, 0x47, 0xfe, 0xe7, 0x01, 0x00, 0xad, 0xde,
    0x02, 0x00, 0xad, 0xde, 0x03, 0x00, 0xad, 0xde, 0x04, 0x00, 0xad, 0xde,
    0x05, 0x00, 0xad, 0xde, 0x06, 0x00, 0xad, 0xde, 0x07, 0x00, 0xad, 0xde,
    0x08, 0x00, 0xad, 0xde, 0x09, 0x00, 0xad, 0xde, 0x0a, 0x00, 0xad, 0xde,
    0x0b, 0x00, 0xad, 0xde, 0x0c, 0x00, 0xad, 0xde};

// ---------------------------------------------------------------------------
// Device config (from checkm8_bootkit's config.h -- only CPID 0x8947 kept)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t cpid;
    const char* platform;
    uint32_t loadaddr;
    uint32_t loadsize;
    uint32_t memmove;
    uint32_t platform_get_boot_trampoline;
    uint32_t platform_bootprep;
    uint32_t usb_quiesce_no_free;
    uint32_t timer_stop_all;
    uint32_t interrupt_mask_all;
    uint32_t clocks_quiesce;
    uint32_t enter_critical_section;
    uint32_t arch_cpu_quiesce;
} config_t;

static const config_t configs[] = {
    {.cpid = 0x8947,
     .platform = "s5l8947x",
     .loadaddr = 0x34000000,
     .loadsize = 0x2C000,
     .memmove = 0x9A3C,
     .platform_get_boot_trampoline = 0x6C74 + 1,
     .platform_bootprep = 0x4EB0 + 1,
     .usb_quiesce_no_free = 0x324C + 1,
     .interrupt_mask_all = 0xD88 + 1,
     .timer_stop_all = 0xA338 + 1,  // nullsub
     .clocks_quiesce = 0x5A88 + 1,
     .enter_critical_section = 0x6054 + 1,
     .arch_cpu_quiesce = 0x6C44 + 1}};

static const config_t* get_config(uint32_t cpid) {
    for (size_t i = 0; i < sizeof(configs) / sizeof(config_t); i++) {
        if (configs[i].cpid == cpid) return &configs[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// "exec" USB command (ipwndfu's custom protocol) + upload
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint32_t magic2;
    uint32_t function;
    uint8_t padding[4];
} usb_command_t;

// 'exec' as a 4-char integer constant, exactly as checkm8_bootkit and the
// checkm8 payload agree on it. (Multi-char constants are implementation-
// defined but stable for the clang/gcc this builds under.)
#define USB_COMMAND_MAGIC 'exec'

#define MAX_PACKET_SIZE 0x800
#define USB_SMALL_TIMEOUT 100
#define USB_TIMEOUT 5000

static size_t min_sz(size_t first, size_t second) { return first < second ? first : second; }

static int send_chunks(irecv_client_t client, unsigned char* command, size_t length) {
    size_t index = 0;
    while (index < length) {
        size_t amount = min_sz(length - index, MAX_PACKET_SIZE);
        if ((size_t)irecv_usb_control_transfer(client, 0x21, 1, 0, 0, command + index, amount, USB_TIMEOUT) !=
            amount)
            return -1;
        index += amount;
    }
    return 0;
}

static int send_command(irecv_client_t client, unsigned char* command, size_t length) {
    unsigned char dummy_data[16];
    memset(&dummy_data, 0x0, sizeof(dummy_data));

    if (send_chunks(client, (unsigned char*)&dummy_data, sizeof(dummy_data)) != 0) {
        printf("bootkit: ERROR: failed to send dummy data\n");
        return -1;
    }

    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, NULL, 0, USB_SMALL_TIMEOUT);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, (unsigned char*)&dummy_data, 6, USB_SMALL_TIMEOUT);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, (unsigned char*)&dummy_data, 6, USB_SMALL_TIMEOUT);

    if (send_chunks(client, command, length) != 0) {
        printf("bootkit: ERROR: failed to send command buffer\n");
        return -1;
    }

    irecv_usb_control_transfer(client, 0xA1, 2, 0xFFFF, 0, (unsigned char*)&dummy_data, 1, USB_TIMEOUT);
    return 0;
}

// ---------------------------------------------------------------------------
// Payload/command construction
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t loadaddr;
    uint32_t imageaddr;
    uint32_t imagesize;
    uint32_t memmove;
    uint32_t platform_get_boot_trampoline;
    uint32_t platform_bootprep;
    uint32_t usb_quiesce_no_free;
    uint32_t interrupt_mask_all;
    uint32_t timer_stop_all;
    uint32_t clocks_quiesce;
    uint32_t enter_critical_section;
    uint32_t arch_cpu_quiesce;
} payload_offsets_t;

static unsigned char* construct_payload(const config_t* config, off_t bootloader_offset,
                                        size_t bootloader_length) {
    unsigned char* payload_copy = malloc(sizeof(payload));
    if (!payload_copy) {
        printf("bootkit: ERROR: out of memory\n");
        return NULL;
    }
    memmove(payload_copy, &payload, sizeof(payload));

    static const uint32_t magic = 0xDEAD0001;
    unsigned char* offset = memmem(payload_copy, sizeof(payload), &magic, sizeof(magic));
    if (!offset || sizeof(payload) - (size_t)(offset - payload_copy) < sizeof(payload_offsets_t)) {
        printf("bootkit: ERROR: improper payload\n");
        free(payload_copy);
        return NULL;
    }

    payload_offsets_t* payload_offsets = (payload_offsets_t*)offset;
    payload_offsets->loadaddr = config->loadaddr;
    payload_offsets->imageaddr = (uint32_t)(config->loadaddr + bootloader_offset);
    payload_offsets->imagesize = (uint32_t)bootloader_length;
    payload_offsets->memmove = config->memmove;
    payload_offsets->platform_get_boot_trampoline = config->platform_get_boot_trampoline;
    payload_offsets->platform_bootprep = config->platform_bootprep;
    payload_offsets->usb_quiesce_no_free = config->usb_quiesce_no_free;
    payload_offsets->interrupt_mask_all = config->interrupt_mask_all;
    payload_offsets->timer_stop_all = config->timer_stop_all;
    payload_offsets->clocks_quiesce = config->clocks_quiesce;
    payload_offsets->enter_critical_section = config->enter_critical_section;
    payload_offsets->arch_cpu_quiesce = config->arch_cpu_quiesce;

    return payload_copy;
}

#define ARM_RESET_VECTOR 0xEA00000E

static int construct_command(irecv_client_t client, const unsigned char* bootloader, size_t bootloader_length,
                             unsigned char** result, size_t* result_length) {
    if (*(const uint32_t*)bootloader != ARM_RESET_VECTOR) {
        printf("bootkit: ERROR: provided bootloader doesn't seem to be an ARM image\n");
        return -1;
    }

    const struct irecv_device_info* info = irecv_get_device_info(client);
    const config_t* config = get_config(info->cpid);
    if (!config) {
        printf("bootkit: ERROR: no config available for CPID:%04X\n", info->cpid);
        return -1;
    }

    size_t command_length = sizeof(payload) + bootloader_length + sizeof(usb_command_t);
    if (command_length > config->loadsize) {
        printf("bootkit: ERROR: resulting command is too big, use a smaller bootloader (at least %lu smaller)\n",
               command_length - config->loadsize);
        return -1;
    }

    unsigned char* buffer = malloc(command_length);
    if (!buffer) {
        printf("bootkit: ERROR: out of memory\n");
        return -1;
    }
    memset(buffer, 0x0, command_length);

    off_t bootloader_offset = sizeof(usb_command_t);
    off_t payload_offset = bootloader_offset + bootloader_length;

    usb_command_t* command = (usb_command_t*)buffer;
    command->magic = USB_COMMAND_MAGIC;
    command->magic2 = USB_COMMAND_MAGIC;
    command->function = (uint32_t)(config->loadaddr + payload_offset + 1);
    memset(&command->padding, 0x0, sizeof(command->padding));

    memmove(buffer + bootloader_offset, bootloader, bootloader_length);

    unsigned char* prepared_payload = construct_payload(config, bootloader_offset, bootloader_length);
    if (!prepared_payload) {
        printf("bootkit: ERROR: failed to construct payload\n");
        free(buffer);
        return -1;
    }
    memmove(buffer + payload_offset, prepared_payload, sizeof(payload));
    free(prepared_payload);

    *result = buffer;
    *result_length = command_length;
    return 0;
}

static int validate_device(irecv_client_t client) {
    const struct irecv_device_info* info = irecv_get_device_info(client);

    int mode;
    if (irecv_get_mode(client, &mode) != IRECV_E_SUCCESS) {
        printf("bootkit: ERROR: failed to get device mode\n");
        return -1;
    }
    if (mode != IRECV_K_DFU_MODE) {
        printf("bootkit: ERROR: non-DFU device found\n");
        return -1;
    }
    if (!info->srtg) {
        printf("bootkit: ERROR: soft-DFU device found\n");
        return -1;
    }
    if (!info->serial_string || !strstr(info->serial_string, "PWND:[checkm8]")) {
        printf("bootkit: ERROR: non-pwned-DFU device found\n");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------
int dfu_boot(irecv_client_t client, const unsigned char* bootloader, size_t bootloader_length) {
    if (validate_device(client) != 0) {
        printf("bootkit: ERROR: device validation failed\n");
        return -1;
    }

    unsigned char* command = NULL;
    size_t command_length = 0;
    if (construct_command(client, bootloader, bootloader_length, &command, &command_length) != 0) {
        printf("bootkit: ERROR: failed to construct command\n");
        return -1;
    }

    int rc = send_command(client, command, command_length);
    free(command);  // upstream leaked this; freed here
    if (rc != 0) {
        printf("bootkit: ERROR: failed to send command\n");
        return -1;
    }
    return 0;
}
