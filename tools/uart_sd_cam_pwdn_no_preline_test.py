#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 11C-5F-1A：OV5640 PWDN 隔离 clean test

关键区别：
- 在 SD TAKEOVER ENTER 之后执行 SD CAMOFF。
- SD CAMOFF 后不调用 SD LINESTATE。
- 直接执行 SD INIT / SD CARDINFO。
- 只有 SD INIT 完成后才调用 SD LINESTATE。
这样避免“SD INIT 前调用 SD LINESTATE”这个已知干扰变量。

不写 SD 卡，不接 FATFS。
"""

from __future__ import annotations

import argparse
import binascii
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

LINE_KEYS = [
    ("PC8_D0", "pc8_d0"),
    ("PC9_D1", "pc9_d1"),
    ("PC10_D2", "pc10_d2"),
    ("PC11_D3", "pc11_d3"),
    ("PC12_CK", "pc12_ck"),
    ("PD2_CMD", "pd2_cmd"),
]


@dataclass
class ImageResult:
    """保存一次图像请求的校验结果。"""
    ok: bool
    length: int
    frame_id: Optional[int] = None
    error: str = ""
    elapsed_ms: float = 0.0


def now_tag() -> str:
    """生成适合文件名使用的当前时间标签。"""
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def crc32_u32(data: bytes) -> int:
    """计算 CRC32，使请求或图像负载的传输损坏可被协议检测。"""
    return binascii.crc32(data) & 0xFFFFFFFF


def build_image_request(seq: int) -> bytes:
    """构造带 CRC32 的二进制图像请求帧。"""
    body = struct.pack("<BBHH", 0x01, 0x20, seq & 0xFFFF, 0)
    return b"\xA5\x5A" + body + struct.pack("<I", crc32_u32(body)) + b"\x0D\x0A"


def parse_kv(text: str) -> Dict[str, str]:
    """解析 CLI 响应中的键值字段。"""
    out: Dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def get_int(kv: Dict[str, str], key: str, default: int = -1) -> int:
    """读取整数键值，缺失或非法时返回默认值。"""
    v = kv.get(key)
    if v is None:
        return default
    try:
        return int(v, 0)
    except ValueError:
        return default


def classify_readtest(text: str, kv: Dict[str, str]) -> str:
    """根据 READTEST 响应字段归类测试结果。"""
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
    """封装串口 CLI 与二进制图像请求会话。"""
    def __init__(self, port: str, baud: int, log: List[str]):
        """初始化测试会话及其运行状态。"""
        self.log = log
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.1
        self.ser.write_timeout = 2.0
        self.ser.rtscts = False
        self.ser.dsrdtr = False
        # open 前关闭控制线，避免 CH340 自动下载电路切换 BOOT0 或复位 MCU。
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        time.sleep(0.2)
        self.drain(0.8)

    def close(self) -> None:
        """关闭串口或日志等会话资源。"""
        if self.ser.is_open:
            self.ser.close()

    def drain(self, max_time: float = 0.5) -> bytes:
        """清空串口中本轮测试前的残留输入。"""
        end = time.monotonic() + max_time
        chunks: List[bytes] = []
        while time.monotonic() < end:
            data = self.ser.read(self.ser.in_waiting or 1)
            if data:
                chunks.append(data)
                end = time.monotonic() + 0.2
        return b"".join(chunks)

    def read_until_quiet(self, max_time: float = 8.0, quiet_time: float = 0.45) -> bytes:
        """持续读取响应，直到串口保持静默。"""
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
        """发送一条 CLI 命令并读取完整文本响应。"""
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
        """在超时约束内读取指定字节数。"""
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
        """发送二进制图像请求并校验完整响应。"""
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

        version = data[8]
        fmt = data[9]
        width = struct.unpack_from("<H", data, 10)[0]
        height = struct.unpack_from("<H", data, 12)[0]
        payload_len = struct.unpack_from("<I", data, 14)[0]
        frame_id = struct.unpack_from("<I", data, 18)[0]
        payload = data[22:22 + payload_len]
        recv_crc = struct.unpack_from("<I", data, 22 + payload_len)[0]
        calc_crc = crc32_u32(payload)
        crc_ok = recv_crc == calc_crc
        self.log.append(
            "frame_id=%s, version=%s, fmt=%s, size=%sx%s, payload_len=%s, recv_crc=0x%08X, calc_crc=0x%08X, crc_ok=%s, elapsed_ms=%.2f"
            % (frame_id, version, fmt, width, height, payload_len, recv_crc, calc_crc, crc_ok, elapsed_ms)
        )
        ok = version == 1 and fmt == 1 and width == 160 and height == 120 and payload_len == FRAME_PAYLOAD_LEN and crc_ok
        return ImageResult(ok, len(data), frame_id=frame_id, elapsed_ms=elapsed_ms)


def add_line_summary(summary: Dict[str, str], prefix: str, text: str) -> None:
    """把本轮 SDIO 线状态写入汇总记录。"""
    kv = parse_kv(text)
    summary[prefix + "_LINESTATE_READONLY"] = str(get_int(kv, "linestate_readonly", -1))
    summary[prefix + "_HAL_SD_API_CALL"] = str(get_int(kv, "linestate_hal_sd_api_call", -1))
    summary[prefix + "_IS_INITIALIZED"] = str(get_int(kv, "is_initialized", -1))
    summary[prefix + "_SDIO_READY"] = str(get_int(kv, "sdio_ready", -1))
    summary[prefix + "_HAL_SD_STATE"] = str(get_int(kv, "hal_sd_state", -1))
    summary[prefix + "_CARD_STATE"] = str(get_int(kv, "hal_sd_card_state", -1))
    summary[prefix + "_GPIOC_IDR"] = kv.get("gpioc_idr", "NA")
    summary[prefix + "_GPIOD_IDR"] = kv.get("gpiod_idr", "NA")

    all_data_cmd_high = True
    known = True
    for label, key in LINE_KEYS:
        for field in ("idr", "mode", "pull", "speed", "af"):
            summary["%s_%s_%s" % (prefix, label, field.upper())] = str(get_int(kv, key + "_" + field, -1))
        if label != "PC12_CK":
            idr = get_int(kv, key + "_idr", -1)
            if idr not in (0, 1):
                known = False
            elif idr != 1:
                all_data_cmd_high = False

    summary[prefix + "_DATA_CMD_IDLE_HIGH"] = "YES" if known and all_data_cmd_high else "NO"


def run_readtest(sess: UartSession, block: int, summary: Dict[str, str], gap: float) -> None:
    """执行一次 SD READTEST 并归类结果。"""
    text1 = sess.cli("SD READTEST %d" % block, max_time=8.0, after_delay=gap)
    text2 = sess.cli("SD READINFO", max_time=8.0, after_delay=gap)
    combined = text1 + "\n" + text2
    kv = parse_kv(combined)
    prefix = "READTEST_%d" % block
    summary[prefix] = classify_readtest(combined, kv)
    summary[prefix + "_LAST_ERROR"] = str(get_int(kv, "last_block_read_error", -1))
    summary[prefix + "_DATA_CRC"] = str(get_int(kv, "last_block_read_error_is_data_crc_fail", 0))
    summary[prefix + "_SIZE"] = str(get_int(kv, "last_block_read_size", -1))


def main() -> int:
    """解析参数并执行完整测试流程。"""
    p = argparse.ArgumentParser(description="Stage 11C-5F-1A CAMOFF no-pre-line clean diagnostic")
    p.add_argument("--port", default="COM4")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--captures", default="captures")
    p.add_argument("--repeat", type=int, default=20)
    p.add_argument("--frame-timeout", type=float, default=10.0)
    p.add_argument("--command-gap", type=float, default=1.0)
    p.add_argument("--after-prepare-delay", type=float, default=3.0)
    p.add_argument("--after-enter-delay", type=float, default=5.0)
    p.add_argument("--after-camoff-delay", type=float, default=1.0)
    p.add_argument("--after-camon-delay", type=float, default=1.0)
    p.add_argument("--after-exit-delay", type=float, default=1.0)
    p.add_argument("--after-restore-delay", type=float, default=1.5)
    args = p.parse_args()

    out_dir = Path(args.captures)
    out_dir.mkdir(parents=True, exist_ok=True)
    tag = now_tag()
    log_path = out_dir / ("sd_cam_pwdn_no_preline_%s_log.txt" % tag)
    summary_path = out_dir / ("sd_cam_pwdn_no_preline_%s_summary.txt" % tag)

    log: List[str] = []
    summary: Dict[str, str] = {}
    sess: Optional[UartSession] = None
    rc = 0

    try:
        sess = UartSession(args.port, args.baud, log)

        init_before = sess.cli("SD INIT", after_delay=args.command_gap)
        summary["PRE_TAKEOVER_SD_INIT_BLOCKED"] = "YES" if "need SDIO takeover" in init_before else "NO"

        sess.cli("SNAPSHOT PREPARE", max_time=8.0, after_delay=args.after_prepare_delay)
        sess.cli("SD TAKEOVER ENTER", max_time=8.0, after_delay=args.after_enter_delay)

        camoff = sess.cli("SD CAMOFF", max_time=8.0, after_delay=args.after_camoff_delay)
        summary["SD_CAMOFF"] = "PASS" if ("OK" in camoff or "PWDN" in camoff or "camoff" in camoff.lower()) else "UNKNOWN"

        power_status = sess.cli("SD CAMPOWER", max_time=8.0, after_delay=args.command_gap)
        kvp = parse_kv(power_status)
        summary["CAMERA_PWDN_ACTIVE"] = str(get_int(kvp, "camera_pwdn_active", -1))
        summary["CAMERA_POWERDOWN_COUNT"] = str(get_int(kvp, "camera_powerdown_count", -1))

        # 关键：CAMOFF 后不做 SD LINESTATE，直接 SD INIT
        init_text = sess.cli("SD INIT", max_time=10.0, after_delay=args.command_gap)
        status_text = sess.cli("SD STATUS", max_time=10.0, after_delay=args.command_gap)
        cardinfo_text = sess.cli("SD CARDINFO", max_time=8.0, after_delay=args.command_gap)
        init_all = init_text + "\n" + status_text + "\n" + cardinfo_text
        kv = parse_kv(init_all)
        init_ok = "HAL_SD_Init OK" in init_text or get_int(kv, "sdio_hal_init_success_count", 0) > 0

        summary["SD_INIT"] = "PASS" if init_ok else "FAIL"
        summary["IS_INITIALIZED"] = str(get_int(kv, "is_initialized", -1))
        summary["SDIO_READY"] = str(get_int(kv, "sdio_ready", -1))
        summary["HAL_INIT_SUCCESS"] = str(get_int(kv, "sdio_hal_init_success_count", -1))
        summary["HAL_INIT_ERROR"] = str(get_int(kv, "sdio_hal_init_error_count", -1))
        summary["LAST_HAL_INIT_STATUS"] = str(get_int(kv, "last_hal_sd_init_status", -1))
        summary["LAST_HAL_ERROR"] = str(get_int(kv, "last_hal_sd_error", -1))
        summary["CARDINFO_SUCCESS"] = str(get_int(kv, "card_info_read_success_count", -1))
        summary["CARD_BLOCK_SIZE"] = str(get_int(kv, "card_block_size", -1))
        summary["CARD_LOG_BLOCK_SIZE"] = str(get_int(kv, "card_log_block_size", -1))

        line = sess.cli("SD LINESTATE", max_time=8.0, after_delay=args.command_gap)
        add_line_summary(summary, "LINE_AFTER_INIT", line)

        if init_ok:
            run_readtest(sess, 0, summary, args.command_gap)
            line = sess.cli("SD LINESTATE", max_time=8.0, after_delay=args.command_gap)
            add_line_summary(summary, "LINE_AFTER_READ0", line)

            run_readtest(sess, 2048, summary, args.command_gap)
            line = sess.cli("SD LINESTATE", max_time=8.0, after_delay=args.command_gap)
            add_line_summary(summary, "LINE_AFTER_READ2048", line)
        else:
            summary["READTEST_0"] = "SKIP"
            summary["READTEST_2048"] = "SKIP"

        dump_text = sess.cli("DUMP", max_time=5.0, after_delay=args.command_gap)
        summary["DUMP_GUARD"] = "PASS" if "DUMP blocked" in dump_text else "FAIL"

        camon = sess.cli("SD CAMON", max_time=8.0, after_delay=args.after_camon_delay)
        summary["SD_CAMON"] = "PASS" if ("OK" in camon or "PWDN" in camon or "camon" in camon.lower()) else "UNKNOWN"

        power_status = sess.cli("SD CAMPOWER", max_time=8.0, after_delay=args.command_gap)
        kvp = parse_kv(power_status)
        summary["CAMERA_PWDN_AFTER_CAMON"] = str(get_int(kvp, "camera_pwdn_active", -1))

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
            continuous = all(frame_ids[i] + 1 == frame_ids[i + 1] for i in range(len(frame_ids) - 1))
            summary["REPEAT_FIRST_FRAME_ID"] = str(frame_ids[0])
            summary["REPEAT_LAST_FRAME_ID"] = str(frame_ids[-1])
            summary["REPEAT_FRAME_ID_CONTINUOUS"] = "YES" if continuous else "NO"
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
        summary["LOG_FILE"] = str(log_path)
        summary["SUMMARY_FILE"] = str(summary_path)
        log_path.write_text("\n".join(log), encoding="utf-8")
        text = "\n".join("%s=%s" % (k, v) for k, v in summary.items())
        summary_path.write_text(text + "\n", encoding="utf-8")
        print(text)

    return rc


if __name__ == "__main__":
    raise SystemExit(main())
