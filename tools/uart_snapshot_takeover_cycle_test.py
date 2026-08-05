#!/usr/bin/env python3
import argparse
import csv
import re
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

import serial


REQUEST_MAGIC = b"\xA5\x5A"
REQUEST_VERSION = 0x01
REQUEST_TYPE = 0x20
REQUEST_SIZE = 14

IMAGE_MAGIC = b"OV56RGB5"
IMAGE_VERSION = 1
IMAGE_PIXEL_FORMAT = 1
IMAGE_WIDTH = 160
IMAGE_HEIGHT = 120
IMAGE_HEADER_SIZE = 22
IMAGE_PAYLOAD_SIZE = 38400
IMAGE_CRC_SIZE = 4
IMAGE_FRAME_SIZE = 38426

CSV_FIELDS = [
    "cycle",
    "prepare_result",
    "takeover_enter_result",
    "dump_block_result",
    "binary_block_result",
    "takeover_exit_result",
    "restore_result",
    "restore_binary_result",
    "frame_id",
    "restore_binary_time_ms",
    "error",
]


def parse_args():
    """解析命令行参数。"""
    parser = argparse.ArgumentParser(
        description="多轮验证 SNAPSHOT 与冲突引脚释放/恢复闭环。"
    )
    parser.add_argument("--port", default="COM4", help="串口名称，默认 COM4")
    parser.add_argument("--baud", type=int, default=115200, help="串口波特率")
    parser.add_argument("--cycles", type=int, default=5, help="循环次数")
    parser.add_argument(
        "--guard-timeout",
        type=float,
        default=2.0,
        help="文本响应和 guard 阻止检查超时，单位秒",
    )
    parser.add_argument(
        "--frame-timeout",
        type=float,
        default=10.0,
        help="RESTORE 后图像帧接收超时，单位秒",
    )
    parser.add_argument(
        "--interval", type=float, default=0.2, help="每轮之间的延时，单位秒"
    )
    parser.add_argument(
        "--tag", default="stage11_b10_takeover_cycle", help="输出文件标签"
    )
    args = parser.parse_args()

    if args.baud <= 0:
        parser.error("--baud 必须大于 0")
    if args.cycles <= 0:
        parser.error("--cycles 必须大于 0")
    if args.guard_timeout <= 0.0:
        parser.error("--guard-timeout 必须大于 0")
    if args.frame_timeout <= 0.0:
        parser.error("--frame-timeout 必须大于 0")
    if args.interval < 0.0:
        parser.error("--interval 不能小于 0")

    return args


def sanitize_tag(tag):
    """把标签转换为适合文件名的形式。"""
    safe_tag = re.sub(r"[^A-Za-z0-9_-]+", "_", tag.strip()).strip("_")
    return safe_tag or "stage11_b10_takeover_cycle"


def build_request(seq):
    """按协议构造 14 字节二进制图像请求。"""
    if not 0 <= seq <= 0xFFFF:
        raise ValueError("请求 seq 必须在 0 到 65535 之间。")

    body = struct.pack("<BBHH", REQUEST_VERSION, REQUEST_TYPE, seq, 0)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    request = REQUEST_MAGIC + body + struct.pack("<I", crc) + b"\x0D\x0A"
    if len(request) != REQUEST_SIZE:
        raise ValueError("请求帧长度错误。")
    return request


def open_serial_port(args):
    """在打开串口前固定关闭硬件流控以及 DTR/RTS。"""
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.05
    ser.write_timeout = 2.0
    ser.rtscts = False
    ser.dsrdtr = False
    ser.dtr = False
    ser.rts = False

    try:
        ser.open()
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
    except Exception:
        if ser.is_open:
            ser.close()
        raise

    print(f"串口：{ser.port}")
    print(f"波特率：{ser.baudrate}")
    print(f"cycles：{args.cycles}")
    print(f"DTR 状态：{ser.dtr}")
    print(f"RTS 状态：{ser.rts}")
    return ser


def read_text_response(ser, timeout_seconds):
    """在总超时内读取短文本响应，收到数据后安静 0.2 秒即结束。"""
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds
    last_data_time = None

    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        chunk = ser.read(waiting if waiting > 0 else 1)
        if chunk:
            data.extend(chunk)
            last_data_time = time.monotonic()
            continue
        if last_data_time is not None and time.monotonic() - last_data_time >= 0.2:
            break

    return data.decode("utf-8", errors="ignore")


def send_text_command(ser, command, timeout_seconds):
    """清除旧输入后，以 CRLF 结尾发送文本命令并读取响应。"""
    ser.reset_input_buffer()
    ser.write((command + "\r\n").encode("ascii"))
    ser.flush()
    return read_text_response(ser, timeout_seconds)


def read_for_duration(ser, timeout_seconds):
    """在固定时间内收集串口数据，用于确认 guard 下没有合法图像帧。"""
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        chunk = ser.read(waiting if waiting > 0 else 1)
        if chunk:
            data.extend(chunk)
    return bytes(data)


def validate_image_frame(frame):
    """校验 OV56RGB5 头、尺寸、payload 长度和 CRC。"""
    if len(frame) != IMAGE_FRAME_SIZE:
        return False, None, f"响应长度错误：{len(frame)}/{IMAGE_FRAME_SIZE} B"

    magic = frame[0:8]
    version = frame[8]
    pixel_format = frame[9]
    width, height = struct.unpack("<HH", frame[10:14])
    payload_len, frame_id = struct.unpack("<II", frame[14:22])

    if magic != IMAGE_MAGIC:
        return False, None, "响应 magic 错误"
    if version != IMAGE_VERSION:
        return False, None, "响应 version 错误"
    if pixel_format != IMAGE_PIXEL_FORMAT:
        return False, None, "响应 pixel_format 错误"
    if width != IMAGE_WIDTH or height != IMAGE_HEIGHT:
        return False, None, f"响应图像尺寸错误：{width}x{height}"
    if payload_len != IMAGE_PAYLOAD_SIZE:
        return False, None, f"响应 payload_len 错误：{payload_len}"

    payload_end = IMAGE_HEADER_SIZE + payload_len
    payload = frame[IMAGE_HEADER_SIZE:payload_end]
    if len(payload) != IMAGE_PAYLOAD_SIZE:
        return False, None, "实际 payload 长度错误"

    received_crc = struct.unpack(
        "<I", frame[payload_end:payload_end + IMAGE_CRC_SIZE]
    )[0]
    calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if received_crc != calculated_crc:
        return False, None, (
            f"payload CRC 错误：接收 0x{received_crc:08X}，"
            f"计算 0x{calculated_crc:08X}"
        )

    return True, frame_id, ""


def find_valid_image_frame(data):
    """在 guard 期间收到的数据中查找并校验完整图像帧。"""
    search_start = 0
    while True:
        magic_index = data.find(IMAGE_MAGIC, search_start)
        if magic_index < 0:
            return False, None
        frame_end = magic_index + IMAGE_FRAME_SIZE
        if frame_end <= len(data):
            valid, frame_id, _ = validate_image_frame(data[magic_index:frame_end])
            if valid:
                return True, frame_id
        search_start = magic_index + 1


def read_image_frame(ser, timeout_seconds):
    """在总超时内查找并读取一帧完整 OV56RGB5 响应。"""
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        chunk = ser.read(waiting if waiting > 0 else 1)
        if chunk:
            data.extend(chunk)

        magic_index = data.find(IMAGE_MAGIC)
        if magic_index >= 0:
            if magic_index > 0:
                del data[:magic_index]
            if len(data) >= IMAGE_FRAME_SIZE:
                return bytes(data[:IMAGE_FRAME_SIZE])
        elif len(data) > len(IMAGE_MAGIC) - 1:
            del data[:-(len(IMAGE_MAGIC) - 1)]

    return bytes(data)


def send_binary_request(ser, seq, timeout_seconds):
    """发送一帧二进制请求并在指定时间内接收图像帧。"""
    ser.reset_input_buffer()
    ser.write(build_request(seq))
    ser.flush()
    return read_image_frame(ser, timeout_seconds)


def check_frame_id_continuous(frame_ids):
    """检查 RESTORE 后收到的 frame_id 是否逐帧加一。"""
    if not frame_ids:
        return False
    for index in range(1, len(frame_ids)):
        expected = (frame_ids[index - 1] + 1) & 0xFFFFFFFF
        if frame_ids[index] != expected:
            return False
    return True


def write_csv(csv_path, rows):
    """保存逐轮测试结果。"""
    with csv_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_summary(summary_path, args, stats, final_pass):
    """保存测试参数、统计、连续性和最终结论。"""
    lines = [
        "Stage 11B-10 冲突引脚释放/恢复循环测试汇总",
        "",
        f"port={args.port}",
        f"baud={args.baud}",
        f"cycles={args.cycles}",
        f"guard_timeout={args.guard_timeout}",
        f"frame_timeout={args.frame_timeout}",
        f"interval={args.interval}",
        f"tag={args.tag}",
        "DTR=False",
        "RTS=False",
        "",
        f"cycle_total={stats['cycle_total']}",
        f"prepare_ok_count={stats['prepare_ok_count']}",
        f"takeover_enter_ok_count={stats['takeover_enter_ok_count']}",
        f"text_dump_block_ok_count={stats['text_dump_block_ok_count']}",
        f"binary_block_ok_count={stats['binary_block_ok_count']}",
        f"takeover_exit_ok_count={stats['takeover_exit_ok_count']}",
        f"restore_command_ok_count={stats['restore_command_ok_count']}",
        f"restore_binary_ok_count={stats['restore_binary_ok_count']}",
        f"fail_count={stats['fail_count']}",
        f"first_frame_id={stats['first_frame_id']}",
        f"last_frame_id={stats['last_frame_id']}",
        f"frame_id_continuous={stats['frame_id_continuous']}",
        f"avg_restore_binary_time_ms={stats['avg_restore_binary_time_ms']}",
        f"min_restore_binary_time_ms={stats['min_restore_binary_time_ms']}",
        f"max_restore_binary_time_ms={stats['max_restore_binary_time_ms']}",
        "",
        f"最终结果={'PASS' if final_pass else 'FAIL'}",
        "",
        "说明：guard 状态下二进制请求超时是预期现象。",
        "说明：SD TAKEOVER ENTER 后 PC8/PC9/PC11 应释放成功。",
        "说明：SD TAKEOVER EXIT 后 PC8/PC9/PC11 应恢复为 DCMI AF13。",
        "说明：RESTORE 后二进制请求必须 PASS。",
    ]
    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    args = parse_args()
    args.tag = sanitize_tag(args.tag)
    captures_dir = Path("captures")
    captures_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    prefix = f"snapshot_takeover_cycle_{args.tag}_{timestamp}"
    csv_path = captures_dir / f"{prefix}.csv"
    summary_path = captures_dir / f"{prefix}_summary.txt"

    try:
        ser = open_serial_port(args)
    except (serial.SerialException, OSError) as error:
        print(f"串口打开失败：{error}")
        print(f"无法打开 {args.port}，请确认其他串口工具已经关闭。")
        return 1
    except Exception as error:
        print(f"串口初始化失败：{error}")
        return 1

    rows = []
    prepare_ok_count = 0
    takeover_enter_ok_count = 0
    text_dump_block_ok_count = 0
    binary_block_ok_count = 0
    takeover_exit_ok_count = 0
    restore_command_ok_count = 0
    restore_binary_ok_count = 0
    fail_count = 0
    frame_ids = []
    restore_binary_times = []
    next_seq = 1

    try:
        for cycle_index in range(1, args.cycles + 1):
            errors = []
            frame_id = None
            restore_binary_time_ms = None

            prepare_ok = False
            try:
                response = send_text_command(
                    ser, "SNAPSHOT PREPARE", args.guard_timeout
                )
                response_normal = any(
                    keyword in response
                    for keyword in (
                        "SNAPSHOT PREPARE",
                        "DCMI stop OK",
                        "CAMERA_PAUSED",
                    )
                )
                prepare_ok = response_normal and "DCMI stop OK" in response
                if not prepare_ok:
                    errors.append("PREPARE 未确认 DCMI stop OK")
            except Exception as error:
                errors.append(f"PREPARE 异常：{error}")
            if prepare_ok:
                prepare_ok_count += 1

            takeover_enter_ok = False
            try:
                response = send_text_command(
                    ser, "SD TAKEOVER ENTER", args.guard_timeout
                )
                takeover_enter_ok = "conflict pins released" in response
                if "blocked, run SNAPSHOT PREPARE first" in response:
                    errors.append("TAKEOVER ENTER 被前置条件阻止")
                    takeover_enter_ok = False
                elif not takeover_enter_ok:
                    errors.append("TAKEOVER ENTER 未确认冲突引脚已释放")
            except Exception as error:
                errors.append(f"TAKEOVER ENTER 异常：{error}")
            if takeover_enter_ok:
                takeover_enter_ok_count += 1

            dump_block_ok = False
            try:
                response = send_text_command(ser, "DUMP", args.guard_timeout)
                dump_block_ok = (
                    "DUMP blocked: snapshot software guard active." in response
                )
                if not dump_block_ok:
                    errors.append("文本 DUMP 未确认被 guard 阻止")
            except Exception as error:
                errors.append(f"文本 DUMP 异常：{error}")
            if dump_block_ok:
                text_dump_block_ok_count += 1

            binary_block_ok = False
            try:
                ser.reset_input_buffer()
                ser.write(build_request(next_seq))
                ser.flush()
                next_seq = (next_seq + 1) & 0xFFFF
                guard_response = read_for_duration(ser, args.guard_timeout)
                valid_frame_received, guard_frame_id = find_valid_image_frame(
                    guard_response
                )
                binary_block_ok = not valid_frame_received
                if not binary_block_ok:
                    errors.append(
                        f"guard 状态收到合法图像帧，frame_id={guard_frame_id}"
                    )
            except Exception as error:
                errors.append(f"guard 二进制请求异常：{error}")
            if binary_block_ok:
                binary_block_ok_count += 1

            takeover_exit_ok = False
            try:
                response = send_text_command(
                    ser, "SD TAKEOVER EXIT", args.guard_timeout
                )
                takeover_exit_ok = "conflict pins restored" in response
                if not takeover_exit_ok:
                    errors.append("TAKEOVER EXIT 未确认冲突引脚已恢复")
            except Exception as error:
                errors.append(f"TAKEOVER EXIT 异常：{error}")
            if takeover_exit_ok:
                takeover_exit_ok_count += 1

            restore_ok = False
            try:
                response = send_text_command(
                    ser, "SNAPSHOT RESTORE", args.guard_timeout
                )
                restore_ok = any(
                    keyword in response
                    for keyword in ("SNAPSHOT RESTORE", "RESTORE_DEFERRED")
                )
                if not restore_ok:
                    errors.append("RESTORE 响应缺少预期关键字")
            except Exception as error:
                errors.append(f"RESTORE 异常：{error}")
            if restore_ok:
                restore_command_ok_count += 1

            restore_binary_ok = False
            try:
                start_time = time.monotonic()
                frame = send_binary_request(ser, next_seq, args.frame_timeout)
                restore_binary_time_ms = (time.monotonic() - start_time) * 1000.0
                next_seq = (next_seq + 1) & 0xFFFF
                restore_binary_ok, frame_id, frame_error = validate_image_frame(frame)
                if restore_binary_ok:
                    frame_ids.append(frame_id)
                    restore_binary_times.append(restore_binary_time_ms)
                else:
                    errors.append(f"RESTORE 后二进制请求失败：{frame_error}")
            except Exception as error:
                if restore_binary_time_ms is None:
                    restore_binary_time_ms = 0.0
                errors.append(f"RESTORE 后二进制请求异常：{error}")
            if restore_binary_ok:
                restore_binary_ok_count += 1

            cycle_pass = all(
                (
                    prepare_ok,
                    takeover_enter_ok,
                    dump_block_ok,
                    binary_block_ok,
                    takeover_exit_ok,
                    restore_ok,
                    restore_binary_ok,
                )
            )
            if not cycle_pass:
                fail_count += 1

            frame_id_text = str(frame_id) if frame_id is not None else "-"
            time_text = (
                f"{restore_binary_time_ms:.2f}"
                if restore_binary_time_ms is not None
                else "-"
            )
            print(
                f"[{cycle_index:02d}/{args.cycles:02d}] "
                f"PREPARE={'PASS' if prepare_ok else 'FAIL'} "
                f"TAKEOVER_ENTER={'PASS' if takeover_enter_ok else 'FAIL'} "
                f"DUMP_BLOCK={'PASS' if dump_block_ok else 'FAIL'} "
                f"BINARY_BLOCK={'PASS' if binary_block_ok else 'FAIL'} "
                f"TAKEOVER_EXIT={'PASS' if takeover_exit_ok else 'FAIL'} "
                f"RESTORE={'PASS' if restore_ok else 'FAIL'} "
                f"RESTORE_BINARY={'PASS' if restore_binary_ok else 'FAIL'} "
                f"frame_id={frame_id_text} time={time_text} ms"
            )
            if errors:
                print("  失败原因：" + "；".join(errors))

            rows.append(
                {
                    "cycle": cycle_index,
                    "prepare_result": "PASS" if prepare_ok else "FAIL",
                    "takeover_enter_result": (
                        "PASS" if takeover_enter_ok else "FAIL"
                    ),
                    "dump_block_result": "PASS" if dump_block_ok else "FAIL",
                    "binary_block_result": "PASS" if binary_block_ok else "FAIL",
                    "takeover_exit_result": (
                        "PASS" if takeover_exit_ok else "FAIL"
                    ),
                    "restore_result": "PASS" if restore_ok else "FAIL",
                    "restore_binary_result": (
                        "PASS" if restore_binary_ok else "FAIL"
                    ),
                    "frame_id": "" if frame_id is None else frame_id,
                    "restore_binary_time_ms": (
                        ""
                        if restore_binary_time_ms is None
                        else f"{restore_binary_time_ms:.2f}"
                    ),
                    "error": " | ".join(errors),
                }
            )
            time.sleep(args.interval)
    finally:
        if ser.is_open:
            ser.close()

    frame_id_continuous = check_frame_id_continuous(frame_ids)
    first_frame_id = frame_ids[0] if frame_ids else "无"
    last_frame_id = frame_ids[-1] if frame_ids else "无"
    if restore_binary_times:
        average_ms = sum(restore_binary_times) / len(restore_binary_times)
        minimum_ms = min(restore_binary_times)
        maximum_ms = max(restore_binary_times)
        average_text = f"{average_ms:.2f}"
        minimum_text = f"{minimum_ms:.2f}"
        maximum_text = f"{maximum_ms:.2f}"
    else:
        average_text = "无"
        minimum_text = "无"
        maximum_text = "无"

    final_pass = (
        prepare_ok_count == args.cycles
        and takeover_enter_ok_count == args.cycles
        and text_dump_block_ok_count == args.cycles
        and binary_block_ok_count == args.cycles
        and takeover_exit_ok_count == args.cycles
        and restore_command_ok_count == args.cycles
        and restore_binary_ok_count == args.cycles
        and fail_count == 0
        and frame_id_continuous
    )

    stats = {
        "cycle_total": args.cycles,
        "prepare_ok_count": prepare_ok_count,
        "takeover_enter_ok_count": takeover_enter_ok_count,
        "text_dump_block_ok_count": text_dump_block_ok_count,
        "binary_block_ok_count": binary_block_ok_count,
        "takeover_exit_ok_count": takeover_exit_ok_count,
        "restore_command_ok_count": restore_command_ok_count,
        "restore_binary_ok_count": restore_binary_ok_count,
        "fail_count": fail_count,
        "first_frame_id": first_frame_id,
        "last_frame_id": last_frame_id,
        "frame_id_continuous": "是" if frame_id_continuous else "否",
        "avg_restore_binary_time_ms": average_text,
        "min_restore_binary_time_ms": minimum_text,
        "max_restore_binary_time_ms": maximum_text,
    }

    write_csv(csv_path, rows)
    write_summary(summary_path, args, stats, final_pass)

    print("\n========== 测试汇总 ==========")
    for key, value in stats.items():
        print(f"{key}={value}")
    print(f"CSV：{csv_path}")
    print(f"summary：{summary_path}")
    print(f"测试结果：{'PASS' if final_pass else 'FAIL'}")
    return 0 if final_pass else 1


if __name__ == "__main__":
    sys.exit(main())
