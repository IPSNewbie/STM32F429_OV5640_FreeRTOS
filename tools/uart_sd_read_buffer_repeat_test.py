#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 11C-5I：失败读块 buffer 指纹重复性测试脚本。

用于 C5H 之后：
- 不写 SD 卡，不接 FATFS，不改 SDIO 参数。
- 反复执行 SD READTEST 0 / 2048。
- 记录 buffer 统计字段，判断 CRC 失败时 buffer 内容是否稳定。
"""

from __future__ import annotations

import argparse
import binascii
import csv
import datetime as dt
import re
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

try:
    import serial  # type: ignore
except ImportError:
    print("错误：未安装 pyserial，请先执行：pip install pyserial")
    raise

MAGIC = b"OV56RGB5"
FRAME_PAYLOAD_LEN = 160 * 120 * 2
FRAME_TOTAL_LEN = 22 + FRAME_PAYLOAD_LEN + 4


@dataclass
class ImageResult:
    ok: bool
    length: int
    frame_id: Optional[int] = None
    error: str = ""
    elapsed_ms: float = 0.0


def crc32_u32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def build_image_request(seq: int) -> bytes:
    body = struct.pack("<BBHH", 0x01, 0x20, seq & 0xFFFF, 0)
    return b"\xA5\x5A" + body + struct.pack("<I", crc32_u32(body)) + b"\x0D\x0A"


def parse_kv(text: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def get_int(kv: Dict[str, str], key: str, default: int = -1) -> int:
    v = kv.get(key)
    if v is None:
        return default
    try:
        return int(v, 0)
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
        return "FAIL_ERROR_%d" % get_int(kv, "last_block_read_error", -1)
    if get_int(kv, "last_block_read_error_is_data_crc_fail", 0) == 1:
        return "DATA_CRC_FAIL"
    return "UNKNOWN"


class UartSession:
    def __init__(self, port: str, baud: int, log: List[str]):
        self.log = log
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.1
        self.ser.write_timeout = 2.0
        self.ser.rtscts = False
        self.ser.dsrdtr = False
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        time.sleep(0.2)
        self.drain(0.8)

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
            if chunks and now - last >= quiet_time:
                break
        return b"".join(chunks)

    def cli(self, cmd: str, max_time: float = 8.0, after_delay: float = 0.0) -> str:
        self.drain(0.15)
        self.log.append("\n===== CLI >>> %s =====" % cmd)
        self.ser.write((cmd + "\r\n").encode("ascii"))
        self.ser.flush()
        text = self.read_until_quiet(max_time=max_time).decode("utf-8", errors="replace")
        self.log.append(text.rstrip())
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

    def image_request(self, seq: int, timeout: float) -> ImageResult:
        self.drain(0.2)
        self.log.append("\n===== BIN >>> IMAGE_REQUEST seq=0x%04X =====" % (seq & 0xFFFF))
        start = time.monotonic()
        self.ser.write(build_image_request(seq))
        self.ser.flush()
        data = self.read_exact(FRAME_TOTAL_LEN, timeout)
        elapsed_ms = (time.monotonic() - start) * 1000.0
        if len(data) != FRAME_TOTAL_LEN:
            err = "接收超时，只收到 %d/%d B" % (len(data), FRAME_TOTAL_LEN)
            self.log.append(err)
            return ImageResult(False, len(data), error=err, elapsed_ms=elapsed_ms)
        if data[:8] != MAGIC:
            err = "magic 错误：%r" % (data[:8],)
            self.log.append(err)
            return ImageResult(False, len(data), error=err, elapsed_ms=elapsed_ms)
        payload_len = struct.unpack_from("<I", data, 14)[0]
        frame_id = struct.unpack_from("<I", data, 18)[0]
        payload = data[22:22 + payload_len]
        recv_crc = struct.unpack_from("<I", data, 22 + payload_len)[0]
        calc_crc = crc32_u32(payload)
        ok = (
                data[8] == 1 and data[9] == 1
                and struct.unpack_from("<H", data, 10)[0] == 160
                and struct.unpack_from("<H", data, 12)[0] == 120
                and payload_len == FRAME_PAYLOAD_LEN
                and recv_crc == calc_crc
        )
        self.log.append("frame_id=%s payload_len=%s ok=%s elapsed_ms=%.2f" % (frame_id, payload_len, ok, elapsed_ms))
        return ImageResult(ok, len(data), frame_id=frame_id, elapsed_ms=elapsed_ms)


def read_once(sess: UartSession, block: int, index: int, gap: float) -> Dict[str, str]:
    text1 = sess.cli("SD READTEST %d" % block, max_time=8.0, after_delay=gap)
    text2 = sess.cli("SD READINFO", max_time=8.0, after_delay=gap)
    combined = text1 + "\n" + text2
    kv = parse_kv(combined)
    return {
        "block": str(block),
        "index": str(index),
        "result": classify_readtest(combined, kv),
        "last_error": str(get_int(kv, "last_block_read_error", -1)),
        "data_crc": str(get_int(kv, "last_block_read_error_is_data_crc_fail", 0)),
        "read_size": str(get_int(kv, "last_block_read_size", -1)),
        "buffer_inspected": str(get_int(kv, "last_block_read_buffer_inspected", -1)),
        "buffer_len": str(get_int(kv, "last_block_read_buffer_len", -1)),
        "prefill_count": str(get_int(kv, "last_block_read_buffer_prefill_count512", -1)),
        "changed_count": str(get_int(kv, "last_block_read_buffer_changed_count512", -1)),
        "zero_count": str(get_int(kv, "last_block_read_buffer_zero_count512", -1)),
        "ff_count": str(get_int(kv, "last_block_read_buffer_ff_count512", -1)),
        "nonzero_count": str(get_int(kv, "last_block_read_buffer_nonzero_count512", -1)),
        "sum512": str(get_int(kv, "last_block_read_buffer_sum512", -1)),
        "xor512": kv.get("last_block_read_buffer_xor512", "NA"),
        "fnv1a32": kv.get("last_block_read_buffer_fnv1a32", "NA"),
        "all_prefill": str(get_int(kv, "last_block_read_buffer_all_prefill", -1)),
        "first_changed": str(get_int(kv, "last_block_read_buffer_first_changed_index", -1)),
        "last_changed": str(get_int(kv, "last_block_read_buffer_last_changed_index", -1)),
        "first32": kv.get("last_block_read_buffer_first32", "NA"),
        "middle16": kv.get("last_block_read_buffer_middle16", "NA"),
        "tail16": kv.get("last_block_read_buffer_tail16", "NA"),
        "hal_error_after_read": kv.get("read_hal_error_after_read", "NA"),
        "after_dctrl": kv.get("read_after_read_dctrl", "NA"),
        "after_dlen": kv.get("read_after_read_dlen", "NA"),
        "after_dcount": kv.get("read_after_read_dcount", "NA"),
        "after_fifocnt": kv.get("read_after_read_fifocnt", "NA"),
    }


def main() -> int:
    p = argparse.ArgumentParser(description="Stage 11C-5I SD read buffer fingerprint repeat test")
    p.add_argument("--port", default="COM4")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--captures", default="captures")
    p.add_argument("--read-count", type=int, default=5)
    p.add_argument("--repeat", type=int, default=20)
    p.add_argument("--frame-timeout", type=float, default=10.0)
    p.add_argument("--command-gap", type=float, default=1.0)
    p.add_argument("--after-prepare-delay", type=float, default=3.0)
    p.add_argument("--after-enter-delay", type=float, default=5.0)
    p.add_argument("--after-exit-delay", type=float, default=1.0)
    p.add_argument("--after-restore-delay", type=float, default=1.5)
    args = p.parse_args()

    out_dir = Path(args.captures)
    out_dir.mkdir(parents=True, exist_ok=True)
    tag = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = out_dir / ("sd_read_buffer_repeat_%s_log.txt" % tag)
    csv_path = out_dir / ("sd_read_buffer_repeat_%s.csv" % tag)
    summary_path = out_dir / ("sd_read_buffer_repeat_%s_summary.txt" % tag)

    log: List[str] = []
    rows: List[Dict[str, str]] = []
    summary: Dict[str, str] = {}
    sess: Optional[UartSession] = None
    rc = 0

    try:
        sess = UartSession(args.port, args.baud, log)

        init_before = sess.cli("SD INIT", after_delay=args.command_gap)
        summary["PRE_TAKEOVER_SD_INIT_BLOCKED"] = "YES" if "need SDIO takeover" in init_before else "NO"

        sess.cli("SNAPSHOT PREPARE", max_time=8.0, after_delay=args.after_prepare_delay)
        sess.cli("SD TAKEOVER ENTER", max_time=8.0, after_delay=args.after_enter_delay)

        init_text = sess.cli("SD INIT", max_time=10.0, after_delay=args.command_gap)
        status_text = sess.cli("SD STATUS", max_time=10.0, after_delay=args.command_gap)
        cardinfo_text = sess.cli("SD CARDINFO", max_time=8.0, after_delay=args.command_gap)
        init_all = init_text + "\n" + status_text + "\n" + cardinfo_text
        kv = parse_kv(init_all)
        init_ok = "HAL_SD_Init OK" in init_text or get_int(kv, "sdio_hal_init_success_count", 0) > 0
        summary["SD_INIT"] = "PASS" if init_ok else "FAIL"
        summary["IS_INITIALIZED"] = str(get_int(kv, "is_initialized", -1))
        summary["SDIO_READY"] = str(get_int(kv, "sdio_ready", -1))
        summary["CARDINFO_SUCCESS"] = str(get_int(kv, "card_info_read_success_count", -1))
        summary["LAST_HAL_ERROR"] = str(get_int(kv, "last_hal_sd_error", -1))

        if init_ok:
            for block in (0, 2048):
                for i in range(args.read_count):
                    rows.append(read_once(sess, block, i + 1, args.command_gap))
            for block in (0, 2048):
                block_rows = [r for r in rows if r["block"] == str(block)]
                summary["BLOCK_%d_RESULTS" % block] = ",".join(r["result"] for r in block_rows)
                for key in ("fnv1a32", "sum512", "first32", "tail16", "changed_count", "zero_count", "ff_count"):
                    values = [r[key] for r in block_rows]
                    summary["BLOCK_%d_%s_UNIQUE" % (block, key.upper())] = str(len(set(values)))
                    summary["BLOCK_%d_%s_VALUES" % (block, key.upper())] = " | ".join(values)
        else:
            summary["READ_REPEAT"] = "SKIP"

        dump_text = sess.cli("DUMP", max_time=5.0, after_delay=args.command_gap)
        summary["DUMP_GUARD"] = "PASS" if "DUMP blocked" in dump_text else "FAIL"

        exit_text = sess.cli("SD TAKEOVER EXIT", max_time=10.0, after_delay=args.after_exit_delay)
        summary["TAKEOVER_EXIT"] = "PASS" if ("HAL_SD_DeInit status=0" in exit_text and "restored" in exit_text) else "FAIL"

        restore_text = sess.cli("SNAPSHOT RESTORE", max_time=8.0, after_delay=args.after_restore_delay)
        summary["SNAPSHOT_RESTORE"] = "PASS" if "SNAPSHOT RESTORE" in restore_text else "FAIL"

        final_status = sess.cli("STATUS", max_time=10.0, after_delay=args.command_gap)
        kvs = parse_kv(final_status)
        summary["SYSTEM_STATUS"] = "PASS"
        summary["IWDG_SKIP"] = str(get_int(kvs, "iwdg_refresh_skip_count", -1))
        summary["HOOK_FAULT"] = str(get_int(kvs, "hook_fault_code", -1))
        summary["UART_DMA_ERROR"] = str(get_int(kvs, "uart_dma_error_count", -1))
        summary["STREAM_OVERFLOW"] = str(get_int(kvs, "stream_buffer_overflow_bytes", -1))

        basic = sess.image_request(seq=0x1234, timeout=args.frame_timeout)
        summary["BASIC_IMAGE"] = "PASS" if basic.ok else "FAIL"
        summary["BASIC_FRAME_ID"] = str(basic.frame_id if basic.frame_id is not None else -1)
        summary["BASIC_ERROR"] = basic.error

        success = 0
        frame_ids: List[int] = []
        elapsed: List[float] = []
        for i in range(args.repeat):
            time.sleep(0.2)
            r = sess.image_request(seq=i + 1, timeout=args.frame_timeout)
            if r.ok:
                success += 1
                if r.frame_id is not None:
                    frame_ids.append(r.frame_id)
                elapsed.append(r.elapsed_ms)
        summary["REPEAT_SUCCESS"] = "%d/%d" % (success, args.repeat)
        if frame_ids:
            summary["REPEAT_FIRST_FRAME_ID"] = str(frame_ids[0])
            summary["REPEAT_LAST_FRAME_ID"] = str(frame_ids[-1])
            summary["REPEAT_FRAME_ID_CONTINUOUS"] = "YES" if all(frame_ids[i] + 1 == frame_ids[i+1] for i in range(len(frame_ids)-1)) else "NO"
        else:
            summary["REPEAT_FIRST_FRAME_ID"] = "-1"
            summary["REPEAT_LAST_FRAME_ID"] = "-1"
            summary["REPEAT_FRAME_ID_CONTINUOUS"] = "NO"
        summary["REPEAT_AVG_MS"] = "%.2f" % (sum(elapsed) / len(elapsed)) if elapsed else "0.00"

    except Exception as exc:
        summary["SCRIPT_EXCEPTION"] = repr(exc)
        log.append("\n脚本异常：%r" % (exc,))
        rc = 2
    finally:
        if sess is not None:
            sess.close()

        if rows:
            fieldnames = list(rows[0].keys())
            with csv_path.open("w", encoding="utf-8", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=fieldnames)
                writer.writeheader()
                writer.writerows(rows)

        summary["CSV_FILE"] = str(csv_path)
        summary["LOG_FILE"] = str(log_path)
        summary["SUMMARY_FILE"] = str(summary_path)

        log_path.write_text("\n".join(log), encoding="utf-8")
        text = "\n".join("%s=%s" % (k, v) for k, v in summary.items())
        summary_path.write_text(text + "\n", encoding="utf-8")
        print(text)

    return rc


if __name__ == "__main__":
    raise SystemExit(main())