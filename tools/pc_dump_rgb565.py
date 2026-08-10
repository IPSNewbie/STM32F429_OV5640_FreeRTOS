#!/usr/bin/env python3
"""OV5640 RGB565 图像接收、CRC 校验与质量分析工具。"""

import argparse
import csv
import re
import struct
import time
import zlib
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np
import serial


MAGIC = b"OV56RGB5"
HEADER_FORMAT = "<BBHHII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
IMAGE_VERSION = 1
PIXEL_FORMAT_RGB565 = 1
IMAGE_WIDTH = 160
IMAGE_HEIGHT = 120
IMAGE_PAYLOAD_SIZE = IMAGE_WIDTH * IMAGE_HEIGHT * 2
SHARPNESS_BLUR_THRESHOLD = 100.0
CAPTURE_NUMBER_PATTERN = re.compile(r"^(\d+)_")
SUMMARY_FIELDS = [
    "frame_id",
    "timestamp",
    "tag",
    "image_file",
    "mean_brightness",
    "shadow_ratio",
    "highlight_ratio",
    "r_mean",
    "g_mean",
    "b_mean",
    "r_g_ratio",
    "b_g_ratio",
    "b_r_ratio",
    "sharpness",
]


def open_camera_serial_port(port_name: str, baudrate: int, timeout_s: float) -> serial.Serial:
    """
    在明确设置安全的 RTS/DTR 状态后打开串口。

    ATK STM32F429 + CH340 自动下载电路中，RTS 可能影响 BOOT0，
    DTR 可能影响 RESET，因此使用硬件实测安全的 False 状态。

    这里不能直接构造已打开的 Serial 对象，否则来不及先固定控制线状态。
    """
    port = serial.Serial()
    port.port = port_name
    port.baudrate = baudrate
    port.timeout = timeout_s
    port.write_timeout = 2.0
    port.rtscts = False
    port.dsrdtr = False

    # 必须在 open() 前固定控制线：RTS=False 避免进入 ISP，DTR=False 避免复位 MCU。
    port.rts = False
    port.dtr = False

    try:
        port.open()
        print(f"Serial control: DTR={port.dtr} RTS={port.rts}")

        # 留出短暂时间让 MCU 与 USB-UART 控制线稳定。
        time.sleep(0.1)

        # 清除旧启动日志和残留字节，避免干扰下一次 DUMP 帧头搜索。
        port.reset_input_buffer()
        port.reset_output_buffer()
    except Exception:
        if port.is_open:
            port.close()
        raise

    return port


def read_exact(port: serial.Serial, size: int) -> bytes:
    """在超时约束内读取指定字节数。"""
    data = bytearray()
    while len(data) < size:
        chunk = port.read(size - len(data))
        if not chunk:
            raise TimeoutError(f"serial timeout: received {len(data)} of {size} bytes")
        data.extend(chunk)
    return bytes(data)


def find_magic(port: serial.Serial, timeout_s: float) -> bool:
    """在截止时间内滑动查找 OV56RGB5 帧头。"""
    window = bytearray()
    deadline = time.monotonic() + timeout_s

    # 使用滑动窗口重新同步，避免一段残留文本让后续图像帧永久失步。
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
    """将 RGB565 负载转换为 OpenCV BGR 图像。"""
    pixels = np.frombuffer(payload, dtype="<u2").reshape(height, width).astype(np.uint32)
    red = (((pixels >> 11) & 0x1F) * 255 // 31).astype(np.uint8)
    green = (((pixels >> 5) & 0x3F) * 255 // 63).astype(np.uint8)
    blue = ((pixels & 0x1F) * 255 // 31).astype(np.uint8)
    return np.dstack((blue, green, red))


def safe_ratio(numerator: float, denominator: float) -> float:
    """安全计算比值并处理零分母。"""
    if denominator > 0.0:
        return numerator / denominator
    return 1.0 if numerator == 0.0 else float("inf")


def analyze_image(image: np.ndarray) -> tuple[np.ndarray, dict[str, float]]:
    """计算图像亮度、色彩和清晰度指标。"""
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    blue, green, red = cv2.split(image)

    red_mean = float(np.mean(red))
    green_mean = float(np.mean(green))
    blue_mean = float(np.mean(blue))

    metrics = {
        "mean_brightness": float(np.mean(gray)),
        "min_brightness": float(np.min(gray)),
        "max_brightness": float(np.max(gray)),
        "shadow_ratio": float(np.mean(gray < 15) * 100.0),
        "highlight_ratio": float(np.mean(gray > 245) * 100.0),
        "red_mean": red_mean,
        "green_mean": green_mean,
        "blue_mean": blue_mean,
        "red_green_ratio": safe_ratio(red_mean, green_mean),
        "blue_green_ratio": safe_ratio(blue_mean, green_mean),
        "blue_red_ratio": safe_ratio(blue_mean, red_mean),
        "red_blue_ratio": safe_ratio(red_mean, blue_mean),
        "sharpness_score": float(cv2.Laplacian(gray, cv2.CV_64F).var()),
    }
    return gray, metrics


def create_histogram_image(gray: np.ndarray, mean_brightness: float) -> np.ndarray:
    """根据灰度图生成亮度直方图图像。"""
    width = 512
    height = 320
    baseline = height - 35
    plot_height = height - 70
    canvas = np.zeros((height, width, 3), dtype=np.uint8)
    histogram = cv2.calcHist([gray], [0], None, [256], [0, 256]).flatten()

    peak = float(histogram.max())
    if peak > 0.0:
        histogram = histogram / peak * plot_height

    points = np.array(
        [
            [int(index * (width - 1) / 255), baseline - int(value)]
            for index, value in enumerate(histogram)
        ],
        dtype=np.int32,
    )
    cv2.polylines(canvas, [points], False, (255, 255, 255), 1)

    for value, color in ((15, (0, 0, 255)), (245, (0, 0, 255))):
        x = int(value * (width - 1) / 255)
        cv2.line(canvas, (x, 10), (x, baseline), color, 1)

    mean_x = int(mean_brightness * (width - 1) / 255)
    cv2.line(canvas, (mean_x, 10), (mean_x, baseline), (0, 255, 0), 1)
    cv2.putText(
        canvas,
        f"Brightness histogram, mean={mean_brightness:.2f}",
        (10, height - 10),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.5,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )
    return canvas


def sanitize_tag(tag: str) -> str:
    """清洗标签，使其可安全用于文件名。"""
    sanitized = re.sub(r"[^A-Za-z0-9_-]+", "_", tag.strip()).strip("_")
    return sanitized or "capture"


def next_capture_number(captures_dir: Path) -> int:
    """扫描现有文件并生成下一个抓图编号。"""
    highest_number = 0
    for path in captures_dir.iterdir():
        match = CAPTURE_NUMBER_PATTERN.match(path.name)
        if match:
            highest_number = max(highest_number, int(match.group(1)))
    return highest_number + 1


def create_capture_paths(
        captures_dir: Path, tag: str, capture_time: datetime
) -> dict[str, Path]:
    """为本次抓图生成互不冲突的输出文件路径。"""
    capture_number = next_capture_number(captures_dir)
    timestamp = capture_time.strftime("%Y%m%d_%H%M%S")
    prefix = f"{capture_number:03d}_{tag}_{timestamp}"
    return {
        "image": captures_dir / f"{prefix}.png",
        "gray": captures_dir / f"{prefix}_gray.png",
        "histogram": captures_dir / f"{prefix}_hist.png",
        "report": captures_dir / f"{prefix}_report.txt",
    }


def append_summary(
        summary_path: Path,
        frame_id: int,
        timestamp: str,
        tag: str,
        image_file: Path,
        metrics: dict[str, float],
) -> None:
    """将本次图像指标追加到 CSV 汇总文件。"""
    write_header = not summary_path.exists() or summary_path.stat().st_size == 0
    row = {
        "frame_id": frame_id,
        "timestamp": timestamp,
        "tag": tag,
        "image_file": image_file.as_posix(),
        "mean_brightness": f"{metrics['mean_brightness']:.6f}",
        "shadow_ratio": f"{metrics['shadow_ratio']:.6f}",
        "highlight_ratio": f"{metrics['highlight_ratio']:.6f}",
        "r_mean": f"{metrics['red_mean']:.6f}",
        "g_mean": f"{metrics['green_mean']:.6f}",
        "b_mean": f"{metrics['blue_mean']:.6f}",
        "r_g_ratio": f"{metrics['red_green_ratio']:.6f}",
        "b_g_ratio": f"{metrics['blue_green_ratio']:.6f}",
        "b_r_ratio": f"{metrics['blue_red_ratio']:.6f}",
        "sharpness": f"{metrics['sharpness_score']:.6f}",
    }

    with summary_path.open("a", newline="", encoding="utf-8") as summary_file:
        writer = csv.DictWriter(summary_file, fieldnames=SUMMARY_FIELDS)
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def build_report(
        frame_id: int,
        width: int,
        height: int,
        payload_len: int,
        crc32: int,
        timestamp: str,
        tag: str,
        output_files: dict[str, Path],
        metrics: dict[str, float],
) -> str:
    """根据协议字段和图像指标生成质量报告。"""
    warnings = []
    if metrics["highlight_ratio"] > 5.0:
        warnings.append("Possible overexposure: highlight ratio is above 5%.")
    if metrics["shadow_ratio"] > 20.0:
        warnings.append("Possible underexposure: shadow ratio is above 20%.")
    if metrics["blue_red_ratio"] > 1.25:
        warnings.append("Possible blue color cast: B/R ratio is above 1.25.")
    if metrics["red_blue_ratio"] > 1.25:
        warnings.append("Possible red color cast: R/B ratio is above 1.25.")
    if metrics["sharpness_score"] < SHARPNESS_BLUR_THRESHOLD:
        warnings.append(
            f"Possible blur: sharpness score is below {SHARPNESS_BLUR_THRESHOLD:.1f}."
        )
    if not warnings:
        warnings.append("No threshold-based image quality warning.")

    warning_text = "\n".join(f"- {warning}" for warning in warnings)
    return f"""OV5640 RGB565 Image Quality Report
==================================
Frame ID: {frame_id}
Resolution: {width}x{height}
Payload length: {payload_len} bytes
CRC32: 0x{crc32:08X}
Timestamp: {timestamp}
Tag: {tag}

Output files
------------
Image: {output_files['image'].as_posix()}
Gray: {output_files['gray'].as_posix()}
Histogram: {output_files['histogram'].as_posix()}
Report: {output_files['report'].as_posix()}

Brightness
----------
Mean brightness: {metrics['mean_brightness']:.3f}
Min brightness: {metrics['min_brightness']:.0f}
Max brightness: {metrics['max_brightness']:.0f}
Shadow ratio (< 15): {metrics['shadow_ratio']:.3f}%
Highlight ratio (> 245): {metrics['highlight_ratio']:.3f}%

RGB channels
------------
R mean: {metrics['red_mean']:.3f}
G mean: {metrics['green_mean']:.3f}
B mean: {metrics['blue_mean']:.3f}
R/G ratio: {metrics['red_green_ratio']:.3f}
B/G ratio: {metrics['blue_green_ratio']:.3f}
B/R ratio: {metrics['blue_red_ratio']:.3f}
R/B ratio: {metrics['red_blue_ratio']:.3f}

Sharpness
---------
Laplacian variance: {metrics['sharpness_score']:.3f}
Blur threshold: {SHARPNESS_BLUR_THRESHOLD:.1f}

Assessment
----------
{warning_text}
"""


def main() -> None:
    """解析参数并执行完整测试流程。"""
    parser = argparse.ArgumentParser(description="Receive one OV5640 RGB565 frame")
    parser.add_argument("--port", required=True, help="serial port, for example COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--tag", default="capture", help="capture label used in filenames")
    args = parser.parse_args()

    captures_dir = Path("captures")
    captures_dir.mkdir(parents=True, exist_ok=True)
    capture_tag = sanitize_tag(args.tag)

    port = open_camera_serial_port(args.port, args.baud, args.timeout)

    try:
        magic_found = False
        for attempt in range(1, 4):
            print(f"Sending DUMP command, attempt {attempt}/3...")

            # 保留原有每次DUMP尝试前的短间隔，不再切换已配置的控制线。
            time.sleep(0.02)

            # 新请求前丢弃残留数据，确保帧头来自本次 DUMP。
            port.reset_input_buffer()

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

        if version != IMAGE_VERSION or pixel_format != PIXEL_FORMAT_RGB565:
            raise ValueError(f"unsupported frame version={version}, format={pixel_format}")
        if width != IMAGE_WIDTH or height != IMAGE_HEIGHT:
            raise ValueError(
                f"invalid resolution {width}x{height}, "
                f"expected {IMAGE_WIDTH}x{IMAGE_HEIGHT}"
            )
        if payload_len != IMAGE_PAYLOAD_SIZE:
            raise ValueError(
                f"invalid payload length {payload_len}, expected {IMAGE_PAYLOAD_SIZE}"
            )

        payload = read_exact(port, payload_len)
        received_crc = struct.unpack("<I", read_exact(port, 4))[0]
        # CRC32 覆盖完整 RGB565 负载，用于发现串口丢字节、错位或传输损坏。
        calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if received_crc != calculated_crc:
            raise ValueError(
                f"CRC32 mismatch: received=0x{received_crc:08X}, "
                f"calculated=0x{calculated_crc:08X}"
            )

    finally:
        if port.is_open:
            port.close()

    image = rgb565_to_bgr(payload, width, height)
    gray, metrics = analyze_image(image)
    histogram_image = create_histogram_image(gray, metrics["mean_brightness"])
    capture_time = datetime.now()
    capture_timestamp = capture_time.isoformat(timespec="seconds")
    output_files = create_capture_paths(captures_dir, capture_tag, capture_time)
    report = build_report(
        frame_id,
        width,
        height,
        payload_len,
        calculated_crc,
        capture_timestamp,
        capture_tag,
        output_files,
        metrics,
    )

    output_images = {
        output_files["image"]: image,
        output_files["gray"]: gray,
        output_files["histogram"]: histogram_image,
        Path("ov5640_dump.png"): image,
        Path("ov5640_gray.png"): gray,
        Path("ov5640_hist.png"): histogram_image,
    }
    for output_path, output_image in output_images.items():
        if not cv2.imwrite(str(output_path), output_image):
            raise RuntimeError(f"failed to save {output_path}")

    for report_path in (output_files["report"], Path("ov5640_report.txt")):
        report_path.write_text(report, encoding="utf-8")

    append_summary(
        captures_dir / "summary.csv",
        frame_id,
        capture_timestamp,
        capture_tag,
        output_files["image"],
        metrics,
        )

    print(
        f"Frame {frame_id}: {width}x{height}, {payload_len} bytes, "
        f"CRC32=0x{calculated_crc:08X}"
    )
    print(report)
    print(f"Saved capture set: {output_files['image'].parent.as_posix()}")
    print("Updated latest files: ov5640_dump.png, ov5640_gray.png, ov5640_hist.png")
    print("Updated latest report: ov5640_report.txt")
    cv2.imshow("OV5640 RGB565", image)
    cv2.waitKey(0)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
