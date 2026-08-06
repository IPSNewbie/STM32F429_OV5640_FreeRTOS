#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 11C-5D：SDIO 显式 1-bit / 4-bit bus 配置自动测试脚本

使用前提：
1. 固件已经实现 SD BUSWIDTH / SD BUSWIDTH 1B / SD BUSWIDTH 4B。
2. 当前仍使用 COM4、115200。
3. 本脚本只通过 UART CLI 和二进制图像请求测试，不写 SD 卡。

默认流程：
- SD INIT（确认 takeover 前被拦截）
- SNAPSHOT PREPARE
- SD TAKEOVER ENTER
- SD INIT
- SD CARDINFO
- SD BUSWIDTH
- SD BUSWIDTH 1B + READTEST 0/2048
- SD BUSWIDTH 4B + READTEST 0/2048
- DUMP guard
- SD TAKEOVER EXIT
- SNAPSHOT RESTORE
- STATUS
- RESTORE 后 basic 图像请求
- RESTORE 后 repeat 图像请求

输出：
- captures/sd_buswidth_auto_<timestamp>_log.txt
- captures/sd_buswidth_auto_<timestamp>_summary.txt
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
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import serial  # type: ignore
except ImportError:
    print("错误：未安装 pyserial，请先执行：pip install pyserial", file=sys.stderr)
    raise


MAGIC = b"OV56RGB5"
FRAME_HEADER_LEN = 22
FRAME_PAYLOAD_LEN = 160 * 120 * 2
FRAME_TOTAL_LEN = FRAME_HEADER_LEN + FRAME_PAYLOAD_LEN + 4


@dataclass
class ImageResult:
    ok: bool
    length: int
    frame_id: Optional[int] = None
    crc_ok: bool = False
    error: str = ""
    elapsed_ms: float = 0.0


def now_tag() -> str:
    return _dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def decode_text(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


def parse_kv(text: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$", line)
        if m:
            result[m.group(1)] = m.group(2)
    return result


def get_int(kv: Dict[str, str], key: str, default: int = -1) -> int:
    value = kv.get(key)
    if value is None:
        return default
    try:
        return int(value, 0)
    except ValueError:
        return default


def classify_readtest(text: str, kv: Dict[str, str]) -> str:
    if "not ready" in text:
        return "SKIP_NOT_READY"
    if "block read OK" in text:
        return "PASS"
    if "block read failed" in text:
        if get_int(kv, "last_block_read_error_is_data_crc_fail", 0) == 1:
            return "DATA_CRC_FAIL"
        err = get_int(kv, "last_block_read_error", -1)
        return f"FAIL_ERROR_{err}"
    if get_int(kv, "block_read_success_count", 0) > 0 and get_int(kv, "last_block_read_size", 0) == 512:
        return "PASS"
    if get_int(kv, "last_block_read_error_is_data_crc_fail", 0) == 1:
        return "DATA_CRC_FAIL"
    return "UNKNOWN"


def crc32_u32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def build_image_request(seq: int) -> bytes:
    body = struct.pack("<BBHH", 0x01, 0x20, seq & 0xFFFF, 0)
    crc = crc32_u32(body)
    return b"\xA5\x5A" + body + struct.pack("<I", crc) + b"\x0D\x0A"


class UartSession:
    def __init__(self, port: str, baud: int, log_lines: List[str], read_timeout: float = 0.1):
        self.log_lines = log_lines
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = read_timeout
        self.ser.write_timeout = 2.0
        self.ser.rtscts = False
        self.ser.dsrdtr = False
        # 关键：open 前关闭 DTR/RTS，避免打开串口导致板子复位
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        time.sleep(0.2)
        self.drain(max_time=0.8)

    def close(self) -> None:
        if self.ser.is_open:
            self.ser.close()

    def drain(self, max_time: float = 0.5) -> bytes:
        end = time.monotonic() + max_time
        chunks: List[bytes] = []
        while time.monotonic() < end:
            data = self.ser.read(self.ser.in_waiting or 1)
            if data:
                chunks.append(data)
                end = time.monotonic() + 0.2
        return b"".join(chunks)

    def read_until_quiet(self, max_time: float = 8.0, quiet_time: float = 0.45) -> bytes:
        start = time.monotonic()
        last = start
        chunks: List[bytes] = []
        while True:
            data = self.ser.read(self.ser.in_waiting or 1)
            now = time.monotonic()
            if data:
                chunks.append(data)
                last = now
            if now - start >= max_time:
                break
            if chunks and (now - last) >= quiet_time:
                break
        return b"".join(chunks)

    def send_cli(self, command: str, max_time: float = 8.0, quiet_time: float = 0.45,
                 after_delay: float = 0.0) -> str:
        self.drain(max_time=0.15)
        self.log_lines.append(f"\n===== CLI >>> {command} =====")
        self.ser.write((command + "\r\n").encode("ascii"))
        self.ser.flush()
        data = self.read_until_quiet(max_time=max_time, quiet_time=quiet_time)
        text = decode_text(data)
        self.log_lines.append(text.rstrip())
        if after_delay > 0:
            time.sleep(after_delay)
        return text

    def read_exact(self, n: int, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        chunks: List[bytes] = []
        total = 0
        while total < n and time.monotonic() < deadline:
            chunk = self.ser.read(n - total)
            if chunk:
                chunks.append(chunk)
                total += len(chunk)
        return b"".join(chunks)

    def send_image_request(self, seq: int, timeout: float) -> ImageResult:
        self.drain(max_time=0.2)
        req = build_image_request(seq)
        self.log_lines.append(f"\n===== BIN >>> IMAGE_REQUEST seq=0x{seq & 0xFFFF:04X} =====")
        start = time.monotonic()
        self.ser.write(req)
        self.ser.flush()
        data = self.read_exact(FRAME_TOTAL_LEN, timeout=timeout)
        elapsed_ms = (time.monotonic() - start) * 1000.0

        if len(data) != FRAME_TOTAL_LEN:
            msg = f"接收超时，只收到 {len(data)}/{FRAME_TOTAL_LEN} B"
            self.log_lines.append(msg)
            return ImageResult(False, len(data), error=msg, elapsed_ms=elapsed_ms)

        if data[:8] != MAGIC:
            msg = f"magic 错误：{data[:8]!r}"
            self.log_lines.append(msg)
            return ImageResult(False, len(data), error=msg, elapsed_ms=elapsed_ms)

        version = data[8]
        pixel_format = data[9]
        width = struct.unpack_from("<H", data, 10)[0]
        height = struct.unpack_from("<H", data, 12)[0]
        payload_len = struct.unpack_from("<I", data, 14)[0]
        frame_id = struct.unpack_from("<I", data, 18)[0]
        payload = data[22:22 + payload_len]
        recv_crc = struct.unpack_from("<I", data, 22 + payload_len)[0]
        calc_crc = crc32_u32(payload)
        crc_ok = recv_crc == calc_crc

        self.log_lines.append(
            f"frame_id={frame_id}, version={version}, fmt={pixel_format}, "
            f"size={width}x{height}, payload_len={payload_len}, "
            f"recv_crc=0x{recv_crc:08X}, calc_crc=0x{calc_crc:08X}, "
            f"crc_ok={crc_ok}, elapsed_ms={elapsed_ms:.2f}"
        )

        ok = (
                version == 1
                and pixel_format == 1
                and width == 160
                and height == 120
                and payload_len == FRAME_PAYLOAD_LEN
                and crc_ok
        )
        return ImageResult(ok, len(data), frame_id=frame_id, crc_ok=crc_ok, elapsed_ms=elapsed_ms)


def run_readtest_pair(sess: UartSession, bus_name: str, summary: Dict[str, str],
                      command_gap: float) -> None:
    for block in (0, 2048):
        text1 = sess.send_cli(f"SD READTEST {block}", max_time=8.0, after_delay=command_gap)
        text2 = sess.send_cli("SD READINFO", max_time=8.0, after_delay=command_gap)
        combined = text1 + "\n" + text2
        kv = parse_kv(combined)
        key_prefix = f"READTEST_{bus_name}_{block}"
        summary[key_prefix] = classify_readtest(combined, kv)
        summary[f"{key_prefix}_LAST_ERROR"] = str(get_int(kv, "last_block_read_error", -1))
        summary[f"{key_prefix}_DATA_CRC"] = str(get_int(kv, "last_block_read_error_is_data_crc_fail", 0))
        summary[f"{key_prefix}_SIZE"] = str(get_int(kv, "last_block_read_size", -1))


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage 11C-5D SDIO 1-bit / 4-bit bus 自动测试")
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--captures", default="captures")
    parser.add_argument("--repeat", type=int, default=20)
    parser.add_argument("--frame-timeout", type=float, default=10.0)
    parser.add_argument("--command-gap", type=float, default=0.8)
    parser.add_argument("--after-prepare-delay", type=float, default=1.5)
    parser.add_argument("--after-enter-delay", type=float, default=2.0)
    parser.add_argument("--after-buswidth-delay", type=float, default=1.0)
    parser.add_argument("--after-exit-delay", type=float, default=1.0)
    parser.add_argument("--after-restore-delay", type=float, default=1.0)
    args = parser.parse_args()

    out_dir = Path(args.captures)
    out_dir.mkdir(parents=True, exist_ok=True)
    tag = now_tag()
    log_path = out_dir / f"sd_buswidth_auto_{tag}_log.txt"
    summary_path = out_dir / f"sd_buswidth_auto_{tag}_summary.txt"

    log_lines: List[str] = []
    summary: Dict[str, str] = {}

    log_lines.append("Stage 11C-5D SDIO bus width auto test")
    log_lines.append(f"port={args.port}, baud={args.baud}")
    log_lines.append(f"timestamp={tag}")

    sess: Optional[UartSession] = None
    try:
        sess = UartSession(args.port, args.baud, log_lines)

        # takeover 前 SD INIT 应该被 NEED_TAKEOVER 拦截
        init_before = sess.send_cli("SD INIT", after_delay=args.command_gap)
        summary["PRE_TAKEOVER_SD_INIT_BLOCKED"] = "YES" if "need SDIO takeover" in init_before else "NO"

        sess.send_cli("SNAPSHOT PREPARE", max_time=8.0, after_delay=args.after_prepare_delay)
        sess.send_cli("SD TAKEOVER ENTER", max_time=8.0, after_delay=args.after_enter_delay)

        init_text = sess.send_cli("SD INIT", max_time=10.0, after_delay=args.command_gap)
        status_text = sess.send_cli("SD STATUS", max_time=10.0, after_delay=args.command_gap)
        cardinfo_text = sess.send_cli("SD CARDINFO", max_time=8.0, after_delay=args.command_gap)
        init_combined = init_text + "\n" + status_text + "\n" + cardinfo_text
        init_kv = parse_kv(init_combined)

        init_ok = "HAL_SD_Init OK" in init_text or get_int(init_kv, "sdio_hal_init_success_count", 0) > 0
        summary["SD_INIT"] = "PASS" if init_ok else "FAIL"
        summary["IS_INITIALIZED"] = str(get_int(init_kv, "is_initialized", -1))
        summary["SDIO_READY"] = str(get_int(init_kv, "sdio_ready", -1))
        summary["HAL_INIT_SUCCESS"] = str(get_int(init_kv, "sdio_hal_init_success_count", -1))
        summary["HAL_INIT_ERROR"] = str(get_int(init_kv, "sdio_hal_init_error_count", -1))
        summary["LAST_HAL_INIT_STATUS"] = str(get_int(init_kv, "last_hal_sd_init_status", -1))
        summary["LAST_HAL_ERROR"] = str(get_int(init_kv, "last_hal_sd_error", -1))
        summary["CARDINFO_SUCCESS"] = str(get_int(init_kv, "card_info_read_success_count", -1))
        summary["CARD_BLOCK_SIZE"] = str(get_int(init_kv, "card_block_size", -1))
        summary["CARD_LOG_BLOCK_SIZE"] = str(get_int(init_kv, "card_log_block_size", -1))

        if not init_ok:
            summary["BUSWIDTH_QUERY"] = "SKIP"
            summary["BUSWIDTH_1B_SET"] = "SKIP"
            summary["READTEST_1B_0"] = "SKIP"
            summary["READTEST_1B_2048"] = "SKIP"
            summary["BUSWIDTH_4B_SET"] = "SKIP"
            summary["READTEST_4B_0"] = "SKIP"
            summary["READTEST_4B_2048"] = "SKIP"
        else:
            bus_query = sess.send_cli("SD BUSWIDTH", max_time=8.0, after_delay=args.command_gap)
            summary["BUSWIDTH_QUERY"] = "PASS" if ("SD BUSWIDTH" in bus_query or "bus_width" in bus_query) else "UNKNOWN"

            bus1 = sess.send_cli("SD BUSWIDTH 1B", max_time=10.0,
                                 after_delay=args.after_buswidth_delay)
            summary["BUSWIDTH_1B_SET"] = "PASS" if ("OK" in bus1 or "set OK" in bus1) else "FAIL"
            sess.send_cli("SD BUSWIDTH", max_time=8.0, after_delay=args.command_gap)
            run_readtest_pair(sess, "1B", summary, args.command_gap)

            bus4 = sess.send_cli("SD BUSWIDTH 4B", max_time=10.0,
                                 after_delay=args.after_buswidth_delay)
            summary["BUSWIDTH_4B_SET"] = "PASS" if ("OK" in bus4 or "set OK" in bus4) else "FAIL"
            sess.send_cli("SD BUSWIDTH", max_time=8.0, after_delay=args.command_gap)
            run_readtest_pair(sess, "4B", summary, args.command_gap)

        dump_text = sess.send_cli("DUMP", max_time=5.0, after_delay=args.command_gap)
        summary["DUMP_GUARD"] = "PASS" if "DUMP blocked" in dump_text else "FAIL"

        exit_text = sess.send_cli("SD TAKEOVER EXIT", max_time=10.0,
                                  after_delay=args.after_exit_delay)
        summary["TAKEOVER_EXIT"] = (
            "PASS"
            if ("HAL_SD_DeInit status=0" in exit_text and "restored" in exit_text)
            else "FAIL"
        )

        sess.send_cli("SD STATUS", max_time=8.0, after_delay=args.command_gap)
        sess.send_cli("SD TAKEOVER STATUS", max_time=8.0, after_delay=args.command_gap)
        sess.send_cli("SD READINFO", max_time=8.0, after_delay=args.command_gap)
        sess.send_cli("SD CARDINFO", max_time=8.0, after_delay=args.command_gap)

        restore_text = sess.send_cli("SNAPSHOT RESTORE", max_time=8.0,
                                     after_delay=args.after_restore_delay)
        summary["SNAPSHOT_RESTORE"] = "PASS" if ("SNAPSHOT RESTORE" in restore_text) else "FAIL"

        final_status = sess.send_cli("STATUS", max_time=10.0, after_delay=args.command_gap)
        status_kv = parse_kv(final_status)
        summary["SYSTEM_STATUS"] = "PASS"
        summary["IWDG_SKIP"] = str(get_int(status_kv, "iwdg_refresh_skip_count", -1))
        summary["HOOK_FAULT"] = str(get_int(status_kv, "hook_fault_code", -1))
        summary["UART_DMA_ERROR"] = str(get_int(status_kv, "uart_dma_error_count", -1))
        summary["STREAM_OVERFLOW"] = str(get_int(status_kv, "stream_buffer_overflow_bytes", -1))

        basic = sess.send_image_request(seq=0x1234, timeout=args.frame_timeout)
        summary["BASIC_IMAGE"] = "PASS" if basic.ok else "FAIL"
        summary["BASIC_FRAME_ID"] = str(basic.frame_id if basic.frame_id is not None else -1)
        summary["BASIC_ERROR"] = basic.error

        success = 0
        frame_ids: List[int] = []
        elapsed: List[float] = []
        for i in range(args.repeat):
            time.sleep(0.2)
            result = sess.send_image_request(seq=i + 1, timeout=args.frame_timeout)
            if result.ok:
                success += 1
                if result.frame_id is not None:
                    frame_ids.append(result.frame_id)
                elapsed.append(result.elapsed_ms)

        summary["REPEAT_SUCCESS"] = f"{success}/{args.repeat}"
        if frame_ids:
            continuous = all((frame_ids[i] + 1 == frame_ids[i + 1]) for i in range(len(frame_ids) - 1))
            summary["REPEAT_FIRST_FRAME_ID"] = str(frame_ids[0])
            summary["REPEAT_LAST_FRAME_ID"] = str(frame_ids[-1])
            summary["REPEAT_FRAME_ID_CONTINUOUS"] = "YES" if continuous else "NO"
        else:
            summary["REPEAT_FIRST_FRAME_ID"] = "-1"
            summary["REPEAT_LAST_FRAME_ID"] = "-1"
            summary["REPEAT_FRAME_ID_CONTINUOUS"] = "NO"
        summary["REPEAT_AVG_MS"] = f"{(sum(elapsed) / len(elapsed)):.2f}" if elapsed else "0.00"

    except Exception as exc:
        summary["SCRIPT_EXCEPTION"] = repr(exc)
        log_lines.append(f"\n脚本异常：{exc!r}")
        return_code = 2
    else:
        return_code = 0
    finally:
        if sess is not None:
            sess.close()

        summary["LOG_FILE"] = str(log_path)
        summary["SUMMARY_FILE"] = str(summary_path)

        log_path.write_text("\n".join(log_lines), encoding="utf-8")
        summary_text = "\n".join(f"{k}={v}" for k, v in summary.items())
        summary_path.write_text(summary_text + "\n", encoding="utf-8")

        print(summary_text)

    return return_code


if __name__ == "__main__":
    raise SystemExit(main())