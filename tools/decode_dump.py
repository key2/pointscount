#!/usr/bin/env python3
"""Decode a pointscount --debug dump file.

The dump contains one record per line:
    <epoch_ms> <kind> <base64>     raw bytes received from TikTok
    # <epoch_ms> <free text>       human-readable markers written by pointscount

Kinds:
    ws_frame        raw WebSocket push frame (WebcastPushFrame)
    im_fetch        raw /webcast/im/fetch/ response (ProtoMessageFetchResult)
    msg:<Method>    payload of one decoded webcast message

Usage:
    tools/decode_dump.py dump.log                 # summary: count per kind/method
    tools/decode_dump.py dump.log --gifts         # decode all gift messages
    tools/decode_dump.py dump.log --kind msg:WebcastGiftMessage --raw
                                                  # protoc --decode_raw each record
    tools/decode_dump.py dump.log --extract N out.bin
                                                  # write record N's bytes to a file

--gifts decodes WebcastGiftMessage fields without needing protoc:
    gift_id, repeat_count, repeat_end, user nickname, gift name/type/diamonds.
--raw pipes each selected record through `protoc --decode_raw` (needs protoc,
and proto/tiktok.proto for schema-aware decoding is in the ttlive-cpp submodule).
"""

import argparse
import base64
import collections
import datetime
import subprocess
import sys


def parse_lines(path):
    """Yield (lineno, ts_ms, kind, data) for records; kind=None for comments."""
    with open(path, "rb") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if line.startswith(b"#"):
                yield lineno, None, None, line.decode("utf-8", "replace")
                continue
            parts = line.split(b" ", 2)
            if len(parts) != 3:
                continue
            try:
                ts = int(parts[0])
                data = base64.b64decode(parts[2])
            except Exception:
                continue
            yield lineno, ts, parts[1].decode(), data


# --- minimal protobuf wire decoding -----------------------------------------

def read_varint(buf, i):
    v = 0
    shift = 0
    while i < len(buf):
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, i
        shift += 7
    raise EOFError


def iter_fields(buf):
    """Yield (field_number, wire_type, value) over a protobuf buffer."""
    i = 0
    while i < len(buf):
        tag, i = read_varint(buf, i)
        fnum, wtype = tag >> 3, tag & 7
        if wtype == 0:
            v, i = read_varint(buf, i)
        elif wtype == 1:
            v, i = buf[i:i + 8], i + 8
        elif wtype == 2:
            ln, i = read_varint(buf, i)
            v, i = buf[i:i + ln], i + ln
        elif wtype == 5:
            v, i = buf[i:i + 4], i + 4
        else:
            raise ValueError(f"unsupported wire type {wtype}")
        yield fnum, wtype, v


def field_map(buf):
    m = collections.defaultdict(list)
    try:
        for fnum, _, v in iter_fields(buf):
            m[fnum].append(v)
    except Exception:
        pass
    return m


def decode_gift(payload):
    """Decode the interesting bits of a WebcastGiftMessage."""
    m = field_map(payload)
    out = {
        "gift_id": m.get(2, [0])[0],
        "repeat_count": m.get(5, [0])[0],
        "repeat_end": m.get(9, [0])[0],
    }
    if 7 in m and isinstance(m[7][0], (bytes, bytearray)):   # user
        u = field_map(m[7][0])
        if 3 in u:                                            # nickname
            out["user"] = u[3][0].decode("utf-8", "replace")
        if 38 in u:                                           # display_id
            out["unique_id"] = u[38][0].decode("utf-8", "replace")
    if 15 in m and isinstance(m[15][0], (bytes, bytearray)):  # gift
        g = field_map(m[15][0])
        if 16 in g:
            out["gift_name"] = g[16][0].decode("utf-8", "replace")
        if 11 in g:
            out["gift_type"] = g[11][0]
        if 12 in g:
            out["diamond_count"] = g[12][0]
    return out


def fmt_ts(ts_ms):
    return datetime.datetime.fromtimestamp(ts_ms / 1000).strftime(
        "%Y-%m-%d %H:%M:%S.%f")[:-3]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--kind", help="only records whose kind matches this prefix")
    ap.add_argument("--gifts", action="store_true",
                    help="decode all msg:WebcastGiftMessage records")
    ap.add_argument("--raw", action="store_true",
                    help="pipe selected records through protoc --decode_raw")
    ap.add_argument("--extract", nargs=2, metavar=("LINENO", "OUT"),
                    help="write the raw bytes of record at line LINENO to OUT")
    args = ap.parse_args()

    if args.extract:
        want = int(args.extract[0])
        for lineno, ts, kind, data in parse_lines(args.dump):
            if lineno == want and kind is not None:
                with open(args.extract[1], "wb") as f:
                    f.write(data)
                print(f"wrote {len(data)} bytes ({kind}) to {args.extract[1]}")
                return
        sys.exit(f"no record at line {want}")

    if args.gifts:
        args.kind = "msg:WebcastGiftMessage"

    counts = collections.Counter()
    for lineno, ts, kind, data in parse_lines(args.dump):
        if kind is None:
            if not args.kind and not args.gifts:
                print(data)  # comment line
            continue
        counts[kind] += 1
        if args.kind and not kind.startswith(args.kind):
            continue
        if args.gifts:
            g = decode_gift(data)
            streak = "STREAKING" if g.get("repeat_end", 0) == 0 else "FINAL"
            print(f"{fmt_ts(ts)} line={lineno} {streak} "
                  f"user={g.get('unique_id', '?')} gift_id={g.get('gift_id')} "
                  f"name={g.get('gift_name', '?')!r} x{g.get('repeat_count')} "
                  f"type={g.get('gift_type', '?')} "
                  f"diamonds={g.get('diamond_count', '?')}")
        elif args.raw:
            print(f"---- {fmt_ts(ts)} line={lineno} {kind} ({len(data)} bytes)")
            subprocess.run(["protoc", "--decode_raw"], input=data)
        elif args.kind:
            print(f"{fmt_ts(ts)} line={lineno} {kind} {len(data)} bytes")

    if not args.kind and not args.gifts and not args.extract:
        print("\nrecord counts by kind:")
        for kind, n in counts.most_common():
            print(f"  {n:6d}  {kind}")


if __name__ == "__main__":
    main()
