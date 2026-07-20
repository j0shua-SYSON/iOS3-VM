#!/usr/bin/env python3
"""
iOS3-VM - inspect an IPSW without extracting the whole thing.

An IPSW is a ZIP. This lists what is inside, identifies the IMG3 containers by
their real on-disk magic, reads the Restore.plist to confirm the device and
build, and can extract individual members for img3dump to chew on.

Ships no Apple data - the user supplies the IPSW.

Usage:
  python tools/ipsw_explore.py firmware/iPhone1,2_3.1.3_7E18_Restore.ipsw
  python tools/ipsw_explore.py <ipsw> -x <member> -o firmware/out.img3
  python tools/ipsw_explore.py <ipsw> --extract-all-img3 firmware/extracted

Copyright (c) 2026 j0shua-SYSON. MIT licensed.
"""
import argparse
import os
import plistlib
import sys
import zipfile

IMG3_ON_DISK = b"3gmI"          # what a real IMG3 starts with in a hex dump


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f}{unit}" if unit == "B" else f"{n/1:.1f}{unit}"
        n /= 1024.0
    return str(n)


def sniff(zf, info):
    """Peek at a member's first bytes to identify it."""
    try:
        with zf.open(info) as f:
            head = f.read(8)
    except Exception:
        return "?"
    if head[:4] == IMG3_ON_DISK:
        return "IMG3"
    if head[:2] == b"BZ":
        return "bzip2"
    if head[:4] == b"koly" or info.filename.endswith(".dmg"):
        return "DMG"
    if head[:4] == b"bplist"[:4]:
        return "plist"
    if head[:1] == b"<":
        return "xml"
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ipsw")
    ap.add_argument("-x", "--extract", help="member to extract")
    ap.add_argument("-o", "--out", help="output path for -x")
    ap.add_argument("--extract-all-img3", metavar="DIR",
                    help="extract every IMG3 member into DIR")
    args = ap.parse_args()

    if not os.path.isfile(args.ipsw):
        sys.exit(f"not found: {args.ipsw}")

    with zipfile.ZipFile(args.ipsw) as zf:
        names = zf.namelist()

        # Restore.plist tells us exactly which device and build this is.
        for cand in ("Restore.plist", "restore.plist"):
            if cand in names:
                try:
                    pl = plistlib.loads(zf.read(cand))
                except Exception as e:
                    print(f"Restore.plist unreadable: {e}")
                    break
                print("=== Restore.plist ===")
                for k in ("ProductType", "ProductVersion", "ProductBuildVersion",
                          "DeviceClass", "DeviceMap", "SupportedProductTypes"):
                    if k in pl:
                        v = pl[k]
                        if isinstance(v, list) and v and isinstance(v[0], dict):
                            v = f"<{len(v)} entries>"
                        print(f"  {k:22} {v}")
                print()
                break

        print(f"=== {len(names)} members in {os.path.basename(args.ipsw)} ===")
        img3s = []
        for info in sorted(zf.infolist(), key=lambda i: -i.file_size):
            if info.is_dir():
                continue
            kind = sniff(zf, info)
            if kind == "IMG3":
                img3s.append(info.filename)
            print(f"  {info.file_size:>12,}  {kind:<6} {info.filename}")

        print(f"\n=== {len(img3s)} IMG3 container(s) ===")
        for n in img3s:
            print(f"  {n}")
        print("\nRun tools/img3dump on any of these to see whether our parser\n"
              "agrees with the real bytes.")

        if args.extract:
            out = args.out or os.path.basename(args.extract)
            os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
            with zf.open(args.extract) as src, open(out, "wb") as dst:
                dst.write(src.read())
            print(f"\nextracted {args.extract} -> {out}")

        if args.extract_all_img3:
            os.makedirs(args.extract_all_img3, exist_ok=True)
            for n in img3s:
                out = os.path.join(args.extract_all_img3,
                                   n.replace("/", "_"))
                with zf.open(n) as src, open(out, "wb") as dst:
                    dst.write(src.read())
            print(f"\nextracted {len(img3s)} IMG3 file(s) -> {args.extract_all_img3}")


if __name__ == "__main__":
    main()
