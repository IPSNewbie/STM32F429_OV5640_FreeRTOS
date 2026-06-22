#!/usr/bin/env python3
import argparse
import struct
import time
import zlib

import cv2
import numpy as np
import serial


MAGIC = b"OV56RGB5"
HEADER_FORMAT = "<BBHHII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


def read_exact(port: serial.Serial, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = port.read(size - len(data))
        if not chunk:
            raise TimeoutError(f"serial timeout: received {len(data)} of {size} bytes")
        data.extend(chunk)
    return bytes(data)


def find_magic(port: serial.Serial, timeout_s: float) -> bool:
    window = bytearray()
    deadline = time.monotonic() + timeout_s

    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        port.timeout = min(0.1, max(remaining, 0.0))
        byte = port.read(1)
        if not byte:
            continue

        window.extend(byte)
        if len(window) > len(MAGIC):
            del window[0]
        if bytes(window) == MAGIC:
            return True

    return False


def rgb565_to_bgr(payload: bytes, width: int, height: int) -> np.ndarray:
    pixels = np.frombuffer(payload, dtype="<u2").reshape(height, width).astype(np.uint32)
    red = (((pixels >> 11) & 0x1F) * 255 // 31).astype(np.uint8)
    green = (((pixels >> 5) & 0x3F) * 255 // 63).astype(np.uint8)
    blue = ((pixels & 0x1F) * 255 // 31).astype(np.uint8)
    return np.dstack((blue, green, red))


def main() -> None:
    parser = argparse.ArgumentParser(description="Receive one OV5640 RGB565 frame")
    parser.add_argument("--port", required=True, help="serial port, for example COM5")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    with serial.Serial(
        args.port,
        args.baud,
        timeout=args.timeout,
        write_timeout=3.0,
    ) as port:
        magic_found = False
        for attempt in range(1, 4):
            print(f"Sending DUMP command, attempt {attempt}/3...")
            port.write(b"DUMP\n")
            port.flush()

            if find_magic(port, 3.0):
                magic_found = True
                break

            print("OV56RGB5 not received within 3 seconds")

        if not magic_found:
            raise TimeoutError("OV56RGB5 not received after 3 DUMP attempts")

        port.timeout = args.timeout

        header = read_exact(port, HEADER_SIZE)
        version, pixel_format, width, height, payload_len, frame_id = struct.unpack(
            HEADER_FORMAT, header
        )

        if version != 1 or pixel_format != 1:
            raise ValueError(f"unsupported frame version={version}, format={pixel_format}")
        if payload_len != width * height * 2:
            raise ValueError(
                f"invalid payload length {payload_len}, expected {width * height * 2}"
            )

        payload = read_exact(port, payload_len)
        received_crc = struct.unpack("<I", read_exact(port, 4))[0]
        calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if received_crc != calculated_crc:
            raise ValueError(
                f"CRC32 mismatch: received=0x{received_crc:08X}, "
                f"calculated=0x{calculated_crc:08X}"
            )

    image = rgb565_to_bgr(payload, width, height)
    output_path = "ov5640_dump.png"
    if not cv2.imwrite(output_path, image):
        raise RuntimeError(f"failed to save {output_path}")

    print(
        f"Frame {frame_id}: {width}x{height}, {payload_len} bytes, "
        f"CRC32=0x{calculated_crc:08X}"
    )
    print(f"Saved {output_path}")
    cv2.imshow("OV5640 RGB565", image)
    cv2.waitKey(0)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
