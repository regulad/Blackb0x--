#!/usr/bin/env python3
"""
fetch_firmware_component.py — pull a single component out of a remote IPSW
without downloading the whole (often 500MB-2GB+) archive, decrypt it, and
optionally extract one file from the result.

Consolidates everything learned doing this by hand once:
  - IPSWs are ordinary ZIPs; a small HTTP-Range-request-backed file object
    lets Python's zipfile module read just the central directory + the one
    member we want, no full download needed.
  - BuildManifest.plist's component keys don't always match ipsw.me's/this
    project's own naming casing (e.g. "RestoreRamDisk", not "RestoreRamdisk")
    — resolved case-insensitively here.
  - Blackb0x/ImageKeys/<device>/<device>_<buildID>.keys stores each
    component's [IV, KEY] in that ORDER (array index 0 = IV, index 1 = KEY)
    — this is easy to get backwards (we did, once) since both are just hex
    strings with no label. Matches src/IPSW.cpp's own parsing.
  - Components are IMG3-wrapped: xpwntool needs two passes — one with
    -k/-iv/-decrypt to decrypt the IMG3 container in place, then a second
    plain pass (no flags) to unwrap the IMG3 envelope down to the raw
    payload (a bare HFS+ volume for RestoreRamDisk on this hardware era —
    no UDIF/"koly" wrapper, confirmed via the H+ signature at offset 0x400).
  - That raw HFS+ payload can be listed/extracted from directly via p7zip
    (`7z l`/`7z x`) without ever mounting anything — useful for read-only
    inspection since it needs no loop device / CAP_SYS_ADMIN.

Usage:
    scripts/fetch_firmware_component.py <device> <buildID> <component> [outfile]
    scripts/fetch_firmware_component.py <device> <buildID> <component> \\
        --extract <path-substring> [--extract-out <file>]

Examples:
    # Just get the decrypted, IMG3-unwrapped RestoreRamDisk payload:
    scripts/fetch_firmware_component.py AppleTV2,1 11D257c RestoreRamDisk \\
        /tmp/ramdisk.raw

    # Pull one file out of it directly (needs 7z, no root):
    scripts/fetch_firmware_component.py AppleTV2,1 11D257c RestoreRamDisk \\
        --extract sbin/launchd --extract-out /tmp/real_launchd

Requires: this repo's Blackb0x/ImageKeys/<device>/*.keys for the requested
build, and build/third_party/xpwn/ipsw-patch/xpwntool already built
(`cmake --build build`). --extract additionally needs `7z` on PATH
(`brew install p7zip`). That used to run inside a throwaway
debian:bookworm-slim container under podman, which only ever existed to
supply p7zip without installing it on an immutable Linux host; podman does
not exist on macOS, and 7z is one brew away.
"""

import argparse
import json
import os
import plistlib
import re
import subprocess
import sys
import urllib.request
import zipfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XPWNTOOL = os.path.join(REPO_ROOT, "build", "third_party", "xpwn", "ipsw-patch", "xpwntool")
IMAGEKEYS_DIR = os.path.join(REPO_ROOT, "Blackb0x", "ImageKeys")


class HTTPRangeFile:
    """A minimal read-only, seekable file-like object backed by HTTP Range
    requests — enough for zipfile to read a remote ZIP's central directory
    and a single member without fetching the whole archive."""

    def __init__(self, url):
        self.url = url
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req, timeout=30) as r:
            self.size = int(r.headers["Content-Length"])
        self.pos = 0

    def seek(self, offset, whence=0):
        if whence == 0:
            self.pos = offset
        elif whence == 1:
            self.pos += offset
        elif whence == 2:
            self.pos = self.size + offset
        return self.pos

    def tell(self):
        return self.pos

    def seekable(self):
        return True

    def read(self, n=-1):
        end = (self.size - 1) if n < 0 else (min(self.pos + n, self.size) - 1)
        if end < self.pos:
            return b""
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{end}"})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        self.pos += len(data)
        return data


def firmware_url(device, build_id):
    url = f"https://api.ipsw.me/v4/device/{device}?type=ipsw"
    with urllib.request.urlopen(url, timeout=30) as r:
        data = json.load(r)
    for fw in data.get("firmwares", []):
        if fw.get("buildid") == build_id:
            return fw["url"]
    raise SystemExit(f"No firmware URL found for {device} {build_id} via ipsw.me")


def fetch_zip_member(url, member_predicate, as_bytes=True):
    f = HTTPRangeFile(url)
    zf = zipfile.ZipFile(f)
    for name in zf.namelist():
        if member_predicate(name):
            data = zf.read(name)
            return name, data
    return None, None


def find_component_path(manifest_bytes, component):
    """BuildManifest.plist component keys are case-inconsistent across
    devices/eras (RestoreRamDisk vs RestoreRamdisk, etc.) — match
    case-insensitively and return the first Info/Path string found nested
    under a matching key anywhere in the manifest."""
    root = plistlib.loads(manifest_bytes)
    target = component.lower()

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k.lower() == target and isinstance(v, dict):
                    info = v.get("Info")
                    if isinstance(info, dict) and "Path" in info:
                        return info["Path"]
                found = walk(v)
                if found:
                    return found
        elif isinstance(node, list):
            for item in node:
                found = walk(item)
                if found:
                    return found
        return None

    return walk(root)


def load_key_iv(device, build_id, component):
    """Blackb0x/ImageKeys/<device>/<device>_<buildID>.keys — each component
    is a 2-element array: [IV, KEY], in that order (see src/IPSW.cpp's
    keysForDevice(), which this mirrors exactly)."""
    path = os.path.join(IMAGEKEYS_DIR, device, f"{device}_{build_id}.keys")
    with open(path, "rb") as f:
        root = plistlib.load(f)
    for key, value in root.items():
        if key.lower() == component.lower() and isinstance(value, list) and len(value) >= 2:
            return value[0], value[1]  # iv, key
    raise SystemExit(f"No '{component}' entry in {path}")


def decrypt_component(encrypted_path, key_hex, iv_hex, out_path):
    """Two xpwntool passes: decrypt the IMG3 container, then unwrap it to
    the raw payload. If the component turns out not to be IMG3-wrapped,
    the second pass is a harmless no-op copy (xpwntool just re-emits
    whatever it's given when there's no IMG3 header to parse)."""
    decrypted_img3 = out_path + ".img3"
    subprocess.run(
        [XPWNTOOL, encrypted_path, decrypted_img3, "-k", key_hex, "-iv", iv_hex, "-decrypt"],
        check=True,
    )
    subprocess.run([XPWNTOOL, decrypted_img3, out_path], check=True)
    os.remove(decrypted_img3)


def extract_file(raw_payload_path, path_substring, extract_out):
    """Extract one file from the raw HFS+ payload with 7z — no mount, no
    root. `hdiutil attach` would also work on macOS, but 7z reads the
    volume without asking the kernel to mount an untrusted HFS+ image."""
    workdir = os.path.dirname(os.path.abspath(raw_payload_path))
    basename = os.path.basename(raw_payload_path)
    listing = subprocess.run(
        ["7z", "l", os.path.join(workdir, basename)],
        capture_output=True, text=True, check=True,
    )
    matches = [line for line in listing.stdout.splitlines() if path_substring in line]
    if not matches:
        raise SystemExit(f"No entry matching '{path_substring}' found in {raw_payload_path}")
    # Path is always the last whitespace-separated field in `7z l` output.
    member_path = matches[0].split()[-1]
    print(f"Extracting: {member_path}")

    out_dir = os.path.dirname(os.path.abspath(extract_out)) or "."
    os.makedirs(out_dir, exist_ok=True)
    extract_root = os.path.join(workdir, "_extract_tmp")
    os.makedirs(extract_root, exist_ok=True)
    subprocess.run(
        ["7z", "x", f"-o{extract_root}", os.path.join(workdir, basename), member_path],
        check=True,
    )
    extracted = os.path.join(extract_root, member_path)
    os.rename(extracted, extract_out)
    subprocess.run(["rm", "-rf", extract_root])
    print(f"Wrote {os.path.getsize(extract_out)} bytes to {extract_out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", help='e.g. "AppleTV2,1"')
    ap.add_argument("build_id", help='e.g. "11D257c"')
    ap.add_argument("component", help='BuildManifest.plist key, e.g. "RestoreRamDisk" (case-insensitive)')
    ap.add_argument("outfile", nargs="?", help="Where to write the decrypted, IMG3-unwrapped payload")
    ap.add_argument("--extract", metavar="PATH_SUBSTR", help="Also extract one file matching this path substring")
    ap.add_argument("--extract-out", metavar="FILE", help="Where to write the extracted file (required with --extract)")
    args = ap.parse_args()

    if not os.path.exists(XPWNTOOL):
        raise SystemExit(f"xpwntool not built yet — run `cmake --build build` first (expected at {XPWNTOOL})")
    if args.extract and not args.extract_out:
        raise SystemExit("--extract requires --extract-out")

    outfile = args.outfile or f"/tmp/{args.device}_{args.build_id}_{args.component}.raw"

    print(f"Looking up firmware URL for {args.device} {args.build_id}...")
    url = firmware_url(args.device, args.build_id)
    print(f"  {url}")

    print("Fetching BuildManifest.plist (range request, no full download)...")
    _, manifest_bytes = fetch_zip_member(url, lambda n: n == "BuildManifest.plist")
    if manifest_bytes is None:
        raise SystemExit("BuildManifest.plist not found in IPSW zip")

    component_path = find_component_path(manifest_bytes, args.component)
    if not component_path:
        raise SystemExit(f"Component '{args.component}' not found in BuildManifest.plist")
    print(f"  component file: {component_path}")

    print(f"Fetching {component_path} (range request, no full download)...")
    _, encrypted_bytes = fetch_zip_member(url, lambda n: n == component_path)
    if encrypted_bytes is None:
        raise SystemExit(f"{component_path} not found in IPSW zip")
    encrypted_path = outfile + ".encrypted"
    with open(encrypted_path, "wb") as f:
        f.write(encrypted_bytes)
    print(f"  {len(encrypted_bytes)} bytes")

    print(f"Loading key/iv from Blackb0x/ImageKeys/{args.device}/{args.device}_{args.build_id}.keys...")
    iv_hex, key_hex = load_key_iv(args.device, args.build_id, args.component)

    print("Decrypting (2 xpwntool passes: decrypt, then unwrap IMG3)...")
    decrypt_component(encrypted_path, key_hex, iv_hex, outfile)
    os.remove(encrypted_path)
    print(f"Wrote decrypted payload to {outfile} ({os.path.getsize(outfile)} bytes)")

    if args.extract:
        extract_file(outfile, args.extract, args.extract_out)


if __name__ == "__main__":
    main()
