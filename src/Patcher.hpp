//
//  Patcher.hpp
//  Blackb0x
//
//  CLI port of Patcher.h/.mm. The class is split across two translation
//  units by whether the method decrypts firmware:
//    - Patcher.cpp holds the crypto-free half (baked-component loaders,
//      useStock* which now send Apple's original img3 untouched, the
//      dist/ existence check, and the component bookkeeping). It links no
//      xpwntool decrypt() and is compiled into BOTH binaries.
//    - PatcherPatch.cpp holds patchiBSS/patchiBEC/patchKernel: they
//      decrypt() (xpwntool.c) and fork/exec the two GPL patch tools
//      (iBoot32Patcher, CBPatcher) as separate binaries, and re-encrypt.
//      It is compiled into bake-firmware ONLY, so the blackb0x jailbreak
//      binary does no decryption at all -- it consumes bake-firmware's
//      already-encrypted dist/ output verbatim.
//
//  The ramdisk baking (the substantial decrypt/inject/re-encrypt step) lives
//  in BakeRamdisk.cpp now, also authoring-only. For the record it:
//    1. decrypt()s the ramdisk (Apple's own AES-CBC encryption, via
//       xpwntool.c).
//    2. Detects whether the decrypted image is UDIF-wrapped ("koly" trailer)
//       or already a raw HFS+ partition — confirmed empirically that older
//       (A4-era Apple TV 2/3) restore ramdisks decrypt straight to raw HFS+
//       with no UDIF wrapper at all, unlike the UDIF-wrapped root-filesystem
//       images third_party/xpwn's own ipsw-patch/main.c reference code
//       assumes. Only genuinely UDIF-wrapped images go through xpwn's
//       extractDmg() to unwrap to a raw HFS+ partition image first.
//    3. Builds the volume and injects every payload via `hdiutil`.
//    4. Writes the modified image back out, overwriting the decrypted file
//       in place — rewrapped via buildDmg() only if step 2 found it
//       UDIF-wrapped to begin with; otherwise written as raw bytes, matching
//       what decrypt() actually produced.
//    5. decrypt()s (re-encrypts) exactly as before.
//  Needs root, because step 3 preserves arbitrary file ownership (setuid
//  binaries, etc.).
//
//  HISTORY, because it explains a constraint that still applies: this step
//  used to have a second, Linux implementation that built a fresh volume with
//  `mkfs.hfsplus` and loop-mounted it via the kernel `hfsplus` driver. That
//  was the third design tried there — an in-memory xpwn Volume
//  (add_hfs()/grow_hfs()) and a from-scratch userspace HFS+ writer (libhfsp)
//  were both tried first and both hit real, reproducible data-corrupting bugs
//  under real firmware payloads, and growing the ORIGINAL volume in place was
//  tried and abandoned too. It is gone with Linux support, but the finding
//  that survives is that xpwn's and libhfsp's HFS+ WRITE paths are not
//  trustworthy for this — don't reach for them here. See patchRamdisk()'s own
//  comment in Patcher.cpp for the full investigation.
//
//  Note the loop-mount path was the VERIFIED one: it produced every dist/
//  ramdisk and the 29/29 patch sweep. The hdiutil path above has never run
//  against real hardware. See .claude/TODO.md item 4a.
//

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "IPSW.hpp"

// Whether dist/RestoreRamDisk-<deviceModel>_<buildID>.dmg needs a fresh bake --
// i.e. whether it exists at all. Factored out here so Cli.cpp's
// downloadAndPatchComponents() can ask the same question up front (before a
// device's own Patcher instance has even loaded keys for it) to decide
// whether to kick off a background bake-firmware run.
bool ramdiskBakeNeeded(const std::string& deviceModel, const std::string& buildID);

// ---------------------------------------------------------------------------
// IMG3 output guard -- re-runs iBoot's own validation predicate on what we wrote
// ---------------------------------------------------------------------------
//
// img3ValidateFile() returns true only if `path` is an IMG3 that the real
// bootloader will accept as well-formed; on failure it prints a full
// diagnosis (file, predicate, actual vs expected) to stderr and returns
// false, and the caller MUST fail the bake. img3FileHasMagic() is the cheap
// 4-byte sniff, for callers that must tell "not an IMG3 at all" (a raw
// payload) apart from "a malformed IMG3".
//
// The predicate, its provenance and why it exists are documented in full at
// the definition in Patcher.cpp -- read that before changing either.
//
// Deliberately defined in Patcher.cpp, the crypto-free half of the split
// described at the top of this header: it is pure <cstdio>/<cstdint> (20
// header bytes plus a tag walk), links no xpwn and no GPL decrypt code, and
// so is available to the authoring TUs (PatcherPatch.cpp, Img3Crypt.cpp) and
// to the blackb0x jailbreak binary alike without putting a single new symbol
// on blackb0x's link line. Putting it in an authoring-only TU instead would
// have made the one check that decides whether an image can boot invisible to
// the binary that actually sends images to the device.
bool img3ValidateFile(const std::string& path, const char* context);
bool img3FileHasMagic(const std::string& path);

// ---------------------------------------------------------------------------
// Boot-args. BAKED INTO iBEC, not set at runtime.
// ---------------------------------------------------------------------------
//
// These are shared between the baker (BakeIboot.cpp, which hands them to
// iBoot32Patcher's -b) and the jailbreak binary (Cli.cpp/DeviceManager.cpp,
// which only report which string a given dist/ component was baked with).
// Header-only so no new translation unit -- bake-iboot and blackb0x link
// disjoint sets of sources.
//
// WHY BAKED. `setenv boot-args ...` over the recovery protocol is INERT on
// this hardware. Verified directly by decrypting this project's own baked
// dist/iBEC-AppleTV3,2_10B329a (AES-256-CBC over the IMG3 DATA tag, keys from
// keys/) and searching the plaintext:
//
//   * The string "boot-args" (file offset 0x31c7c, VA 0x9ff31c7c -- the image
//     loads at 0x9ff00000, per iBoot32Patcher's own
//     get_iboot_base_address()) has exactly ONE reference in the entire
//     image: a 32-bit literal at file offset 0x3dd1c. That literal sits in
//     iBoot's settable-env-var NAME TABLE, immediately after "auto-boot" and
//     before "debug-uarts"/"filesize". It is the name of a variable you may
//     set; nothing reads it back. A scan for Thumb-2 MOVW/MOVT.W pairs
//     materializing that address found none either, so there is no
//     literal-pool-free xref hiding elsewhere.
//   * The kernel-boot routine's literal pool at 0x1b190-0x1b1a4 holds, in
//     order: a NUL byte (the "no extra args" empty string), "rd=md0
//     nand-enable-reformat=1 -progress", a second empty string,
//     "is-tethered", "%s force-usb-power=1 " and "%s ". The code at 0x1af42
//     loads the hardcoded restore string, 0x1af46 loads the empty string,
//     0x1af5c/0x1af66 query the "is-tethered" env var, and the two arms are
//     snprintf'd into the kernel command line. env_get("boot-args") never
//     appears on this path.
//
// So on AppleTV3,2's iBoot-1537.9.55 the kernel command line comes from one
// of two compiled-in constants, full stop. That is why iBoot32Patcher's -b
// exists at all: patch_boot_args() overwrites the hardcoded string AND
// repoints the fallback's `LDR Rd, =null_str` at the boot-args xref, so both
// arms of the select yield the injected string.
//
// This also matches the original NSSpiral/Blackb0x, the only configuration
// ever observed to boot on real hardware: Patcher.mm's -patchiBEC:flags:ticket:
// passed its boot-args as the `args` parameter to iBootPatcher() (that
// library's spelling of -b) and produced TWO iBECs from one decrypted input,
// differing ONLY in the baked string. Collapsing that to one iBEC + a runtime
// `setenv` is what silently removed amfi=0xff/cs_enforcement_disable=1 from
// every boot -- which is a complete explanation of why `--stock-ramdisk`
// (Apple's properly-signed launchd) booted while our ad-hoc-signed entrypoint
// as PID 1 did nothing at all.
namespace bootargs {

// ---------------------------------------------------------------------------
// EVERY FLAG BELOW WAS VERIFIED AGAINST THE REAL KERNELS. Read this before
// editing either string; the obvious simplifications are wrong.
// ---------------------------------------------------------------------------
//
// Method, because the conclusions rest on it. Both kernelcaches this project
// targets -- 10B329a (iOS 6.1.3, xnu-2107) and 12H1006 (tvOS 8.4.7) -- were
// decrypted (10B329a: AES-256-CBC over the FULL 16-aligned IMG3 DATA tag
// body, not dataLength; 12H1006 has no KBAG and is plaintext) and
// LZSS-decompressed to exactly the length their complzss headers claim. Flag
// names were then matched as EXACT NUL-terminated C literals, not with
// `strings | grep` -- a name reaches PE_parse_boot_argn() as a standalone
// literal, so a substring hit inside a longer string is not evidence (four
// such false positives were rejected this way). AMFI's literals were
// confirmed to sit inside the __PRELINK_TEXT range __PRELINK_INFO declares
// for com.apple.driver.AppleMobileFileIntegrity, so "absent ⇒ unparseable"
// holds for AMFI args too. Both images are PIC for KASLR, so string
// references are `LDR rX,[pc,#imm]` into a literal pool holding a PC-relative
// DELTA followed by `ADD rX,pc` -- there are no absolute literal words and no
// MOVW/MOVT materializations anywhere. If a future reader finds "no xrefs to
// this string", that is why; the image is not corrupt.
//
// The install path: the kernel roots off the uploaded ramdisk and runs
// entrypoint.c as PID 1.
//
//   rd=md0                    Root device IS the ramdisk. Parsed by XNU
//                             proper (IOFindBSDRoot(), iokit/bsddev/
//                             IOKitBSDInit.cpp); `rd` and `rootdev` are
//                             literal synonyms and `md0` is special-cased to
//                             the in-memory device iBoot registered from
//                             /chosen/memory-map. Without it the kernel
//                             mounts NAND and runs Apple's launchd.
//   -v                        BOOTLOADER-SIDE, not a kernel boot-arg. The
//                             literal `-v` does not exist in EITHER
//                             kernelcache; it exists in the iBEC, as
//                             `-s\0-v\0debug=\0` immediately after
//                             `gBootArgs.commandLine = [%s]`. iBoot scans its
//                             own assembled command line for it and switches
//                             the framebuffer to text-console mode, which is
//                             what puts kernel printf on HDMI. So -v could
//                             only ever have worked through a string iBoot
//                             itself assembles -- i.e. a BAKED one. That is
//                             an independent corroboration of the -b fix
//                             above: over the inert `setenv` channel, -v was
//                             doing nothing either. Kept: this device has no
//                             serial tap and the framebuffer is the only
//                             channel there is.
//   amfi=0xff                 A bitmask read once into a local by AMFI's
//                             _initializeAppleMobileFileIntegrity(), then
//                             TST.W-tested bit by bit. Decoded out of both
//                             builds (10B329a 0x431e82-0x431f64, 12H1006
//                             0x638e30-0x638f24):
//                               0x01  unrestricted task_for_pid
//                                     (== amfi_unrestrict_task_for_pid=1)
//                               0x02  allow any/invalid signature
//                                     (== amfi_allow_any_signature=1)
//                               0x04  12H1006 ONLY: library validation will
//                                     not mark external binaries as platform
//                               0x08/0x10/0x20/0x40  not tested on either
//                                     build -- INERT
//                               0x80  "get out of my way"
//                                     (== amfi_get_out_of_my_way=1)
//                             So 0xff is effectively 0x83 on 10B329a and 0x87
//                             on 12H1006. 0xff is KEPT anyway: narrowing to
//                             0x83 saves zero bytes (both are 9 characters)
//                             and 0xff is the value every period tool used,
//                             so it is the better-tested one. Knowing the
//                             bits is not for shortening the value -- it is
//                             for knowing what 0xff does NOT cover, below.
//   cs_enforcement_disable=1  THE ONE FLAG THAT CANNOT BE FOLDED INTO
//                             amfi=. Do not "simplify" it away on the
//                             reasoning that 0xff already covers everything;
//                             it does not, and deleting it silently
//                             reintroduces exactly the bug the -b fix just
//                             closed. In both builds the amfi mask is
//                             TST.W-tested for 0x01/0x02/0x80 (plus 0x04 on
//                             12H1006), while this block has NO BIT TEST AT
//                             ALL -- its branch comes straight off
//                             PE_parse_boot_argn()'s own return value. It
//                             writes an independent global (0x80483b28 on
//                             10B329a, vs 0x80483b24 for get_out_of_my_way
//                             and 0x80483b20 for allow_any_signature) and
//                             logs "cs_enforcement disabled by boot-arg".
//                             The absence of the TST.W is the finding.
//
//                             Second surprise worth recording, because
//                             reading XNU source alone would mislead you:
//                             this literal occurs EXACTLY ONCE in each
//                             kernelcache and it is in the AMFI kext, not in
//                             the kernel proper. XNU's own copy
//                             (bsd/kern/kern_cs.c, cs_init()) is behind
//                             `#if !SECURE_KERNEL` and is compiled out on
//                             these shipping ARM RELEASE builds -- XNU's
//                             cs_debug/cs_force_kill/cs_force_hard do appear,
//                             but as sysctl names, not boot-arg parses. The
//                             flag works; it just does not work where the
//                             published source says it should, and there is
//                             no XNU-side effect to go looking for.
//   amfi_get_out_of_my_way=1  REDUNDANT AS ANALYSED, AND DELIBERATELY KEPT.
//                             Decoding both kernels shows amfi bit 0x80
//                             writes the IDENTICAL global this flag writes
//                             (0x80483b24 on 10B329a), so with amfi=0xff
//                             already set this is 25 bytes that provably
//                             change nothing -- and the original upstream
//                             tool's ramdisk args omitted it. It stays
//                             anyway, as an owner decision, not an
//                             oversight: we are not length-limited (see the
//                             budget at the bottom -- 81/87 bytes against a
//                             179-byte ceiling), and this project has just
//                             spent weeks on a bug whose entire content was
//                             that the bypass arguments never reached the
//                             kernel at all. Cheap redundancy against our own
//                             disassembly being wrong is worth more here than
//                             a shorter string. If you ever DO need bytes,
//                             this is the one token in either string that is
//                             safe to drop -- and cs_enforcement_disable=1,
//                             which looks equally redundant, is not.
//   pio-error=0               NOT CARGO CULT, and not a logging knob. It is a
//                             real hardware configuration change consumed by
//                             AppleS5L8940XIO::start() (10B329a 0x8d4641,
//                             12H1006 0xaef569; the A4-era twin
//                             AppleS5L8930XIO has the same block). Decoded at
//                             0x8d0cca-0x8d0d4c: the driver stores a DEFAULT
//                             OF 3, calls PE_parse_boot_argn over it, then
//                             TST.W #1 (bit 0 = enable SoC PIO bus-error
//                             reporting) and TST.W #2 (bit 1 = additionally
//                             enable AXI bus-error reporting). So =0 disables
//                             both, and a stray or mistimed MMIO access to an
//                             unmapped / not-yet-clocked peripheral no longer
//                             raises a bus error. That is precisely the
//                             hazard of a ramdisk boot, which brings hardware
//                             up in an order the stock OS never does.
//                             Device relevance was checked, not assumed:
//                             AppleS5L8940XIO's personality has
//                             IONameMatch = arm-io,s5l8940x, and AppleTV3,2's
//                             own decrypted j33iap DeviceTree declares
//                             arm-io,s5l8940x (the only s5l8947x node in the
//                             whole tree is hdmi,s5l8947x). So the S5L8947X
//                             die-shrink A5 in this box is driven by the
//                             generic 8940X IO driver and this flag reaches
//                             it. 12 bytes to remove a class of spurious
//                             panic this project has no way to diagnose.
//                             Keep it.
//
// NOT here: serial=3 / debug-uarts=3 (kernel serial console). They ARE the
// correct way to make the -v log observable, but the AppleTV3,2's UART is on
// internal hardware test-points, not the micro-USB port (which is USB/DFU
// only) -- so with no soldered tap there is nowhere to read that log. See
// docs/HISTORY.md's "Why serial console debugging is not available" note.
//
// DELIBERATELY DEFERRED, not rejected: `debug=0x14e` plus iBoot32Patcher's
// -d. `debug` is present in both kernelcaches, and DB_LOG_PI_SCRN (0x100)
// makes the kernel render PANIC info onto the framebuffer -- a channel -v
// alone does not cover, and the only one this device has. It is gated by
// PE_i_can_has_debugger / the device-tree `debug-enabled` property, which is
// 0 on a production-fused retail unit; iBoot32Patcher's patch_debug_enabled()
// (-d, which patchiBEC() does NOT currently pass) forces it true. It is
// deliberately NOT part of the -b change: the bit meanings are
// community-documented rather than decoded here, the debug-enabled gate is
// inferred rather than observed, and landing a second behavioural change
// alongside the boot-args-channel fix would make a hardware failure
// uninterpretable. Land it on its own, AFTER -b is confirmed on hardware:
// "rd=md0 -v debug=0x14e amfi=0xff ..." plus -d in PatcherPatch.cpp's
// iBECArgs. Recorded here so it is not lost.
//
// Also rejected on evidence, so nobody re-adds them: `-progress` (parsed by
// pexpert on both builds, but it draws a graphics progress bar that -v's text
// console displaces, and nothing drives it without restored);
// `amfi_allow_any_signature=1` / `amfi_unrestrict_task_for_pid=1` (already
// set by amfi bits 0x02 / 0x01); `cs_debug` and `kextlog=` (console-bound
// verbosity, additive to the one channel that is already the bottleneck); and
// `launchdsuffix` / `cs_enforcement_enable` / `cs_process_enforcement` /
// `vnode_cs_enforcement` / `cs_debug_fail_on_unsigned_code` /
// `amfi_allow_research` / `-restore` / `diags`, all of which are ABSENT from
// both kernelcaches entirely. `launchdsuffix` being absent independently
// confirms AGENTS.md's conclusion that no boot-arg can redirect PID 1 away
// from /sbin/launchd.
//
// Notably absent: nand-enable-reformat=1. The reason is NOT that it would
// reformat anything here, but the reasoning this file used to give was
// factually wrong and is corrected: it said the format "is performed by asr
// under restored", so a ramdisk that never starts restored could not be
// affected. The driver's own diagnostic says otherwise --
// "[NAND] %s:%d Unformatted media requires nand-enable-reformat boot-arg" is
// printed by the FTL driver's own start() path, i.e. the FTL driver itself
// performs the low-level flash format once authorized. asr only lays a
// filesystem down on top afterwards. The arg's blast radius is therefore NOT
// contingent on restored running at all.
//
// What IS true, stated carefully rather than reassuringly, because this is
// the owner's only unit and the arg was set on every boot for a while: both
// of the strings that gate it are REFUSALS --
//   "***Unsupported NAND format. Add nand-enable-reformat to your Restore
//    Boot-Args"
//   "[NAND] %s:%d Unformatted media requires nand-enable-reformat boot-arg"
// -- present verbatim in both kernelcaches. Without the arg the driver's
// behaviour is to STOP. The arg does not instruct a format; it removes an
// interlock that is only ever reached when the media is blank or in a layout
// the driver does not recognise. A formatted, bootable unit whose NAND the
// same driver attaches successfully on every ordinary boot never enters that
// path, and iBoot's own compiled-in restore default (at 0x38847 in this
// project's decrypted iBEC) contains the arg, so every stock DFU restore
// Apple ever performed on this model ran with it set. The honest statement
// is: NO HARM OCCURRED AND NO MECHANISM WAS PLAUSIBLE -- not "it could not
// have mattered". The interlock genuinely was open on every one of those
// boots, and the residual risk in principle is the window in which a
// transient failure to read the format signature would have been read as
// unrecognised media with the interlock already disabled. Narrow, and no
// evidence it ever happened, but real.
//
// It is left out because the closest real-world reference for this exact job
// leaves it out: Legacy-iOS-Kit's SSH-ramdisk flow (boot a ramdisk, poke at
// the existing filesystem -- the same shape as what this does) uses no
// reformat arg, while its restore/downgrade flow adds it. SSHRD_Script adds
// it only for CPIDs 0x8960 (A7), 0x7000 and 0x7001 (A8); this chip is 0x8947,
// which is not among them. If entrypoint.c ever boots but cannot see or mount
// the data partition -- and the two strings above are exactly the symptom
// that would justify it -- this is a cheap thing to try before anything
// expensive (a seconds-long
// `bake-iboot --extra-boot-args nand-enable-reformat=1`).
//
// Also absent: anything about a jailbroken userspace; the ramdisk boot is
// only a vehicle for the file writes, so there is nothing further to ask the
// kernel for.
inline constexpr const char* kRamdiskBootArgs =
    "rd=md0 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1 pio-error=0";

// --tether-boot: root off the OS already on NAND (disk0s1s1 is the system
// partition; disk0s1s2 is /var), no ramdisk uploaded. rd= is NOT optional --
// `bootx` from a restore bootloader skips fsboot's automatic NAND-root setup,
// so with no rd= the kernel comes up with no root device and hangs. The
// original app had no rd= at all on this path AND had its two arg sets wired
// to the wrong images; both are corrected here (see docs/HISTORY.md).
inline constexpr const char* kTetherBootArgs =
    "rd=disk0s1s1 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1 pio-error=0";

// The real ceiling on a BAKED boot-args string. This is NOT
// DeviceManager::kMaxRecoveryCommandLength (127) -- that budget belongs to
// iBoot's recovery COMMAND parser, which a baked string never passes through.
//
// THERE IS NO SINGLE CEILING. patch_boot_args() has TWO REGIMES, chosen by
// one comparison in third_party/iBoot32Patcher/patchers.c:
//
//     if (strlen(boot_args) > strlen(DEFAULT_BOOTARGS_STR)) { relocate }
//     strcpy(default_boot_args_str_loc, boot_args);
//
//  A. IN PLACE, at or under 39 bytes. DEFAULT_BOOTARGS_STR is iBoot's own
//     "rd=md0 nand-enable-reformat=1 -progress" -- exactly 39 characters,
//     confirmed at file offset 0x38847 of the decrypted AppleTV3,2 10B329a
//     iBEC. At or under that length the new string is strcpy()'d straight
//     over the old one and NO relocation patch is applied at all. This is the
//     safest regime and nothing useful fits in it: even
//     "rd=md0 amfi=0xff cs_enforcement_disable=1" is 41. (In both regimes the
//     patcher then repoints the `LDR Rd, =null_str` immediate at the
//     boot-args xref, so BOTH arms of iBoot's select yield the injected
//     string. That part is regime-independent.)
//  B. RELOCATED, above 39 bytes. The 32-bit literal-pool word holding the
//     boot-args pointer is rewritten to the address of the "Reliance on this
//     certificate..." string and the strcpy() lands there instead --
//     unbounded, with no length check of its own. In this image that C string
//     is 193 bytes long (file offset 0x3ebf4, re-measured on the real
//     decrypted iBEC), so a write of up to 193 characters provably touches
//     nothing but bytes that were already inside it. The first 179 of those
//     are the pure-ASCII certificate boilerplate ("Reliance on this
//     certificate by any party ... certification practice statements."); the
//     remaining 14 are DER bytes from the embedded Apple root cert that
//     happen to precede the next NUL, and those vary between builds. 179 is
//     therefore the limit that holds for ANY build, not just this one.
//     (A boot-args report circulating alongside this work quotes 178 for the
//     boilerplate; the direct measurement above is 179 and is what this
//     constant uses. The difference is immaterial -- 179 is already the
//     conservative figure, since the real strcpy() has 193+1 bytes to land
//     in on this build.)
//
// A third limit bounds the kernel command line rather than the patch site:
// iBoot snprintf()s the selected string into gBootArgs.commandLine with size
// 0x100 -- confirmed as `MOV.W r1, #0x100` at file offsets 0x1af7a and
// 0x1af9c, the two snprintf calls in the kernel-boot routine -- matching
// XNU's BOOT_LINE_LENGTH of 256 on 32-bit ARM. iBoot appends its own
// " force-usb-power=1 " (19) and " backlight-level=%d " (~22) to whatever it
// selected, leaving ~214. Not binding here.
//
// 179 is the binding one, and it is regime B's. Both truncate silently, so
// bake-iboot refuses an over-long string rather than shortening it. BOTH
// SHIPPED STRINGS ARE IN REGIME B -- 81 (ramdisk) and 87 (tether) bytes,
// each well past 39 -- which is not a problem and not avoidable: no useful
// argument set fits under 39. It leaves 97 bytes for a ramdisk-mode
// `--extra-boot-args` and 91 for tether mode (the limit minus the string
// minus the separating space bake-iboot inserts).
inline constexpr size_t kMaxBakedBootArgsLength = 179;

// dist/ component names for the two baked iBECs. blackb0x picks by mode
// (Cli.cpp's downloadAndPatchComponents()); bake-iboot publishes both.
inline constexpr const char* kIBECComponent = "iBEC";
inline constexpr const char* kIBECTetherComponent = "iBECTether";

} // namespace bootargs

struct PatchedComponents {
    std::optional<std::string> iBSS;
    // The iBEC this run will actually send. On the jailbreak path it is
    // whichever of dist/iBEC-<tuple> / dist/iBECTether-<tuple> matches the
    // mode -- blackb0x never needs both, so only this field is set there.
    std::optional<std::string> iBEC;
    // bake-iboot ONLY: the tether-mode iBEC produced alongside the one above.
    // TWO iBECs exist again, deliberately, and this is the record of why: a
    // baked boot-args string wins unconditionally (see the namespace comment
    // above), so one image cannot serve both rd=md0 and rd=disk0s1s1. The
    // original app produced two for exactly this reason. Never set on the
    // blackb0x side, and checkPatching() does not require it.
    std::optional<std::string> iBECTether;
    std::optional<std::string> kernel;
    std::optional<std::string> ramdisk;
    std::optional<std::string> deviceTree;
    // See setRestoreLogoPath()'s own comment -- unset whenever this
    // build's manifest doesn't list a RestoreLogo component at all.
    std::optional<std::string> restoreLogo;
    // See addLoadedByIBootComponent()'s own comment -- name -> local
    // downloaded path, one entry per IPSW.hpp's own
    // ManifestInfo::loadedByIBootComponents (usually empty).
    std::vector<std::pair<std::string, std::string>> loadedByIBoot;

    // --stock-securerom (Cli.hpp's CliOptions): the matching BuildManifest.plist
    // identity (IPSW.hpp's ManifestInfo::buildIdentity, passed through
    // unchanged) -- sendComponentsToDevice()/DeviceManager::sendiBSS() need
    // this to personalize iBSS with a real TSS-issued SHSH ticket. Unset
    // (nullptr) whenever stockSecurerom isn't in play.
    std::shared_ptr<void> buildIdentity;
    // IPSW.hpp's ManifestInfo::realBuildID, passed through unchanged --
    // personalizeIMG3Component() (Personalize.cpp) needs this alongside
    // buildIdentity above, to check against signedBuildsForDevice()
    // before ever sending a real TSS request.
    std::string buildID;
};

class Patcher {
public:
    Patcher();

    // Called once firmware keys are needed for a given device/build; loads
    // them and (re-)runs any patch step whose input path was already set.
    void loadKeysForDevice(const std::string& deviceID, const std::string& buildID);

    bool patchiBSS(const std::string& path);
    // Produces TWO iBECs from one decrypted input, differing only in the
    // boot-args compiled into each (iBoot32Patcher's -b): `bootArgs` for the
    // install/ramdisk image (outputs_.iBEC) and `tetherBootArgs` for the
    // --tether-boot image (outputs_.iBECTether). See the bootargs namespace
    // above for why the string has to be baked and why one image cannot
    // serve both modes. Both must be non-empty and within
    // bootargs::kMaxBakedBootArgsLength; this refuses otherwise rather than
    // handing iBoot32Patcher something it would truncate into the embedded
    // certificate blob.
    bool patchiBEC(const std::string& path, const std::string& bootArgs,
                   const std::string& tetherBootArgs);
    // uncompressedSizeOverride: DIAGNOSTIC (bake-kernel --uncompressed-size). If
    // nonzero and smaller than the decompressed patched kernel, trim the kernel
    // to that many bytes before re-compressing, so the resulting complzss header
    // reports that (smaller) length_uncompressed self-consistently. Used to test
    // whether iBoot accepts a kernelcache whose declared uncompressed size
    // matches what it actually decodes (dropping trailing padding). 0 = normal.
    bool patchKernel(const std::string& path, const std::string& productVersion,
                     size_t uncompressedSizeOverride = 0);
    // No path parameter (unlike patchiBSS()/patchiBEC()/patchKernel() above)
    // -- this never had one that actually did anything: it only ever looks
    // at dist/RestoreRamDisk-<deviceModel_>_<buildID_>.dmg, built from
    // loadKeysForDevice()/setBuildID()'s own member variables, not from any
    // argument. Cli.cpp's downloadAndPatchComponents() used to download
    // RestoreRamdisk from Apple first and pass its local path in here
    // unused -- that download has always been wasted work on this path and
    // is no longer done at all; see its own comment for what replaced it.
    bool patchRamdisk();

    // --stock-ramdisk (Cli.hpp's CliOptions): sends the RestoreRamdisk
    // exactly as Apple shipped it -- still encrypted, still img3-wrapped, no
    // host-side decryption, no /blackb0x merge, no entrypoint.c -- instead of
    // patchRamdisk()'s usual dist/ lookup. A diagnostic: if the device boots
    // this fine, the failure is in blackb0x's own ramdisk baking; if it fails
    // the same way, the failure is earlier in the chain (iBSS/iBEC patches,
    // kernelcache, devicetree, or the boot trigger itself).
    //
    // Sent untouched regardless of stockRecovery. This is the correction of a
    // wrong belief the earlier code held: that "with blackb0x's own patched
    // iBEC, RSA/ticket checks are bypassed and a decrypted, unwrapped ramdisk
    // is fine." It is not. iBoot32Patcher defeats ONLY iBoot's signature/
    // ticket/KASLR checks -- never its img3 parser or its AES-decrypt path
    // (verified against the patcher source; see docs/HISTORY.md). iBoot always
    // parses the img3 and decrypts the DATA tag itself via the KBAG + hardware
    // GID key, so a host-decrypted, unwrapped image is a headerless payload it
    // cannot load -- which is exactly why the old decrypt-on-stock path never
    // booted. The stockRecovery parameter is now vestigial.
    bool useStockRamdisk(const std::string& path, bool stockRecovery = false);

    // --stock-recovery (Cli.hpp's CliOptions): sends iBSS/iBEC exactly as
    // Apple shipped them -- still encrypted, still img3-wrapped, no
    // iBootPatcher() call, so no KASLR/ticket/RSA-check patches. checkm8/
    // pwnTool still runs beforehand for the device to accept any file at all;
    // this only changes which iBSS/iBEC content is uploaded once pwned.
    // REQUIRES --stock-firmware (Cli.cpp's runCli() refuses otherwise): the
    // resulting stock iBEC enforces real APTicket verification on whatever it
    // loads next, which blackb0x's own patched kernel/ramdisk can never
    // satisfy. If the fully-stock suite boots, the failure is in blackb0x's
    // own iBSS/iBEC patches; if it fails the same way, it is elsewhere.
    //
    // stockSecurerom is now immaterial to what is sent -- either way it is the
    // untouched original img3. There is no host decryption here any more (see
    // useStockRamdisk() above for why decrypting would break the boot). The
    // one thing this cannot produce is a raw, decrypted stock iBSS for the
    // checkm8 boot_client() upload path, which uploads the img3 DATA payload
    // directly as code and so needs plaintext -- producing that requires
    // decryption, which only bake-firmware does now, and a raw iBSS is exactly
    // what the normal jailbreak path already gets from dist/. iBSS being the
    // one component that reaches its loader decrypted is the sole exception to
    // "everything is sent still encrypted"; see Cli.hpp's stockRecovery notes.
    bool useStockIBSS(const std::string& path, bool stockSecurerom = false);
    // Sends the original downloaded iBEC untouched, unconditionally -- same
    // reasoning as useStockRamdisk()/useStockKernel(): whichever iBEC-loader
    // (a stock iBSS's intact RSA check, or a real SecureROM) runs next
    // decrypts the img3 itself, and host-decrypting first would strip the
    // img3 and hand it a payload it cannot load.
    bool useStockIBEC(const std::string& path);

    // --stock-firmware (Cli.hpp's CliOptions): the kernelcache half of the
    // same idea -- sends the kernelcache exactly as Apple shipped it (still
    // encrypted, still img3-wrapped, no host decryption), no patch_kernel()/
    // CBPatcher call (no tfp0, no AMFI/memcmp bypass, no sandbox patch).
    // Combined with useStockIBSS()/useStockIBEC()/useStockRamdisk() this sends
    // a completely unmodified firmware suite end to end -- devicetree is
    // always sent unmodified anyway (see setDeviceTreePath()). If a fully-stock
    // suite boots, checkm8 and the bootx trigger are confirmed and the failure
    // is in one of blackb0x's own patches; if it fails the same way, the
    // failure is somewhere checkm8/the boot trigger doesn't control (or this
    // device/firmware genuinely cannot complete this boot path regardless).
    //
    // Sent untouched regardless of stockRecovery -- iBoot decrypts the img3
    // itself via its KBAG + GID key in every case (see useStockRamdisk()'s own
    // comment). The stockRecovery parameter is now vestigial.
    bool useStockKernel(const std::string& path, bool stockRecovery = false);

    // Feed an already-baked component straight in, with no patching and no
    // decryption. blackb0x uses these for the normal jailbreak path: it
    // consumes bake-firmware's dist/ output rather than producing its own, so
    // it never calls patchiBSS()/patchiBEC()/patchKernel() and therefore never
    // fork/execs iBoot32Patcher or CBPatcher at all. Same shape as
    // setDeviceTreePath() below, which has always worked this way because
    // DeviceTree is sent unmodified.
    //
    // The patch* methods above stay for bake-firmware, which is the only thing
    // that patches now.
    void setBakedIBSSPath(const std::string& path);
    void setBakedIBECPath(const std::string& path);
    void setBakedKernelPath(const std::string& path);
    void setBakedRamdiskPath(const std::string& path);

    void setDeviceTreePath(const std::string& path);

    // Like setDeviceTreePath() above -- sent unmodified, no decrypt/patch
    // step, since it's just an image displayed during Recovery-mode
    // restore, not something iBoot-patchable. Only ever called when
    // IPSW.hpp's ManifestInfo::restoreLogoPath is non-empty (see
    // downloadAndPatchComponents() in Cli.cpp) -- not every build's
    // manifest lists a RestoreLogo component at all.
    void setRestoreLogoPath(const std::string& path);

    // Like setRestoreLogoPath() above -- sent unmodified, no decrypt/
    // patch step. Called once per entry in IPSW.hpp's own
    // ManifestInfo::loadedByIBootComponents (see that field's own
    // comment) -- name must match the manifest's own component key
    // exactly, since DeviceManager::sendStockRestoreTail() sends it
    // alongside the "firmware" command the same way real idevicerestore
    // does, and nothing here validates it further.
    void addLoadedByIBootComponent(const std::string& name, const std::string& path);

    // See PatchedComponents::buildIdentity's own comment.
    void setBuildIdentity(std::shared_ptr<void> identity) { outputs_.buildIdentity = std::move(identity); }
    // See PatchedComponents::buildID's own comment.
    void setBuildID(const std::string& buildID) { outputs_.buildID = buildID; }

    // Read-only view of whatever has been patched so far.
    //
    // Only meaningful for callers that deliberately produce an INCOMPLETE
    // suite: checkPatching() hands a complete set to onComponentsReady and
    // then clears outputs_, so a caller producing everything should use that
    // callback instead. bake-firmware (BakeFirmware.cpp) is the
    // one that needs this -- it patches the bootchain + kernel + devicetree
    // and never touches the ramdisk, so onComponentsReady can never fire for
    // it, and it still needs to know which individual outputs landed.
    const PatchedComponents& components() const { return outputs_; }


    // Called once every component checkPatching() requires is available
    // (replaces MainView's componentsReady:). Fired synchronously from
    // whichever patch*() call completes the last required component.
    std::function<void(const PatchedComponents&)> onComponentsReady;

    // Human-readable names of whichever components checkPatching() is
    // still waiting on -- empty once everything required is present.
    // Mirrors checkPatching()'s own exact requirements, so
    // downloadAndPatchComponents()
    // (Cli.cpp) can report specifically what's missing on failure instead
    // of a blanket "not all components patched successfully" with no
    // detail at all -- a silently-failed download or patch step
    // (wrong/missing keys, a 404 for a build blackb0x has no local
    // support for, etc.) used to surface this way with no indication of
    // which step actually failed or why.
    std::vector<std::string> missingRequiredComponents() const;

private:
    IpswFetch fetcher_;
    std::map<std::string, FirmwareKeyPair> keys_;
    PatchedComponents outputs_;
    // Set by loadKeysForDevice() — patchRamdisk() needs these to compute
    // which dist/RestoreRamDisk-<device>_<buildID>.dmg bake-firmware should
    // already have produced.
    std::string deviceModel_;
    std::string buildID_;

    std::string getRealVersion(const std::string& version) const;
    void checkPatching();
    void clearComponents();

    const FirmwareKeyPair* keyFor(const std::string& imageName) const;
};
