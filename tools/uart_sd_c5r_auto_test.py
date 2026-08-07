#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 11 SD Snapshot 自动板测脚本

用途：
1. 自动发送 C5R 基线恢复测试所需的串口 CLI 命令。
2. 自动判断第二次 SD INIT 是否 OK。
3. SD INIT OK 时自动执行 SD READTEST 0 / 2048。
4. 自动执行 DUMP guard、SD TAKEOVER EXIT、SNAPSHOT RESTORE、STATUS。
5. 自动发送二进制图像请求，验证 RESTORE 后 OV56RGB5 图像链路是否恢复。
6. 可选重复发送二进制图像请求，验证 frame_id 连续性。

默认参数：COM4、115200、repeat=20、frame_timeout=10s。

注意：
- 本脚本只通过串口发送命令，不会修改固件。
- 打开串口前设置 DTR=False、RTS=False，并禁用 rtscts/dsrdtr，避免打开串口导致板子复位。
"""

from __future__ import annotations

import argparse
import binascii
import datetime as _dt
import os
import re
import struct
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    print("错误：未安装 pyserial，请先执行：pip install pyserial")
    sys.exit(2)


FRAME_MAGIC = b"OV56RGB5"
FRAME_HEADER_LEN = 22
FRAME_DEFAULT_TIMEOUT_S = 10.0
EXPECTED_WIDTH = 160
EXPECTED_HEIGHT = 120
EXPECTED_PAYLOAD_LEN = EXPECTED_WIDTH * EXPECTED_HEIGHT * 2
EXPECTED_TOTAL_LEN = FRAME_HEADER_LEN + EXPECTED_PAYLOAD_LEN + 4

REQ_MAGIC = b"\xA5\x5A"
REQ_VERSION = 0x01
REQ_TYPE_IMAGE = 0x20
REQ_EOF = b"\x0D\x0A"


@dataclass
class FrameResult:
    ok: bool
    error: str = ""
    total_len: int = 0
    frame_id: int = 0
    width: int = 0
    height: int = 0
    payload_len: int = 0
    rx_crc32: int = 0
    calc_crc32: int = 0
    elapsed_ms: float = 0.0


class TestLogger:
    def __init__(self, log_path: str):
        self.log_path = log_path
        self._fp = open(log_path, "w", encoding="utf-8", newline="\n")

    def close(self) -> None:
        self._fp.close()

    def write(self, text: str = "") -> None:
        print(text)
        self._fp.write(text + "\n")
        self._fp.flush()

    def section(self, title: str) -> None:
        line = "=" * 72
        self.write("\n" + line)
        self.write(title)
        self.write(line)

    def command(self, cmd: str, response: str) -> None:
        self.section(f"CLI > {cmd}")
        self.write(response.rstrip() if response else "<无输出>")


_FIELD_RE_CACHE: Dict[str, re.Pattern[str]] = {}


def get_field(text: str, key: str) -> Optional[str]:
    pat = _FIELD_RE_CACHE.get(key)
    if pat is None:
        pat = re.compile(rf"^\s*{re.escape(key)}\s*=\s*(.*?)\s*$", re.MULTILINE)
        _FIELD_RE_CACHE[key] = pat
    matches = pat.findall(text)
    return matches[-1] if matches else None


def get_int_field(text: str, key: str) -> Optional[int]:
    val = get_field(text, key)
    if val is None:
        return None
    val = val.strip()
    try:
        if val.lower().startswith("0x"):
            return int(val, 16)
        return int(val, 10)
    except ValueError:
        return None


def contains_any(text: str, keywords: List[str]) -> bool:
    return any(k in text for k in keywords)


def crc32_u32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def make_image_request(seq: int) -> bytes:
    body = bytes([REQ_VERSION, REQ_TYPE_IMAGE]) + struct.pack("<HH", seq & 0xFFFF, 0)
    crc = crc32_u32(body)
    return REQ_MAGIC + body + struct.pack("<I", crc) + REQ_EOF


def open_serial(port: str, baud: int, timeout_s: float) -> "serial.Serial":
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.write_timeout = 2.0
    ser.rtscts = False
    ser.dsrdtr = False

    # 关键：open 前先设置，避免串口打开瞬间拉动 DTR/RTS 造成板子复位。
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False

    time.sleep(min(max(timeout_s, 0.0), 1.0))
    return ser


def drain_serial(ser: "serial.Serial", duration_s: float = 0.25) -> bytes:
    deadline = time.monotonic() + duration_s
    chunks: List[bytes] = []
    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        if waiting:
            chunks.append(ser.read(waiting))
        else:
            time.sleep(0.02)
    return b"".join(chunks)


def send_cli_command(
        ser: "serial.Serial",
        cmd: str,
        *,
        total_timeout_s: float = 8.0,
        idle_timeout_s: float = 0.45,
        reset_input: bool = True,
) -> str:
    if reset_input:
        drain_serial(ser, 0.05)
        ser.reset_input_buffer()

    ser.write((cmd + "\r\n").encode("ascii"))
    ser.flush()

    chunks: List[bytes] = []
    deadline = time.monotonic() + total_timeout_s
    last_data = time.monotonic()

    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        data = ser.read(waiting if waiting else 1)
        if data:
            chunks.append(data)
            last_data = time.monotonic()
        else:
            if time.monotonic() - last_data >= idle_timeout_s:
                break

    return b"".join(chunks).decode("utf-8", errors="replace")


def read_exact_with_timeout(ser: "serial.Serial", n: int, timeout_s: float) -> bytes:
    chunks: List[bytes] = []
    got = 0
    deadline = time.monotonic() + timeout_s
    while got < n and time.monotonic() < deadline:
        waiting = ser.in_waiting
        to_read = min(n - got, waiting if waiting else 1)
        data = ser.read(to_read)
        if data:
            chunks.append(data)
            got += len(data)
        else:
            time.sleep(0.001)
    return b"".join(chunks)


def receive_ov56rgb5_frame(ser: "serial.Serial", timeout_s: float) -> Tuple[FrameResult, bytes]:
    start = time.monotonic()
    deadline = start + timeout_s
    buf = bytearray()

    # 先寻找 magic，允许前面混入少量文本输出。
    while time.monotonic() < deadline:
        data = ser.read(1)
        if data:
            buf.extend(data)
            idx = bytes(buf).find(FRAME_MAGIC)
            if idx >= 0:
                if idx > 0:
                    del buf[:idx]
                break
        else:
            time.sleep(0.001)
    else:
        return FrameResult(False, "未找到 OV56RGB5 magic", total_len=len(buf)), bytes(buf)

    remaining_header = FRAME_HEADER_LEN - len(buf)
    if remaining_header > 0:
        buf.extend(read_exact_with_timeout(ser, remaining_header, max(0.1, deadline - time.monotonic())))

    if len(buf) < FRAME_HEADER_LEN:
        return FrameResult(False, f"header 接收不完整：{len(buf)}/{FRAME_HEADER_LEN} B", total_len=len(buf)), bytes(buf)

    header = bytes(buf[:FRAME_HEADER_LEN])
    magic = header[0:8]
    version = header[8]
    pixel_format = header[9]
    width = struct.unpack_from("<H", header, 10)[0]
    height = struct.unpack_from("<H", header, 12)[0]
    payload_len = struct.unpack_from("<I", header, 14)[0]
    frame_id = struct.unpack_from("<I", header, 18)[0]

    if magic != FRAME_MAGIC:
        return FrameResult(False, "magic 不匹配", total_len=len(buf)), bytes(buf)
    if version != 1 or pixel_format != 1:
        return FrameResult(False, f"version/pixel_format 异常：{version}/{pixel_format}", total_len=len(buf)), bytes(buf)
    if payload_len <= 0 or payload_len > 2_000_000:
        return FrameResult(False, f"payload_len 异常：{payload_len}", total_len=len(buf)), bytes(buf)

    need = payload_len + 4
    buf.extend(read_exact_with_timeout(ser, need, max(0.1, deadline - time.monotonic())))

    total_expected = FRAME_HEADER_LEN + payload_len + 4
    elapsed_ms = (time.monotonic() - start) * 1000.0
    if len(buf) < total_expected:
        return FrameResult(
            False,
            f"接收超时，只收到 {len(buf)}/{total_expected} B",
            total_len=len(buf),
            frame_id=frame_id,
            width=width,
            height=height,
            payload_len=payload_len,
            elapsed_ms=elapsed_ms,
        ), bytes(buf)

    payload = bytes(buf[FRAME_HEADER_LEN:FRAME_HEADER_LEN + payload_len])
    rx_crc = struct.unpack_from("<I", buf, FRAME_HEADER_LEN + payload_len)[0]
    calc_crc = crc32_u32(payload)
    ok = rx_crc == calc_crc
    err = "" if ok else f"CRC 不一致：rx=0x{rx_crc:08X}, calc=0x{calc_crc:08X}"
    return FrameResult(
        ok,
        err,
        total_len=len(buf),
        frame_id=frame_id,
        width=width,
        height=height,
        payload_len=payload_len,
        rx_crc32=rx_crc,
        calc_crc32=calc_crc,
        elapsed_ms=elapsed_ms,
    ), bytes(buf)


def send_image_request(ser: "serial.Serial", seq: int, timeout_s: float) -> FrameResult:
    drain_serial(ser, 0.05)
    ser.reset_input_buffer()
    req = make_image_request(seq)
    ser.write(req)
    ser.flush()
    result, _raw = receive_ov56rgb5_frame(ser, timeout_s)
    return result


def summarize_sd_init(text: str) -> Dict[str, Optional[int]]:
    keys = [
        "is_initialized",
        "sdio_ready",
        "fatfs_ready",
        "sdio_hal_init_success_count",
        "sdio_hal_init_error_count",
        "last_hal_sd_init_status",
        "last_hal_sd_error",
        "card_info_read_success_count",
        "card_block_size",
        "card_log_block_size",
    ]
    return {k: get_int_field(text, k) for k in keys}


def summarize_readinfo(text: str) -> Dict[str, Optional[int]]:
    keys = [
        "block_read_attempt_count",
        "block_read_success_count",
        "block_read_error_count",
        "last_block_read_status",
        "last_block_read_error",
        "last_block_read_addr",
        "last_block_read_count",
        "last_block_read_size",
        "last_block_read_error_is_data_crc_fail",
        "last_block_read_error_is_cmd_crc_fail",
        "last_block_read_error_is_data_timeout",
        "last_block_read_error_is_rx_overrun",
    ]
    return {k: get_int_field(text, k) for k in keys}


def print_kv(logger: TestLogger, title: str, fields: Dict[str, Optional[int]]) -> None:
    logger.write(title)
    for k, v in fields.items():
        logger.write(f"  {k}={v if v is not None else '<未找到>'}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage 11 SD Snapshot 自动板测脚本")
    parser.add_argument("--port", default="COM4", help="串口号，默认 COM4")
    parser.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    parser.add_argument("--startup-drain", type=float, default=0.8, help="打开串口后读取启动残留输出的时间")
    parser.add_argument("--cli-timeout", type=float, default=8.0, help="单条 CLI 命令最大等待时间")
    parser.add_argument("--cli-idle", type=float, default=0.45, help="CLI 输出静默多久认为结束")
    parser.add_argument("--frame-timeout", type=float, default=FRAME_DEFAULT_TIMEOUT_S, help="图像帧接收超时")
    parser.add_argument("--repeat", type=int, default=20, help="RESTORE 后重复二进制图像请求次数，默认 20，设为 0 可跳过")
    parser.add_argument("--output-dir", default="captures", help="日志输出目录，默认 captures")
    parser.add_argument("--tag", default="sd_c5r_auto", help="日志文件标签")
    parser.add_argument("--skip-readtest", action="store_true", help="SD INIT OK 后跳过 SD READTEST 0/2048")
    parser.add_argument("--skip-image", action="store_true", help="跳过 RESTORE 后图像请求验证")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(args.output_dir, f"{args.tag}_{ts}_log.txt")
    summary_path = os.path.join(args.output_dir, f"{args.tag}_{ts}_summary.txt")

    logger = TestLogger(log_path)
    summary_lines: List[str] = []

    def add_summary(line: str) -> None:
        summary_lines.append(line)
        logger.write(line)

    ser: Optional["serial.Serial"] = None
    exit_code = 0

    try:
        logger.section("Stage 11 SD Snapshot 自动测试开始")
        logger.write(f"串口：{args.port}")
        logger.write(f"波特率：{args.baud}")
        logger.write("DTR状态：False")
        logger.write("RTS状态：False")
        logger.write(f"日志文件：{log_path}")

        ser = open_serial(args.port, args.baud, args.startup_drain)
        startup = drain_serial(ser, args.startup_drain).decode("utf-8", errors="replace")
        if startup.strip():
            logger.section("启动/残留输出")
            logger.write(startup.rstrip())

        outputs: Dict[str, str] = {}

        def run(cmd: str, name: Optional[str] = None, timeout: Optional[float] = None) -> str:
            text = send_cli_command(
                ser,  # type: ignore[arg-type]
                cmd,
                total_timeout_s=timeout if timeout is not None else args.cli_timeout,
                idle_timeout_s=args.cli_idle,
            )
            key = name or cmd
            outputs[key] = text
            logger.command(cmd, text)
            return text

        logger.section("一、SD INIT 前置保护与 SDIO takeover")
        pre_init = run("SD INIT")
        prepare = run("SNAPSHOT PREPARE")
        enter = run("SD TAKEOVER ENTER")
        sd_init = run("SD INIT")
        sd_status_after_init = run("SD STATUS")
        sd_cardinfo_after_init = run("SD CARDINFO")

        init_text = sd_init + "\n" + sd_status_after_init + "\n" + sd_cardinfo_after_init
        init_ok = "SD INIT: HAL_SD_Init OK" in sd_init and get_int_field(init_text, "card_info_read_success_count") == 1
        init_failed = "SD INIT: HAL_SD_Init failed" in sd_init

        logger.section("二、SD INIT 结果摘要")
        print_kv(logger, "SD INIT 字段：", summarize_sd_init(init_text))

        if init_ok:
            add_summary("SD_INIT=PASS")
        else:
            add_summary("SD_INIT=FAIL")
            if init_failed:
                add_summary("SD_INIT_FAIL_REASON=HAL_SD_Init failed")
            else:
                add_summary("SD_INIT_FAIL_REASON=unknown")
            exit_code = 1

        if init_ok and not args.skip_readtest:
            logger.section("三、SD READTEST 只读块测试")
            read0 = run("SD READTEST 0")
            readinfo0 = run("SD READINFO")
            read2048 = run("SD READTEST 2048")
            readinfo2048 = run("SD READINFO")

            logger.section("SD READTEST 0 摘要")
            print_kv(logger, "READ 0 字段：", summarize_readinfo(read0 + "\n" + readinfo0))
            logger.section("SD READTEST 2048 摘要")
            print_kv(logger, "READ 2048 字段：", summarize_readinfo(read2048 + "\n" + readinfo2048))

            read0_ok = "SD READTEST: block read OK" in read0
            read2048_ok = "SD READTEST: block read OK" in read2048
            read0_crc = get_int_field(read0 + "\n" + readinfo0, "last_block_read_error_is_data_crc_fail") == 1
            read2048_crc = get_int_field(read2048 + "\n" + readinfo2048, "last_block_read_error_is_data_crc_fail") == 1
            add_summary(f"READTEST_0={'PASS' if read0_ok else 'DATA_CRC_FAIL' if read0_crc else 'FAIL'}")
            add_summary(f"READTEST_2048={'PASS' if read2048_ok else 'DATA_CRC_FAIL' if read2048_crc else 'FAIL'}")
        elif not init_ok:
            logger.section("三、跳过 SD READTEST")
            logger.write("原因：SD INIT 未成功，READTEST 不会真正读卡。")
            add_summary("READTEST_0=SKIP")
            add_summary("READTEST_2048=SKIP")

        logger.section("四、guard、退出 SD takeover、恢复 snapshot")
        dump_guard = run("DUMP", timeout=4.0)
        exit_text = run("SD TAKEOVER EXIT")
        sd_status_after_exit = run("SD STATUS")
        takeover_status_after_exit = run("SD TAKEOVER STATUS")
        readinfo_after_exit = run("SD READINFO")
        cardinfo_after_exit = run("SD CARDINFO")
        restore_text = run("SNAPSHOT RESTORE")
        snapshot_status = run("SNAPSHOT STATUS")
        final_status = run("STATUS")

        guard_ok = "DUMP blocked: snapshot software guard active" in dump_guard
        exit_ok = "SD TAKEOVER EXIT: HAL_SD_DeInit status=0" in exit_text and "conflict pins restored to DCMI AF13" in exit_text
        restore_ok = get_int_field(snapshot_status, "software_guard_active") == 0 and get_int_field(snapshot_status, "dump_block_required") == 0

        add_summary(f"DUMP_GUARD={'PASS' if guard_ok else 'FAIL'}")
        add_summary(f"TAKEOVER_EXIT={'PASS' if exit_ok else 'FAIL'}")
        add_summary(f"SNAPSHOT_RESTORE={'PASS' if restore_ok else 'FAIL'}")

        iwdg_skip = get_int_field(final_status, "iwdg_refresh_skip_count")
        hook_fault = get_int_field(final_status, "hook_fault_code")
        uart_dma_err = get_int_field(final_status, "uart_dma_error_count")
        stream_overflow = get_int_field(final_status, "stream_buffer_overflow_bytes")
        system_ok = (iwdg_skip == 0 and hook_fault == 0 and uart_dma_err == 0 and stream_overflow == 0)
        add_summary(f"SYSTEM_STATUS={'PASS' if system_ok else 'FAIL'}")

        if not args.skip_image:
            logger.section("五、RESTORE 后二进制图像请求验证")
            first = send_image_request(ser, 0x1234, args.frame_timeout)  # type: ignore[arg-type]
            logger.write(
                f"BASIC: {'PASS' if first.ok else 'FAIL'} "
                f"frame_id={first.frame_id} len={first.total_len} "
                f"crc_rx=0x{first.rx_crc32:08X} crc_calc=0x{first.calc_crc32:08X} "
                f"time={first.elapsed_ms:.1f}ms error={first.error}"
            )
            add_summary(f"BASIC_IMAGE={'PASS' if first.ok else 'FAIL'}")
            if not first.ok:
                exit_code = 1

            repeat_results: List[FrameResult] = []
            if args.repeat > 0:
                logger.section(f"六、RESTORE 后重复图像请求：{args.repeat} 次")
                for i in range(1, args.repeat + 1):
                    r = send_image_request(ser, i, args.frame_timeout)  # type: ignore[arg-type]
                    repeat_results.append(r)
                    logger.write(
                        f"[{i:02d}/{args.repeat:02d}] seq=0x{i:04X} "
                        f"frame_id={r.frame_id} len={r.total_len} "
                        f"time={r.elapsed_ms:.1f} ms {'PASS' if r.ok else 'FAIL'} "
                        f"{r.error}"
                    )
                    time.sleep(0.2)

                success_count = sum(1 for r in repeat_results if r.ok)
                frame_ids = [r.frame_id for r in repeat_results if r.ok]
                continuous = bool(frame_ids) and frame_ids == list(range(frame_ids[0], frame_ids[0] + len(frame_ids)))
                avg_ms = sum(r.elapsed_ms for r in repeat_results if r.ok) / success_count if success_count else 0.0
                add_summary(f"REPEAT_SUCCESS={success_count}/{args.repeat}")
                add_summary(f"REPEAT_FRAME_ID_CONTINUOUS={'YES' if continuous else 'NO'}")
                add_summary(f"REPEAT_AVG_MS={avg_ms:.2f}")
                if success_count != args.repeat or not continuous:
                    exit_code = 1

        logger.section("测试结论")
        if exit_code == 0:
            logger.write("测试结果：PASS")
        else:
            logger.write("测试结果：FAIL / PARTIAL，请查看上方失败字段。")

    except KeyboardInterrupt:
        logger.write("\n用户中断。")
        exit_code = 130
    except Exception as exc:  # pragma: no cover
        logger.write(f"脚本异常：{exc!r}")
        exit_code = 1
    finally:
        try:
            if ser is not None and ser.is_open:
                ser.dtr = False
                ser.rts = False
                ser.close()
        finally:
            with open(summary_path, "w", encoding="utf-8", newline="\n") as fp:
                fp.write("\n".join(summary_lines) + "\n")
            logger.write(f"\n完整日志：{log_path}")
            logger.write(f"摘要文件：{summary_path}")
            logger.close()

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())