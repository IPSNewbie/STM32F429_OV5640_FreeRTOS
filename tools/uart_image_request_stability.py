"""UART 图像请求长时间稳定性测试与统计工具。"""

import argparse
import csv
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

import serial


DEFAULT_PORT = "COM4"
DEFAULT_BAUD = 115200
DEFAULT_COUNT = 500
DEFAULT_INTERVAL_SECONDS = 0.2
DEFAULT_RESPONSE_TIMEOUT_SECONDS = 10.0
DEFAULT_TAG = "stability"
OUTPUT_DIRECTORY = Path("captures")

START_SEQ = 1
REQUEST_FRAME_SIZE = 14
IMAGE_MAGIC = b"OV56RGB5"
IMAGE_VERSION = 1
IMAGE_PIXEL_FORMAT = 1
IMAGE_WIDTH = 160
IMAGE_HEIGHT = 120
IMAGE_HEADER_SIZE = 22
IMAGE_PAYLOAD_SIZE = 38400
IMAGE_CRC_SIZE = 4
IMAGE_FRAME_SIZE = 38426

ERROR_TYPES = (
    "LENGTH_ERROR",
    "MAGIC_ERROR",
    "HEADER_ERROR",
    "CRC_ERROR",
    "FRAME_ID_GAP",
    "TIMEOUT_OR_EMPTY",
    "SERIAL_ERROR",
)


def parse_arguments():
    """解析长时间稳定性测试命令行参数。"""
    parser = argparse.ArgumentParser(
        description="连续请求OV56RGB5图像帧并统计长时间运行稳定性。"
    )
    parser.add_argument("--port", default=DEFAULT_PORT, help="串口名称，默认COM4。")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="波特率。")
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT, help="请求次数。")
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_INTERVAL_SECONDS,
        help="两次请求之间的间隔秒数。",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_RESPONSE_TIMEOUT_SECONDS,
        help="单帧响应总超时秒数。",
    )
    parser.add_argument("--tag", default=DEFAULT_TAG, help="输出文件标签。")
    return parser.parse_args()


def validate_arguments(args):
    """检查参数范围，避免进入无法完成的测试。"""
    if not args.port:
        raise ValueError("port不能为空。")
    if args.baud <= 0:
        raise ValueError("baud必须大于0。")
    if args.count <= 0:
        raise ValueError("count必须大于0。")
    if args.interval < 0:
        raise ValueError("interval不能小于0。")
    if args.timeout <= 0:
        raise ValueError("timeout必须大于0。")


def make_safe_tag(tag):
    """将tag转换为适合文件名的简单字符串。"""
    safe_tag = "".join(
        character if character.isalnum() or character in "-_" else "_"
        for character in tag
    ).strip("_")
    return safe_tag or DEFAULT_TAG


def build_output_paths(tag, timestamp):
    """构造本次测试使用的三个固定类型输出路径。"""
    safe_tag = make_safe_tag(tag)
    csv_path = OUTPUT_DIRECTORY / f"stability_{safe_tag}_{timestamp}.csv"
    summary_path = OUTPUT_DIRECTORY / f"stability_{safe_tag}_{timestamp}_summary.txt"
    failed_path = (
        OUTPUT_DIRECTORY
        / f"stability_first_failed_response_{safe_tag}_{timestamp}.bin"
    )
    return csv_path, summary_path, failed_path


def build_request(seq):
    """按现有二进制协议构造14 B REQUEST_IMAGE请求。"""
    if not 0 <= seq <= 0xFFFF:
        raise ValueError("请求seq必须在0到65535之间。")

    body = struct.pack("<BBHH", 0x01, 0x20, seq, 0)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    request = b"\xA5\x5A" + body + struct.pack("<I", crc) + b"\x0D\x0A"
    if len(request) != REQUEST_FRAME_SIZE:
        raise ValueError("请求帧长度错误。")
    return request


def read_exact(ser, expected_size, timeout_seconds):
    """循环读取，直到收到目标长度或达到单帧总超时。"""
    response = bytearray()
    deadline = time.monotonic() + timeout_seconds

    while len(response) < expected_size and time.monotonic() < deadline:
        chunk = ser.read(expected_size - len(response))
        if chunk:
            response.extend(chunk)

    return bytes(response)


def validate_response(response, previous_frame_id):
    """校验响应并返回结果字典；每次失败只归入一种主要错误。"""
    result = {
        "passed": False,
        "frame_id": None,
        "crc_expected": None,
        "crc_actual": None,
        "error_type": "",
        "frame_id_valid": False,
    }

    if not response:
        result["error_type"] = "TIMEOUT_OR_EMPTY"
        return result

    if len(response) != IMAGE_FRAME_SIZE:
        result["error_type"] = "LENGTH_ERROR"
        return result

    if response[0:8] != IMAGE_MAGIC:
        result["error_type"] = "MAGIC_ERROR"
        return result

    version = response[8]
    pixel_format = response[9]
    width, height = struct.unpack("<HH", response[10:14])
    payload_len, frame_id = struct.unpack("<II", response[14:22])
    result["frame_id"] = frame_id

    if (
        version != IMAGE_VERSION
        or pixel_format != IMAGE_PIXEL_FORMAT
        or width != IMAGE_WIDTH
        or height != IMAGE_HEIGHT
        or payload_len != IMAGE_PAYLOAD_SIZE
    ):
        result["error_type"] = "HEADER_ERROR"
        return result

    payload = response[IMAGE_HEADER_SIZE:IMAGE_HEADER_SIZE + payload_len]
    crc_offset = IMAGE_HEADER_SIZE + payload_len
    received_crc = struct.unpack("<I", response[crc_offset:crc_offset + 4])[0]
    calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
    result["crc_expected"] = received_crc
    result["crc_actual"] = calculated_crc

    if received_crc != calculated_crc:
        result["error_type"] = "CRC_ERROR"
        return result

    result["frame_id_valid"] = True
    if previous_frame_id is not None:
        expected_frame_id = (previous_frame_id + 1) & 0xFFFFFFFF
        if frame_id != expected_frame_id:
            result["error_type"] = "FRAME_ID_GAP"
            return result

    result["passed"] = True
    return result


def format_crc(value):
    """将CRC转换成固定宽度十六进制；未知时保持空白。"""
    return "" if value is None else f"0x{value:08X}"


def save_first_failed_response(path, response):
    """只保存第一次失败时已经收到的原始字节。"""
    with path.open("wb") as output_file:
        saved_size = output_file.write(response)
    print(f"首次失败响应已保存：{path}")
    print(f"首次失败响应字节数：{saved_size}")


def write_summary(path, summary):
    """生成便于长期归档和人工检查的文本汇总。"""
    lines = [
        f"port={summary['port']}",
        f"baud={summary['baud']}",
        f"count={summary['count']}",
        f"interval={summary['interval']}",
        f"timeout={summary['timeout']}",
        f"start_time={summary['start_time']}",
        f"end_time={summary['end_time']}",
        f"total_duration_seconds={summary['total_duration_seconds']:.3f}",
        f"success_count={summary['success_count']}",
        f"fail_count={summary['fail_count']}",
        f"success_rate={summary['success_rate']:.2f}%",
        f"average_elapsed_ms={summary['average_elapsed_ms']}",
        f"min_elapsed_ms={summary['min_elapsed_ms']}",
        f"max_elapsed_ms={summary['max_elapsed_ms']}",
        f"first_frame_id={summary['first_frame_id']}",
        f"last_frame_id={summary['last_frame_id']}",
        f"frame_id_continuous={summary['frame_id_continuous']}",
        "error_counters:",
    ]
    for error_type in ERROR_TYPES:
        lines.append(f"  {error_type}={summary['error_counters'][error_type]}")
    lines.append(f"final_result={summary['final_result']}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def print_final_summary(summary, csv_path, summary_path, failed_path):
    """在终端输出最终结果和所有生成文件的位置。"""
    print("\n========== 稳定性测试汇总 ==========")
    print(f"请求总数：{summary['count']}")
    print(f"成功次数：{summary['success_count']}")
    print(f"失败次数：{summary['fail_count']}")
    print(f"成功率：{summary['success_rate']:.2f}%")
    print(f"平均耗时：{summary['average_elapsed_ms']} ms")
    print(f"最短耗时：{summary['min_elapsed_ms']} ms")
    print(f"最长耗时：{summary['max_elapsed_ms']} ms")
    print(f"首个frame_id：{summary['first_frame_id']}")
    print(f"最后frame_id：{summary['last_frame_id']}")
    print(f"frame_id连续：{summary['frame_id_continuous']}")
    print("失败分类：")
    for error_type in ERROR_TYPES:
        print(f"  {error_type}={summary['error_counters'][error_type]}")
    print(f"最终结果：{summary['final_result']}")
    print(f"CSV路径：{csv_path}")
    print(f"summary路径：{summary_path}")
    if summary["fail_count"] > 0:
        print(f"首次失败响应路径：{failed_path}")


def main():
    """解析参数并执行完整测试流程。"""
    args = parse_arguments()
    try:
        validate_arguments(args)
    except ValueError as error:
        print(f"参数错误：{error}")
        return 1

    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path, summary_path, failed_path = build_output_paths(args.tag, timestamp)

    error_counters = {error_type: 0 for error_type in ERROR_TYPES}
    elapsed_times = []
    success_count = 0
    fail_count = 0
    previous_frame_id = None
    first_frame_id = None
    last_frame_id = None
    frame_id_continuous = True
    first_failure_saved = False
    start_datetime = datetime.now().astimezone()
    start_monotonic = time.monotonic()

    print(f"串口：{args.port}")
    print(f"波特率：{args.baud}")
    print(f"请求次数：{args.count}")
    print(f"请求间隔：{args.interval}秒")
    print(f"响应超时：{args.timeout}秒")

    ser = serial.Serial()
    serial_opened = False
    try:
        ser.port = args.port
        ser.baudrate = args.baud
        ser.timeout = 0.2
        ser.write_timeout = 2.0
        ser.rtscts = False
        ser.dsrdtr = False
        # open 前关闭控制线，避免 CH340 自动下载电路切换 BOOT0 或复位 MCU。
        ser.dtr = False
        ser.rts = False
        ser.open()
        serial_opened = True

        print(f"DTR状态：{ser.dtr}")
        print(f"RTS状态：{ser.rts}")
        time.sleep(0.2)
        ser.reset_output_buffer()

        with csv_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
            field_names = (
                "index",
                "seq",
                "pass",
                "frame_id",
                "elapsed_ms",
                "rx_len",
                "crc_expected",
                "crc_actual",
                "error_type",
            )
            writer = csv.DictWriter(csv_file, fieldnames=field_names)
            writer.writeheader()

            digits = max(3, len(str(args.count)))
            for request_index in range(1, args.count + 1):
                seq = (START_SEQ + request_index - 1) & 0xFFFF
                request = build_request(seq)
                response = b""
                result = None
                request_start = time.monotonic()

                try:
                    ser.reset_input_buffer()
                    ser.write(request)
                    ser.flush()
                    response = read_exact(ser, IMAGE_FRAME_SIZE, args.timeout)
                    result = validate_response(response, previous_frame_id)
                except (serial.SerialException, OSError) as error:
                    result = {
                        "passed": False,
                        "frame_id": None,
                        "crc_expected": None,
                        "crc_actual": None,
                        "error_type": "SERIAL_ERROR",
                        "frame_id_valid": False,
                    }
                    print(f"串口通信错误：{error}")

                elapsed_ms = (time.monotonic() - request_start) * 1000.0
                elapsed_times.append(elapsed_ms)

                if result["frame_id_valid"]:
                    current_frame_id = result["frame_id"]
                    if first_frame_id is None:
                        first_frame_id = current_frame_id
                    last_frame_id = current_frame_id
                    previous_frame_id = current_frame_id

                if result["passed"]:
                    success_count += 1
                    print(
                        f"[{request_index:0{digits}d}/{args.count:0{digits}d}] "
                        f"seq=0x{seq:04X} frame_id={result['frame_id']} "
                        f"{len(response)} B {elapsed_ms:.1f} ms PASS"
                    )
                else:
                    fail_count += 1
                    error_type = result["error_type"]
                    error_counters[error_type] += 1
                    if error_type == "FRAME_ID_GAP":
                        frame_id_continuous = False
                    print(
                        f"[{request_index:0{digits}d}/{args.count:0{digits}d}] "
                        f"seq=0x{seq:04X} {len(response)} B "
                        f"{elapsed_ms:.1f} ms FAIL {error_type}"
                    )
                    if not first_failure_saved:
                        first_failure_saved = True
                        try:
                            save_first_failed_response(failed_path, response)
                        except OSError as error:
                            print(f"保存首次失败响应时发生错误：{error}")

                writer.writerow(
                    {
                        "index": request_index,
                        "seq": f"0x{seq:04X}",
                        "pass": "PASS" if result["passed"] else "FAIL",
                        "frame_id": "" if result["frame_id"] is None else result["frame_id"],
                        "elapsed_ms": f"{elapsed_ms:.3f}",
                        "rx_len": len(response),
                        "crc_expected": format_crc(result["crc_expected"]),
                        "crc_actual": format_crc(result["crc_actual"]),
                        "error_type": result["error_type"],
                    }
                )
                csv_file.flush()

                if request_index % 20 == 0:
                    print(
                        f"progress: {request_index}/{args.count}, "
                        f"success={success_count}, fail={fail_count}"
                    )

                if request_index < args.count:
                    time.sleep(args.interval)

    except (serial.SerialException, OSError) as error:
        fail_count += 1
        error_counters["SERIAL_ERROR"] += 1
        print(f"无法打开或使用串口：{error}")
        if not csv_path.exists():
            with csv_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
                writer = csv.writer(csv_file)
                writer.writerow(
                    [
                        "index",
                        "seq",
                        "pass",
                        "frame_id",
                        "elapsed_ms",
                        "rx_len",
                        "crc_expected",
                        "crc_actual",
                        "error_type",
                    ]
                )
        try:
            save_first_failed_response(failed_path, b"")
        except OSError as save_error:
            print(f"保存首次失败响应时发生错误：{save_error}")
    finally:
        if serial_opened and ser.is_open:
            ser.close()

    end_datetime = datetime.now().astimezone()
    total_duration = time.monotonic() - start_monotonic
    success_rate = success_count * 100.0 / args.count

    if elapsed_times:
        average_elapsed_ms = f"{sum(elapsed_times) / len(elapsed_times):.3f}"
        min_elapsed_ms = f"{min(elapsed_times):.3f}"
        max_elapsed_ms = f"{max(elapsed_times):.3f}"
    else:
        average_elapsed_ms = "N/A"
        min_elapsed_ms = "N/A"
        max_elapsed_ms = "N/A"

    if first_frame_id is None:
        frame_id_continuous_text = "N/A"
    else:
        frame_id_continuous_text = "YES" if frame_id_continuous else "NO"

    final_result = (
        "PASS"
        if success_count == args.count and fail_count == 0 and frame_id_continuous
        else "FAIL"
    )
    summary = {
        "port": args.port,
        "baud": args.baud,
        "count": args.count,
        "interval": args.interval,
        "timeout": args.timeout,
        "start_time": start_datetime.isoformat(timespec="seconds"),
        "end_time": end_datetime.isoformat(timespec="seconds"),
        "total_duration_seconds": total_duration,
        "success_count": success_count,
        "fail_count": fail_count,
        "success_rate": success_rate,
        "average_elapsed_ms": average_elapsed_ms,
        "min_elapsed_ms": min_elapsed_ms,
        "max_elapsed_ms": max_elapsed_ms,
        "first_frame_id": "N/A" if first_frame_id is None else first_frame_id,
        "last_frame_id": "N/A" if last_frame_id is None else last_frame_id,
        "frame_id_continuous": frame_id_continuous_text,
        "error_counters": error_counters,
        "final_result": final_result,
    }

    write_summary(summary_path, summary)
    print_final_summary(summary, csv_path, summary_path, failed_path)
    return 0 if final_result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
