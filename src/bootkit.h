//
//  bootkit.h
//  Blackb0x
//
//  checkm8 soft-DFU image boot for AppleTV3,2 (CPID 0x8947 / S5L8947X).
//
//  PROVENANCE: copied and trimmed from NyanSatan's checkm8_bootkit
//  (https://github.com/NyanSatan/checkm8_bootkit), which boots an arbitrary
//  iBoot over ipwndfu's custom 'exec' USB protocol on 32-bit checkm8
//  platforms. The exact form here is the flattened single-file `libbootkit`
//  the original Blackb0x app carried (Blackb0x/Libraries/libbootkit/), used
//  on AppleTV3,2 specifically -- see bootkit.c for the full note and what was
//  and wasn't kept. checkm8_bootkit declares no license of its own; retained
//  under the same research-tool terms as this project's other exploit code
//  (checkm8.h / SHAtter.h), with attribution.
//
//  This is the piece our C++ port had been missing: our earlier port routed
//  AppleTV3,2 through boot_client() (the AppleTV3,1 method, a raw image
//  upload), but 3,2's checkm8 payload only executes an image wrapped in the
//  'exec' usb_command_t + a device-specific boot trampoline -- which is what
//  dfu_boot() builds. Without it the iBSS was uploaded but never run.
//

#ifndef BLACKB0X_BOOTKIT_H
#define BLACKB0X_BOOTKIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libirecovery.h>

// Boot a raw, already-decrypted iBSS (an ARM image beginning with the
// 0xEA00000E reset vector -- exactly what patchiBSS() produces for AppleTV3)
// on a checkm8-pwned AppleTV3,2 in DFU mode. Wraps it in the 'exec'
// usb_command_t and the CPID-0x8947 trampoline, then sends it. Returns 0 on
// success, -1 on failure (device validation, unsupported CPID, or USB send).
// Does NOT close `client` -- the caller owns its lifecycle.
int dfu_boot(irecv_client_t client, const unsigned char* bootloader, size_t bootloader_length);

#ifdef __cplusplus
}
#endif

#endif
