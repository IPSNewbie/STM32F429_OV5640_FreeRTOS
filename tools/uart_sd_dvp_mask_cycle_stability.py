#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stage 11C-5U-2: DVP mask 稳定性脚本，支持 single-session / cycle 对照。"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import struct
import time
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

try:
    import serial  # type: ignore
except ImportError:
    serial = None  # type: ignore


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
    "timestamp",
    "mode",
    "cycle",
    "read_index",
    "pre_cleanup",
    "sd_init_blocked",
    "snapshot_prepare",
    "dvpstop",
    "dvpstop_result",
    "dvp_mask_active_after_stop",
    "dvp_mask_saved",
    "dvp_mask_before",
    "dvp_mask_written",
    "dvp_mask_after",
    "dvp_mask_error_code",
    "dvp_mask_error_text",
    "takeover_enter",
    "takeover_enter_result",
    "sdio_full_gpio_af12_selected",
    "sdio_af12_selected",
    "takeover_error_code",
    "takeover_error_text",
    "snapshot_pause_confirmed",
    "conflict_pin_release_ready",
    "atk1b_init",
    "atk1b_init_result",
    "atk1b_init_ready",
    "atk1b_hal_init_status",
    "atk1b_hal_error",
    "atk1b_cardinfo_status",
    "atk1b_cardinfo_error",
    "atk1b_wait_transfer_attempt_count",
    "atk1b_wait_transfer_success_count",
    "atk1b_wait_transfer_error_count",
    "atk1b_last_card_state",
    "atk1b_last_operation_ms",
    "atk1b_clock_div",
    "atk1b_bus_width",
    "read_block_0_result",
    "read_block_0_error",
    "read_block_0_size",
    "read_block_0_sum512",
    "read_block_2048_result",
    "read_block_2048_error",
    "read_block_2048_size",
    "read_block_2048_sum512",
    "takeover_exit",
    "dvprestore",
    "dvprestore_result",
    "dvp_mask_active_after_restore",
    "dvp_restore_before",
    "dvp_restore_written",
    "dvp_restore_after",
    "dvp_restore_error_code",
    "dvp_restore_error_text",
    "snapshot_restore",
    "system_status",
    "iwdg_skip",
    "hook_fault",
    "uart_dma_error",
    "stream_overflow",
    "image_result",
    "image_rx_len",
    "image_error",
    "image_magic_found",
    "image_crc_ok",
    "image_frame_id",
    "cycle_result",
    "errors",
]


def parse_blocks(text: str) -> List[int]:
    """按响应边界拆分串口 CLI 文本块。"""
    values: List[int] = []
    for item in text.split(","):
        token = item.strip()
        if not token or not token.isdecimal():
            raise argparse.ArgumentTypeError("--blocks 仅支持逗号分隔的十进制非负 block 地址")
        value = int(token, 10)
        if value > 0xFFFFFFFF:
            raise argparse.ArgumentTypeError("block 地址超出 uint32 范围")
        if value not in values:
            values.append(value)
    if not values:
        raise argparse.ArgumentTypeError("--blocks 不能为空")
    return values


def sanitize_tag(text: str) -> str:
    """清洗标签，使其可安全用于文件名。"""
    tag = re.sub(r"[^A-Za-z0-9_-]+", "_", text.strip()).strip("_")
    if not tag:
        raise argparse.ArgumentTypeError("--tag 必须包含字母、数字、下划线或连字符")
    return tag


def parse_kv(text: str) -> Dict[str, str]:
    """解析 CLI 响应中的键值字段。"""
    values: Dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r"\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def get_int(values: Dict[str, str], key: str, default: int = -1) -> int:
    """读取整数键值，缺失或非法时返回默认值。"""
    text = values.get(key)
    if text is None:
        return default
    try:
        return int(text, 0)
    except ValueError:
        return default


def build_request(seq: int) -> bytes:
    """按正式协议构造二进制图像请求帧。"""
    if not 0 <= seq <= 0xFFFF:
        raise ValueError("请求 seq 必须在 0 到 65535 之间")
    body = struct.pack("<BBHH", REQUEST_VERSION, REQUEST_TYPE, seq, 0)
    request_crc = zlib.crc32(body) & 0xFFFFFFFF
    request = REQUEST_MAGIC + body + struct.pack("<I", request_crc) + b"\x0D\x0A"
    if len(request) != REQUEST_SIZE:
        raise ValueError("请求帧长度错误")
    return request


def validate_image_frame(frame: bytes) -> Tuple[bool, bool, Optional[int], str]:
    """校验 OV56RGB5 图像响应的固定字段与 CRC32。"""
    if len(frame) != IMAGE_FRAME_SIZE:
        return False, False, None, "响应长度错误：%d/%d B" % (len(frame), IMAGE_FRAME_SIZE)

    magic = frame[0:8]
    version = frame[8]
    pixel_format = frame[9]
    width, height = struct.unpack("<HH", frame[10:14])
    payload_len, frame_id = struct.unpack("<II", frame[14:22])
    if magic != IMAGE_MAGIC:
        return False, False, None, "响应 magic 错误"
    if version != IMAGE_VERSION or pixel_format != IMAGE_PIXEL_FORMAT:
        return False, False, frame_id, "响应版本或像素格式错误"
    if width != IMAGE_WIDTH or height != IMAGE_HEIGHT:
        return False, False, frame_id, "响应尺寸错误：%dx%d" % (width, height)
    if payload_len != IMAGE_PAYLOAD_SIZE:
        return False, False, frame_id, "响应 payload_len 错误：%d" % payload_len

    payload_end = IMAGE_HEADER_SIZE + payload_len
    if payload_end + IMAGE_CRC_SIZE != len(frame):
        return False, False, frame_id, "响应总长度与 payload_len 不一致"
    payload = frame[IMAGE_HEADER_SIZE:payload_end]
    received_crc = struct.unpack("<I", frame[payload_end:payload_end + 4])[0]
    calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
    crc_ok = received_crc == calculated_crc
    if not crc_ok:
        return False, False, frame_id, "payload CRC 错误：接收 0x%08X，计算 0x%08X" % (
            received_crc,
            calculated_crc,
        )
    return True, True, frame_id, ""


def classify_read(text: str, values: Dict[str, str]) -> str:
    """根据读块响应字段归类测试结果。"""
    if "SD ATK1BREAD: block read OK" in text or (
            get_int(values, "atk_1bit_last_read_status") == 0
            and get_int(values, "atk_1bit_last_read_size") == 512
    ):
        return "PASS"
    if get_int(values, "atk_1bit_read_error_is_data_crc_fail", 0) == 1 or get_int(
            values, "atk_1bit_last_read_error"
    ) == 2:
        return "DATA_CRC_FAIL"
    if "not ready" in text.lower() or get_int(values, "atk_1bit_init_ready", 0) == 0:
        return "NOT_READY"
    return "OTHER_FAIL"


def check_frame_ids_continuous(frame_ids: Sequence[int]) -> bool:
    """检查图像帧编号是否连续递增。"""
    if len(frame_ids) <= 1:
        return bool(frame_ids)
    return all(
        frame_ids[index] == ((frame_ids[index - 1] + 1) & 0xFFFFFFFF)
        for index in range(1, len(frame_ids))
    )


class UartSession:
    """封装串口 CLI 与二进制图像请求会话。"""
    def __init__(self, port: str, baud: int, command_timeout: float, log: List[str]) -> None:
        """初始化测试会话及其运行状态。"""
        if serial is None:
            raise RuntimeError("未安装 pyserial，请先执行：pip install pyserial")
        self.command_timeout = command_timeout
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
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def close(self) -> None:
        """关闭串口或日志等会话资源。"""
        if self.ser.is_open:
            self.ser.close()

    def read_text_until_quiet(self) -> str:
        """持续读取文本，直到串口保持静默。"""
        started = time.monotonic()
        last_data = started
        quiet_time = min(0.45, max(0.1, self.command_timeout / 4.0))
        chunks: List[bytes] = []
        while True:
            data = self.ser.read(self.ser.in_waiting or 1)
            now = time.monotonic()
            if data:
                chunks.append(data)
                last_data = now
                combined_tail = b"".join(chunks[-2:])[-8:]
                if combined_tail.endswith((b"\r\n> ", b"\r\n# ")):
                    break
            if now - started >= self.command_timeout:
                break
            if chunks and now - last_data >= quiet_time:
                break
        return b"".join(chunks).decode("utf-8", errors="replace")

    def cli(self, command: str) -> str:
        """发送一条 CLI 命令并读取完整文本响应。"""
        self.ser.reset_input_buffer()
        self.log.append("\n===== %s CLI >>> %s =====" % (dt.datetime.now().isoformat(timespec="seconds"), command))
        self.ser.write((command + "\r\n").encode("ascii"))
        self.ser.flush()
        text = self.read_text_until_quiet()
        self.log.append(text.rstrip())
        return text

    def image_request(self, seq: int, timeout_seconds: float) -> Tuple[bytes, int]:
        """发送二进制图像请求并校验完整响应。"""
        request = build_request(seq)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        self.log.append(
            "\n===== %s BINARY IMAGE >>> seq=%d request=%s ====="
            % (dt.datetime.now().isoformat(timespec="seconds"), seq, request.hex(" "))
        )
        self.ser.write(request)
        self.ser.flush()

        data = bytearray()
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                data.extend(chunk)
            # CLI 文本可能先到，按 magic 定位可避免把混合输出误当成图像帧头。
            magic_index = data.find(IMAGE_MAGIC)
            if magic_index >= 0 and len(data) >= magic_index + IMAGE_FRAME_SIZE:
                frame = bytes(data[magic_index:magic_index + IMAGE_FRAME_SIZE])
                self.log.append(
                    "BINARY RX frame_len=%d total_rx=%d magic_offset=%d"
                    % (len(frame), len(data), magic_index)
                )
                return frame, len(frame)

        self.log.append("BINARY RX timeout total_rx=%d" % len(data))
        return bytes(data), len(data)


def safe_cli(session: UartSession, command: str, log: List[str]) -> str:
    """执行 CLI 命令，并将异常转换为可记录结果。"""
    try:
        return session.cli(command)
    except Exception as exc:
        log.append("%s 异常：%r" % (command, exc))
        return ""


def sleep_and_log(log: List[str], label: str, seconds: float) -> None:
    """记录原因后等待指定时间。"""
    if seconds > 0.0:
        log.append("%s wait %.3f s" % (label, seconds))
        time.sleep(seconds)


def best_effort_cleanup(session: UartSession, log: List[str], label: str, cleanup_wait: float) -> str:
    """尽最大努力恢复 takeover 状态，避免一次失败影响后续测试。"""
    log.append("\n===== %s =====" % label)
    responses: List[bool] = []
    commands = ("SD TAKEOVER EXIT", "SD DVPRESTORE", "SNAPSHOT RESTORE", "STATUS")
    for index, command in enumerate(commands):
        responses.append(bool(safe_cli(session, command, log).strip()))
        if index + 1 < len(commands):
            sleep_and_log(log, "%s %s" % (label, command), cleanup_wait)
    return "DONE" if all(responses) else "PARTIAL"


def unique_values(rows: Sequence[Dict[str, str]], field: str) -> str:
    """提取字段中按顺序去重的值。"""
    values: List[str] = []
    for row in rows:
        value = row.get(field, "")
        if value not in values:
            values.append(value)
    return "|".join(values)


def failed_cycle_values(rows: Sequence[Dict[str, str]], field: str, pass_value: str) -> str:
    """提取失败循环对应的字段值。"""
    return ",".join(row["cycle"] for row in rows if row.get(field) != pass_value)


def fail_reason_counts(rows: Sequence[Dict[str, str]]) -> str:
    """统计各类失败原因的出现次数。"""
    counts: Dict[str, int] = {}
    for row in rows:
        for reason in row.get("errors", "").split(" | "):
            if reason:
                counts[reason] = counts.get(reason, 0) + 1
    return "|".join("%s:%d" % (reason, counts[reason]) for reason in sorted(counts))


def make_row(mode: str, cycle: int, read_index: str = "") -> Dict[str, str]:
    """创建一行带默认值的循环测试记录。"""
    row = {field: "" for field in CSV_FIELDS}
    row.update(
        {
            "timestamp": dt.datetime.now().isoformat(timespec="seconds"),
            "mode": mode,
            "cycle": str(cycle),
            "read_index": read_index,
            "pre_cleanup": "NOT_RUN",
            "sd_init_blocked": "UNKNOWN",
            "snapshot_prepare": "FAIL",
            "dvpstop": "FAIL",
            "dvpstop_result": "FAIL",
            "dvp_mask_active_after_stop": "-1",
            "dvp_mask_saved": "-1",
            "dvp_mask_before": "-1",
            "dvp_mask_written": "-1",
            "dvp_mask_after": "-1",
            "dvp_mask_error_code": "-1",
            "dvp_mask_error_text": "UNKNOWN",
            "takeover_enter": "FAIL",
            "takeover_enter_result": "FAIL",
            "sdio_full_gpio_af12_selected": "-1",
            "sdio_af12_selected": "-1",
            "takeover_error_code": "-1",
            "takeover_error_text": "UNKNOWN",
            "snapshot_pause_confirmed": "-1",
            "conflict_pin_release_ready": "-1",
            "atk1b_init": "FAIL",
            "atk1b_init_result": "FAIL",
            "atk1b_init_ready": "-1",
            "atk1b_hal_init_status": "-1",
            "atk1b_hal_error": "-1",
            "atk1b_cardinfo_status": "-1",
            "atk1b_cardinfo_error": "-1",
            "atk1b_wait_transfer_attempt_count": "-1",
            "atk1b_wait_transfer_success_count": "-1",
            "atk1b_wait_transfer_error_count": "-1",
            "atk1b_last_card_state": "-1",
            "atk1b_last_operation_ms": "-1",
            "atk1b_clock_div": "-1",
            "atk1b_bus_width": "-1",
            "read_block_0_result": "SKIP",
            "read_block_0_error": "-1",
            "read_block_0_size": "-1",
            "read_block_0_sum512": "-1",
            "read_block_2048_result": "SKIP",
            "read_block_2048_error": "-1",
            "read_block_2048_size": "-1",
            "read_block_2048_sum512": "-1",
            "takeover_exit": "FAIL",
            "dvprestore": "FAIL",
            "dvprestore_result": "FAIL",
            "dvp_mask_active_after_restore": "-1",
            "dvp_restore_before": "-1",
            "dvp_restore_written": "-1",
            "dvp_restore_after": "-1",
            "dvp_restore_error_code": "-1",
            "dvp_restore_error_text": "UNKNOWN",
            "snapshot_restore": "FAIL",
            "system_status": "FAIL",
            "iwdg_skip": "-1",
            "hook_fault": "-1",
            "uart_dma_error": "-1",
            "stream_overflow": "-1",
            "image_result": "IMAGE_SKIP",
            "image_rx_len": "0",
            "image_error": "NOT_RUN",
            "image_magic_found": "0",
            "image_crc_ok": "0",
            "image_frame_id": "-1",
            "cycle_result": "FAIL",
            "errors": "",
        }
    )
    return row


def parse_prepare(text: str) -> bool:
    """解析 SNAPSHOT PREPARE 响应字段。"""
    return any(
        marker in text
        for marker in (
            "SNAPSHOT PREPARE: DCMI stop OK",
            "SNAPSHOT PREPARE: SD-only boot, virtual camera pause OK",
            "virtual camera pause OK",
        )
    )


def run_sd_init_deferred(session: UartSession, row: Dict[str, str], log: List[str]) -> None:
    """执行 takeover 后延迟的 SD 初始化。"""
    pre_init_text = safe_cli(session, "SD INIT", log)
    pre_init_lower = pre_init_text.lower()
    row["sd_init_blocked"] = "YES" if ("need sdio takeover" in pre_init_lower or "deferred" in pre_init_lower) else "NO"


def run_prepare(session: UartSession, row: Dict[str, str], log: List[str]) -> bool:
    """执行 SNAPSHOT PREPARE 步骤。"""
    prepare_text = safe_cli(session, "SNAPSHOT PREPARE", log)
    ok = parse_prepare(prepare_text)
    row["snapshot_prepare"] = "PASS" if ok else "FAIL"
    return ok


def run_dvpstop(session: UartSession, row: Dict[str, str], log: List[str]) -> Tuple[bool, int]:
    """执行 DVP 输出停止步骤。"""
    dvp_stop_text = safe_cli(session, "SD DVPSTOP", log)
    dvp_status_text = safe_cli(session, "SD DVPSTATUS", log)
    dvp_values = parse_kv(dvp_stop_text + "\n" + dvp_status_text)
    saved = get_int(dvp_values, "dvp_mask_reg_3018_saved")
    after = get_int(dvp_values, "dvp_mask_reg_3018_after")
    active = get_int(dvp_values, "dvp_mask_active")
    row["dvp_mask_active_after_stop"] = str(active)
    row["dvp_mask_saved"] = str(saved)
    row["dvp_mask_before"] = str(get_int(dvp_values, "dvp_mask_reg_3018_before"))
    row["dvp_mask_written"] = str(get_int(dvp_values, "dvp_mask_reg_3018_written"))
    row["dvp_mask_after"] = str(after)
    row["dvp_mask_error_code"] = str(get_int(dvp_values, "dvp_mask_last_error_code"))
    row["dvp_mask_error_text"] = dvp_values.get("dvp_mask_last_error_text", "UNKNOWN")
    ok = "SD DVPSTOP: OV5640 D2/D3/D4 pad output disabled" in dvp_stop_text and active == 1 and after == 143
    row["dvpstop"] = "PASS" if ok else "FAIL"
    row["dvpstop_result"] = row["dvpstop"]
    return ok, saved


def run_takeover(session: UartSession, row: Dict[str, str], log: List[str]) -> bool:
    """执行 SDIO takeover 进入步骤。"""
    enter_text = safe_cli(session, "SD TAKEOVER ENTER", log)
    enter_values = parse_kv(enter_text)
    full_gpio_af12 = get_int(enter_values, "sdio_full_gpio_af12_selected")
    sdio_af12 = get_int(enter_values, "sdio_af12_selected")
    row["sdio_full_gpio_af12_selected"] = str(full_gpio_af12)
    row["sdio_af12_selected"] = str(sdio_af12)
    row["takeover_error_code"] = str(get_int(enter_values, "last_takeover_error_code"))
    row["takeover_error_text"] = enter_values.get("last_takeover_error_text", "UNKNOWN")
    row["snapshot_pause_confirmed"] = str(get_int(enter_values, "snapshot_pause_confirmed"))
    row["conflict_pin_release_ready"] = str(get_int(enter_values, "conflict_pin_release_ready"))
    ok = "SD TAKEOVER ENTER: full SDIO GPIO switched to AF12" in enter_text and full_gpio_af12 == 1 and sdio_af12 == 1
    row["takeover_enter"] = "PASS" if ok else "FAIL"
    row["takeover_enter_result"] = row["takeover_enter"]
    return ok


def run_atk1b_init(session: UartSession, row: Dict[str, str], log: List[str]) -> bool:
    """执行 ATK 1-bit SD 初始化步骤。"""
    init_text = safe_cli(session, "SD ATK1BINIT", log)
    init_status_text = safe_cli(session, "SD ATK1BSTATUS", log)
    init_values = parse_kv(init_text + "\n" + init_status_text)
    init_field_map = {
        "atk1b_init_ready": "atk_1bit_init_ready",
        "atk1b_hal_init_status": "atk_1bit_hal_init_status",
        "atk1b_hal_error": "atk_1bit_hal_error",
        "atk1b_cardinfo_status": "atk_1bit_cardinfo_status",
        "atk1b_cardinfo_error": "atk_1bit_cardinfo_error",
        "atk1b_wait_transfer_attempt_count": "atk_1bit_wait_transfer_attempt_count",
        "atk1b_wait_transfer_success_count": "atk_1bit_wait_transfer_success_count",
        "atk1b_wait_transfer_error_count": "atk_1bit_wait_transfer_error_count",
        "atk1b_last_card_state": "atk_1bit_last_card_state",
        "atk1b_last_operation_ms": "atk_1bit_last_operation_ms",
        "atk1b_clock_div": "atk_1bit_clock_div",
        "atk1b_bus_width": "atk_1bit_bus_width",
    }
    for csv_field, status_field in init_field_map.items():
        row[csv_field] = str(get_int(init_values, status_field))
    ok = "SD ATK1BINIT: HAL_SD_Init OK" in init_text or get_int(init_values, "atk_1bit_init_ready", 0) == 1
    row["atk1b_init"] = "PASS" if ok else "FAIL"
    row["atk1b_init_result"] = row["atk1b_init"]
    return ok


def read_block(session: UartSession, block: int) -> Dict[str, str]:
    """执行一次指定地址的 SD 读块测试。"""
    read_text = safe_cli(session, "SD ATK1BREAD %d" % block, session.log)
    status_text = safe_cli(session, "SD ATK1BSTATUS", session.log)
    combined = read_text + "\n" + status_text
    values = parse_kv(combined)
    return {
        "result": classify_read(combined, values),
        "error": str(get_int(values, "atk_1bit_last_read_error")),
        "size": str(get_int(values, "atk_1bit_last_read_size")),
        "sum512": str(get_int(values, "atk_1bit_buffer_sum512")),
    }


def put_read_result(row: Dict[str, str], block: int, result: Dict[str, str]) -> None:
    """把读块结果写入当前循环记录。"""
    if block not in (0, 2048):
        # 为了保持 CSV 表头简单，额外 block 只写入 log，不写入 CSV 汇总字段。
        return
    row["read_block_%d_result" % block] = result["result"]
    row["read_block_%d_error" % block] = result["error"]
    row["read_block_%d_size" % block] = result["size"]
    row["read_block_%d_sum512" % block] = result["sum512"]


def run_takeover_exit(session: UartSession, row: Dict[str, str], log: List[str]) -> bool:
    """执行 SDIO takeover 退出步骤。"""
    exit_text = safe_cli(session, "SD TAKEOVER EXIT", log)
    ok = "full SDIO GPIO restored, conflict pins restored to DCMI AF13" in exit_text or (
            "SD TAKEOVER EXIT" in exit_text and "restored" in exit_text
    )
    row["takeover_exit"] = "PASS" if ok else "FAIL"
    return ok


def run_dvprestore(session: UartSession, row: Dict[str, str], log: List[str], saved_3018: int) -> bool:
    """执行 DVP 输出恢复步骤。"""
    dvp_restore_text = safe_cli(session, "SD DVPRESTORE", log)
    dvp_status_text = safe_cli(session, "SD DVPSTATUS", log)
    values = parse_kv(dvp_restore_text + "\n" + dvp_status_text)
    restore_after = get_int(values, "dvp_restore_reg_3018_after")
    row["dvp_mask_active_after_restore"] = str(get_int(values, "dvp_mask_active"))
    row["dvp_restore_before"] = str(get_int(values, "dvp_restore_reg_3018_before"))
    row["dvp_restore_written"] = str(get_int(values, "dvp_restore_reg_3018_written"))
    row["dvp_restore_after"] = str(restore_after)
    row["dvp_restore_error_code"] = str(get_int(values, "dvp_mask_last_error_code"))
    row["dvp_restore_error_text"] = values.get("dvp_mask_last_error_text", "UNKNOWN")
    ok = (
            "SD DVPRESTORE: OV5640 DVP pad output restored" in dvp_restore_text
            and get_int(values, "dvp_mask_active") == 0
            and 0 <= saved_3018 <= 0xFF
            and restore_after == saved_3018
    )
    row["dvprestore"] = "PASS" if ok else "FAIL"
    row["dvprestore_result"] = row["dvprestore"]
    return ok


def run_snapshot_restore(session: UartSession, row: Dict[str, str], log: List[str]) -> bool:
    """执行 SNAPSHOT RESTORE 恢复步骤。"""
    text = safe_cli(session, "SNAPSHOT RESTORE", log)
    # 当前固件会返回 deferred，也算 guard 已解除的预期行为。
    ok = "SNAPSHOT RESTORE" in text
    row["snapshot_restore"] = "PASS" if ok else "FAIL"
    return ok


def run_status(session: UartSession, row: Dict[str, str], log: List[str]) -> bool:
    """读取并解析缓存状态。"""
    status_text = safe_cli(session, "STATUS", log)
    values = parse_kv(status_text)
    health_values = {
        "iwdg_skip": get_int(values, "iwdg_refresh_skip_count"),
        "hook_fault": get_int(values, "hook_fault_code"),
        "uart_dma_error": get_int(values, "uart_dma_error_count"),
        "stream_overflow": get_int(values, "stream_buffer_overflow_bytes"),
    }
    for key, value in health_values.items():
        row[key] = str(value)
    ok = bool(status_text.strip()) and all(value >= 0 for value in health_values.values())
    row["system_status"] = "PASS" if ok else "FAIL"
    return ok


def run_image_request(session: UartSession, row: Dict[str, str], args: argparse.Namespace, seq: int, log: List[str]) -> bool:
    """执行一次恢复后的图像请求验证。"""
    try:
        frame, rx_len = session.image_request(seq, args.image_timeout)
        image_ok, crc_ok, frame_id, image_error = validate_image_frame(frame)
    except Exception as exc:
        frame = b""
        rx_len = 0
        image_ok = False
        crc_ok = False
        frame_id = None
        image_error = repr(exc)
        log.append("IMAGE seq=%d 异常：%s" % (seq, image_error))
    row["image_result"] = "IMAGE_PASS" if image_ok else "IMAGE_FAIL"
    row["image_rx_len"] = str(rx_len)
    row["image_error"] = image_error if image_error else "NONE"
    row["image_magic_found"] = "1" if IMAGE_MAGIC in frame else "0"
    row["image_crc_ok"] = "1" if crc_ok else "0"
    row["image_frame_id"] = str(frame_id) if frame_id is not None else "-1"
    if image_ok:
        log.append("IMAGE seq=%d frame_id=%d len=%d CRC=PASS" % (seq, frame_id, rx_len))
    else:
        log.append("IMAGE seq=%d len=%d FAIL=%s" % (seq, rx_len, image_error))
    return image_ok


def collect_errors(row: Dict[str, str], blocks: Sequence[int], no_image: bool) -> List[str]:
    """收集本轮测试的失败原因。"""
    errors: List[str] = []
    checks = [
        ("snapshot_prepare", "PASS", "SNAPSHOT_PREPARE"),
        ("dvpstop", "PASS", "DVPSTOP_FAIL"),
        ("takeover_enter", "PASS", "TAKEOVER_ENTER_FAIL"),
        ("atk1b_init", "PASS", "ATK1B_INIT_FAIL"),
        ("takeover_exit", "PASS", "TAKEOVER_EXIT"),
        ("dvprestore", "PASS", "DVPRESTORE_FAIL"),
        ("snapshot_restore", "PASS", "SNAPSHOT_RESTORE"),
        ("system_status", "PASS", "SYSTEM_STATUS"),
    ]
    for field, expected, reason in checks:
        if row.get(field) != expected:
            errors.append(reason)
    for block in blocks:
        if block in (0, 2048) and row.get("read_block_%d_result" % block) != "PASS":
            errors.append("READ_BLOCK_%d" % block)
    if row.get("dvp_mask_after") != "143":
        errors.append("DVP_MASK_AFTER")
    if row.get("dvp_restore_after") != row.get("dvp_mask_saved"):
        errors.append("DVP_RESTORE_AFTER")
    for key in ("iwdg_skip", "hook_fault", "uart_dma_error", "stream_overflow"):
        try:
            if int(row.get(key, "-1"), 0) != 0:
                errors.append(key.upper())
        except ValueError:
            errors.append(key.upper())
    if not no_image and row.get("image_result") != "IMAGE_PASS":
        errors.append("IMAGE_FAIL")
    return errors


def finalize_cycle(row: Dict[str, str], blocks: Sequence[int], no_image: bool) -> None:
    """汇总并定稿一轮循环测试结果。"""
    errors = collect_errors(row, blocks, no_image)
    row["errors"] = " | ".join(errors)
    row["cycle_result"] = "PASS" if not errors else "FAIL"


def log_cycle_diagnostics(row: Dict[str, str], log: List[str]) -> None:
    """记录本轮循环的关键诊断字段。"""
    log.append("\nCYCLE %s DIAGNOSTICS:" % row["cycle"])
    for field in CSV_FIELDS:
        if field not in ("timestamp", "mode"):
            log.append("  %s=%s" % (field, row.get(field, "")))


def print_cycle_progress(cycle: int, total: int, row: Dict[str, str]) -> None:
    """输出当前循环的精简进度。"""
    print(
        "[%02d/%02d] DVP=%s TAKEOVER=%s INIT=%s READ0=%s READ2048=%s "
        "RESTORE=%s IMAGE=%s frame_id=%s CYCLE=%s"
        % (
            cycle,
            total,
            row["dvpstop"],
            row["takeover_enter"],
            row["atk1b_init"],
            row["read_block_0_result"],
            row["read_block_2048_result"],
            row["dvprestore"],
            row["image_result"],
            row["image_frame_id"],
            row["cycle_result"],
        )
    )
    if row["errors"]:
        print("  failures=" + row["errors"])
    if row["atk1b_init"] != "PASS":
        print(
            "  atk1b_diag ready=%s hal_status=%s hal_error=%s "
            "cardinfo_status=%s cardinfo_error=%s card_state=%s"
            % (
                row["atk1b_init_ready"],
                row["atk1b_hal_init_status"],
                row["atk1b_hal_error"],
                row["atk1b_cardinfo_status"],
                row["atk1b_cardinfo_error"],
                row["atk1b_last_card_state"],
            )
        )


def run_cycle_mode(session: UartSession, args: argparse.Namespace, log: List[str]) -> List[Dict[str, str]]:
    """以每轮重开会话的方式执行循环测试。"""
    rows: List[Dict[str, str]] = []
    for cycle in range(1, args.cycles + 1):
        row = make_row("cycle", cycle)
        if not args.no_pre_cleanup:
            row["pre_cleanup"] = best_effort_cleanup(
                session, log, "CYCLE %d DEFENSIVE PRE-CLEANUP" % cycle, args.cleanup_wait
            )
        else:
            row["pre_cleanup"] = "SKIP"
        sleep_and_log(log, "CYCLE %d cycle-gap" % cycle, args.cycle_gap)

        saved_3018 = -1
        if not args.no_sd_init_before_cycle:
            run_sd_init_deferred(session, row, log)
        else:
            row["sd_init_blocked"] = "SKIP"

        if run_prepare(session, row, log):
            dvp_ok, saved_3018 = run_dvpstop(session, row, log)
            sleep_and_log(log, "CYCLE %d delay-after-dvpstop" % cycle, args.delay_after_dvpstop)
            if dvp_ok and run_takeover(session, row, log):
                sleep_and_log(log, "CYCLE %d delay-after-takeover" % cycle, args.delay_after_takeover)
                sleep_and_log(log, "CYCLE %d delay-before-atkinit" % cycle, args.delay_before_atkinit)
                if run_atk1b_init(session, row, log):
                    sleep_and_log(log, "CYCLE %d delay-after-atkinit" % cycle, args.delay_after_atkinit)
                    for block in args.blocks:
                        result = read_block(session, block)
                        put_read_result(row, block, result)

        run_takeover_exit(session, row, log)
        run_dvprestore(session, row, log, saved_3018)
        sleep_and_log(log, "CYCLE %d delay-after-dvprestore" % cycle, args.delay_after_dvprestore)
        run_snapshot_restore(session, row, log)
        sleep_and_log(log, "CYCLE %d restore-wait" % cycle, args.restore_wait)
        run_status(session, row, log)

        if args.no_image:
            row["image_result"] = "IMAGE_SKIP"
            row["image_error"] = "SKIP_BY_OPTION"
        else:
            sleep_and_log(log, "CYCLE %d image-wait" % cycle, args.image_wait)
            run_image_request(session, row, args, cycle & 0xFFFF, log)

        finalize_cycle(row, args.blocks, args.no_image)
        log_cycle_diagnostics(row, log)
        rows.append(row)
        print_cycle_progress(cycle, args.cycles, row)
    return rows


def run_single_session_mode(session: UartSession, args: argparse.Namespace, log: List[str]) -> List[Dict[str, str]]:
    """在同一串口会话中执行全部循环。"""
    rows: List[Dict[str, str]] = []
    base_row = make_row("single-session", 1)
    base_row["pre_cleanup"] = "NOT_RUN"
    saved_3018 = -1

    run_sd_init_deferred(session, base_row, log)
    if run_prepare(session, base_row, log):
        dvp_ok, saved_3018 = run_dvpstop(session, base_row, log)
        sleep_and_log(log, "single-session delay-after-dvpstop", args.delay_after_dvpstop)
        if dvp_ok and run_takeover(session, base_row, log):
            sleep_and_log(log, "single-session delay-after-takeover", args.delay_after_takeover)
            sleep_and_log(log, "single-session delay-before-atkinit", args.delay_before_atkinit)
            if run_atk1b_init(session, base_row, log):
                sleep_and_log(log, "single-session delay-after-atkinit", args.delay_after_atkinit)
                for block in args.blocks:
                    pass_count = 0
                    fail_count = 0
                    last_result: Optional[Dict[str, str]] = None
                    for index in range(1, args.read_count + 1):
                        result = read_block(session, block)
                        last_result = result
                        if result["result"] == "PASS":
                            pass_count += 1
                        else:
                            fail_count += 1
                        read_row = dict(base_row)
                        read_row["read_index"] = "%d:%d" % (block, index)
                        put_read_result(read_row, block, result)
                        read_row["cycle_result"] = "PASS" if result["result"] == "PASS" else "FAIL"
                        read_row["errors"] = "" if result["result"] == "PASS" else "READ_BLOCK_%d" % block
                        rows.append(read_row)
                        print(
                            "[single %s %02d/%02d] result=%s error=%s size=%s sum512=%s"
                            % (
                                block,
                                index,
                                args.read_count,
                                result["result"],
                                result["error"],
                                result["size"],
                                result["sum512"],
                            )
                        )
                    if last_result is not None:
                        put_read_result(base_row, block, last_result)
                    log.append(
                        "single-session block=%d pass=%d fail=%d total=%d"
                        % (block, pass_count, fail_count, args.read_count)
                    )

    run_takeover_exit(session, base_row, log)
    run_dvprestore(session, base_row, log, saved_3018)
    sleep_and_log(log, "single-session delay-after-dvprestore", args.delay_after_dvprestore)
    run_snapshot_restore(session, base_row, log)
    sleep_and_log(log, "single-session restore-wait", args.restore_wait)
    run_status(session, base_row, log)

    if args.no_image:
        base_row["image_result"] = "IMAGE_SKIP"
        base_row["image_error"] = "SKIP_BY_OPTION"
    else:
        sleep_and_log(log, "single-session image-wait", args.image_wait)
        run_image_request(session, base_row, args, 1, log)

    # 在 single-session 模式下，base_row 作为“流程总行”；read_row 作为每次读明细行。
    finalize_cycle(base_row, args.blocks, args.no_image)
    base_row["read_index"] = "summary"
    rows.insert(0, base_row)
    log_cycle_diagnostics(base_row, log)
    print_cycle_progress(1, 1, base_row)
    return rows


def build_parser() -> argparse.ArgumentParser:
    """创建并配置命令行参数解析器。"""
    parser = argparse.ArgumentParser(
        description="Stage 11C-5U-2 DVP mask 稳定性脚本，支持 single-session / cycle 对照"
    )
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mode", choices=("cycle", "single-session"), default="cycle")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--read-count", type=int, default=20)
    parser.add_argument("--blocks", type=parse_blocks, default=parse_blocks("0,2048"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--image-timeout", type=float, default=10.0)
    parser.add_argument("--cycle-gap", type=float, default=1.0)
    parser.add_argument("--restore-wait", type=float, default=1.0)
    parser.add_argument("--image-wait", type=float, default=1.0)
    parser.add_argument("--cleanup-wait", type=float, default=0.5)
    parser.add_argument("--delay-after-dvpstop", type=float, default=0.5)
    parser.add_argument("--delay-after-takeover", type=float, default=0.2)
    parser.add_argument("--delay-before-atkinit", type=float, default=0.5)
    parser.add_argument("--delay-after-atkinit", type=float, default=0.2)
    parser.add_argument("--delay-after-dvprestore", type=float, default=1.0)
    parser.add_argument("--no-pre-cleanup", action="store_true")
    parser.add_argument("--no-image", action="store_true")
    parser.add_argument("--no-sd-init-before-cycle", action="store_true")
    parser.add_argument("--tag", type=sanitize_tag, default="dvp_mask_cycle")
    parser.add_argument("--captures", default="captures")
    return parser


def validate_args(args: argparse.Namespace) -> None:
    """校验循环测试参数范围。"""
    if args.cycles <= 0:
        raise SystemExit("--cycles 必须大于 0")
    if args.read_count <= 0:
        raise SystemExit("--read-count 必须大于 0")
    if args.baud <= 0:
        raise SystemExit("--baud 必须大于 0")
    if args.timeout <= 0.0:
        raise SystemExit("--timeout 必须大于 0")
    if args.image_timeout <= 0.0:
        raise SystemExit("--image-timeout 必须大于 0")
    for name, value in (
            ("cycle-gap", args.cycle_gap),
            ("restore-wait", args.restore_wait),
            ("image-wait", args.image_wait),
            ("cleanup-wait", args.cleanup_wait),
            ("delay-after-dvpstop", args.delay_after_dvpstop),
            ("delay-after-takeover", args.delay_after_takeover),
            ("delay-before-atkinit", args.delay_before_atkinit),
            ("delay-after-atkinit", args.delay_after_atkinit),
            ("delay-after-dvprestore", args.delay_after_dvprestore),
    ):
        if value < 0.0:
            raise SystemExit("--%s 不能小于 0" % name)
    missing_required_blocks = [block for block in (0, 2048) if block not in args.blocks]
    if missing_required_blocks:
        raise SystemExit("--blocks 必须包含单轮判定所需的 0,2048")


def count_rows(rows: Sequence[Dict[str, str]], field: str, value: str, only_summary: bool = True) -> int:
    """统计 CSV 中已记录的数据行数。"""
    selected = rows
    if only_summary:
        selected = [row for row in rows if row.get("read_index", "") in ("", "summary")]
    return sum(row.get(field) == value for row in selected)


def summarize(rows: Sequence[Dict[str, str]], args: argparse.Namespace, paths: Tuple[Path, Path, Path]) -> Dict[str, str]:
    """汇总全部循环并生成统计结果。"""
    summary_rows = [row for row in rows if row.get("read_index", "") in ("", "summary")]
    if not summary_rows:
        summary_rows = list(rows)
    requested_cycles = 1 if args.mode == "single-session" else args.cycles
    pass_count = sum(row["cycle_result"] == "PASS" for row in summary_rows)
    fail_count = max(0, requested_cycles - pass_count)
    frame_ids = [
        int(row["image_frame_id"])
        for row in summary_rows
        if row["image_result"] == "IMAGE_PASS" and row["image_frame_id"].isdecimal()
    ]

    def nonzero_count(field: str) -> int:
        """统计指定字段中非零结果的数量。"""
        total = 0
        for row in summary_rows:
            try:
                total += int(row.get(field, "0"), 0) != 0
            except ValueError:
                total += 1
        return total

    failed_cycles = [row["cycle"] for row in summary_rows if row["cycle_result"] != "PASS"]

    # 单次会话模式下，block 计数按每次读明细行统计；cycle 模式按每轮统计。
    if args.mode == "single-session":
        block0_total = sum(row.get("read_index", "").startswith("0:") for row in rows)
        block0_pass = sum(row.get("read_index", "").startswith("0:") and row.get("read_block_0_result") == "PASS" for row in rows)
        block2048_total = sum(row.get("read_index", "").startswith("2048:") for row in rows)
        block2048_pass = sum(
            row.get("read_index", "").startswith("2048:") and row.get("read_block_2048_result") == "PASS" for row in rows
        )
    else:
        block0_total = requested_cycles
        block0_pass = count_rows(summary_rows, "read_block_0_result", "PASS", only_summary=False)
        block2048_total = requested_cycles
        block2048_pass = count_rows(summary_rows, "read_block_2048_result", "PASS", only_summary=False)

    csv_path, log_path, summary_path = paths
    result = {
        "MODE": args.mode,
        "NO_PRE_CLEANUP": "1" if args.no_pre_cleanup else "0",
        "NO_IMAGE": "1" if args.no_image else "0",
        "NO_SD_INIT_BEFORE_CYCLE": "1" if args.no_sd_init_before_cycle else "0",
        "DELAY_AFTER_DVPSTOP": str(args.delay_after_dvpstop),
        "DELAY_AFTER_TAKEOVER": str(args.delay_after_takeover),
        "DELAY_BEFORE_ATKINIT": str(args.delay_before_atkinit),
        "DELAY_AFTER_ATKINIT": str(args.delay_after_atkinit),
        "DELAY_AFTER_DVPRESTORE": str(args.delay_after_dvprestore),
        "CYCLES_TOTAL": str(requested_cycles),
        "CYCLES_PASS": str(pass_count),
        "CYCLES_FAIL": str(fail_count),
        "CYCLE_PASS_RATE": "%.2f%%" % (100.0 * pass_count / max(1, requested_cycles)),
        "DVPSTOP_PASS_COUNT": str(count_rows(summary_rows, "dvpstop", "PASS", only_summary=False)),
        "DVPSTOP_FAIL_COUNT": str(requested_cycles - count_rows(summary_rows, "dvpstop", "PASS", only_summary=False)),
        "TAKEOVER_ENTER_PASS_COUNT": str(count_rows(summary_rows, "takeover_enter", "PASS", only_summary=False)),
        "TAKEOVER_ENTER_FAIL_COUNT": str(requested_cycles - count_rows(summary_rows, "takeover_enter", "PASS", only_summary=False)),
        "ATK1B_INIT_PASS_COUNT": str(count_rows(summary_rows, "atk1b_init", "PASS", only_summary=False)),
        "ATK1B_INIT_FAIL_COUNT": str(requested_cycles - count_rows(summary_rows, "atk1b_init", "PASS", only_summary=False)),
        "BLOCK_0_PASS_COUNT": str(block0_pass),
        "BLOCK_0_FAIL_COUNT": str(max(0, block0_total - block0_pass)),
        "BLOCK_0_PASS_RATE": "%.2f%%" % (100.0 * block0_pass / max(1, block0_total)),
        "BLOCK_2048_PASS_COUNT": str(block2048_pass),
        "BLOCK_2048_FAIL_COUNT": str(max(0, block2048_total - block2048_pass)),
        "BLOCK_2048_PASS_RATE": "%.2f%%" % (100.0 * block2048_pass / max(1, block2048_total)),
        "DVPRESTORE_PASS_COUNT": str(count_rows(summary_rows, "dvprestore", "PASS", only_summary=False)),
        "DVPRESTORE_FAIL_COUNT": str(requested_cycles - count_rows(summary_rows, "dvprestore", "PASS", only_summary=False)),
        "SNAPSHOT_RESTORE_PASS_COUNT": str(count_rows(summary_rows, "snapshot_restore", "PASS", only_summary=False)),
        "SNAPSHOT_RESTORE_FAIL_COUNT": str(requested_cycles - count_rows(summary_rows, "snapshot_restore", "PASS", only_summary=False)),
        "IMAGE_PASS_COUNT": str(count_rows(summary_rows, "image_result", "IMAGE_PASS", only_summary=False)),
        "IMAGE_FAIL_COUNT": str(count_rows(summary_rows, "image_result", "IMAGE_FAIL", only_summary=False)),
        "IMAGE_SKIP_COUNT": str(count_rows(summary_rows, "image_result", "IMAGE_SKIP", only_summary=False)),
        "IMAGE_FRAME_ID_FIRST": str(frame_ids[0]) if frame_ids else "-1",
        "IMAGE_FRAME_ID_LAST": str(frame_ids[-1]) if frame_ids else "-1",
        "IMAGE_FRAME_ID_CONTINUOUS": "YES" if check_frame_ids_continuous(frame_ids) else "NO",
        "IWDG_SKIP_COUNT": str(nonzero_count("iwdg_skip")),
        "HOOK_FAULT_COUNT": str(nonzero_count("hook_fault")),
        "UART_DMA_ERROR_COUNT": str(nonzero_count("uart_dma_error")),
        "STREAM_OVERFLOW_COUNT": str(nonzero_count("stream_overflow")),
        "FAILED_CYCLES": ",".join(failed_cycles),
        "ATK1B_HAL_ERROR_VALUES": unique_values(summary_rows, "atk1b_hal_error"),
        "ATK1B_HAL_STATUS_VALUES": unique_values(summary_rows, "atk1b_hal_init_status"),
        "ATK1B_CARDINFO_STATUS_VALUES": unique_values(summary_rows, "atk1b_cardinfo_status"),
        "ATK1B_LAST_CARD_STATE_VALUES": unique_values(summary_rows, "atk1b_last_card_state"),
        "ATK1B_INIT_READY_VALUES": unique_values(summary_rows, "atk1b_init_ready"),
        "ATK1B_FAILED_CYCLES": failed_cycle_values(summary_rows, "atk1b_init_result", "PASS"),
        "DVP_MASK_AFTER_VALUES": unique_values(summary_rows, "dvp_mask_after"),
        "DVP_MASK_SAVED_VALUES": unique_values(summary_rows, "dvp_mask_saved"),
        "DVP_MASK_FAILED_CYCLES": failed_cycle_values(summary_rows, "dvpstop_result", "PASS"),
        "TAKEOVER_ERROR_CODE_VALUES": unique_values(summary_rows, "takeover_error_code"),
        "TAKEOVER_ERROR_TEXT_VALUES": unique_values(summary_rows, "takeover_error_text"),
        "SDIO_FULL_GPIO_AF12_SELECTED_VALUES": unique_values(summary_rows, "sdio_full_gpio_af12_selected"),
        "DVP_RESTORE_AFTER_VALUES": unique_values(summary_rows, "dvp_restore_after"),
        "DVP_RESTORE_FAILED_CYCLES": failed_cycle_values(summary_rows, "dvprestore_result", "PASS"),
        "IMAGE_FAILED_CYCLES": failed_cycle_values(summary_rows, "image_result", "IMAGE_PASS") if not args.no_image else "",
        "IMAGE_RX_LEN_VALUES": unique_values(summary_rows, "image_rx_len"),
        "IMAGE_ERROR_VALUES": unique_values(summary_rows, "image_error"),
        "FAIL_REASON_COUNTS": fail_reason_counts(summary_rows),
        "CSV_FILE": str(csv_path),
        "LOG_FILE": str(log_path),
        "SUMMARY_FILE": str(summary_path),
    }
    return result


def write_outputs(rows: Sequence[Dict[str, str]], summary: Dict[str, str], paths: Tuple[Path, Path, Path], log: List[str]) -> None:
    """写出 CSV、摘要和测试日志。"""
    csv_path, log_path, summary_path = paths
    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    log_path.write_text("\n".join(log) + "\n", encoding="utf-8")
    summary_text = "\n".join("%s=%s" % (key, value) for key, value in summary.items())
    summary_path.write_text(summary_text + "\n", encoding="utf-8")
    print("\n" + summary_text)


def main() -> int:
    """解析参数并执行完整测试流程。"""
    args = build_parser().parse_args()
    validate_args(args)

    output_dir = Path(args.captures)
    output_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    stem = "sd_%s_%s" % (args.tag, timestamp)
    csv_path = output_dir / (stem + ".csv")
    log_path = output_dir / (stem + "_log.txt")
    summary_path = output_dir / (stem + "_summary.txt")
    paths = (csv_path, log_path, summary_path)

    log: List[str] = []
    rows: List[Dict[str, str]] = []
    session: Optional[UartSession] = None
    need_final_cleanup = False

    try:
        session = UartSession(args.port, args.baud, args.timeout, log)
        if args.mode == "single-session":
            rows = run_single_session_mode(session, args, log)
        else:
            rows = run_cycle_mode(session, args, log)
        need_final_cleanup = any(row.get("cycle_result") == "FAIL" for row in rows if row.get("read_index", "") in ("", "summary"))
    except Exception as exc:
        log.append("\n脚本异常：%r" % (exc,))
        need_final_cleanup = True
    finally:
        if session is not None:
            if need_final_cleanup:
                best_effort_cleanup(session, log, "FINAL CLEANUP AFTER FAILURE", args.cleanup_wait)
            try:
                session.close()
            except Exception as exc:
                log.append("关闭串口异常：%r" % (exc,))

        summary = summarize(rows, args, paths)
        write_outputs(rows, summary, paths, log)

    summary_rows = [row for row in rows if row.get("read_index", "") in ("", "summary")]
    if not summary_rows:
        return 1
    all_ok = all(row.get("cycle_result") == "PASS" for row in summary_rows)
    if not args.no_image:
        frame_ids = [
            int(row["image_frame_id"])
            for row in summary_rows
            if row.get("image_frame_id", "").isdecimal()
        ]
        all_ok = all_ok and check_frame_ids_continuous(frame_ids)
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
