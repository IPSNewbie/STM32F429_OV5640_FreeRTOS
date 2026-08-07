#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stage 11C-5N: ATK official 1-bit polling read repeat test.

This script only drives text CLI diagnostics. It does not issue a binary image
request, write the SD card, or change firmware configuration.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence

try:
    import serial  # type: ignore
except ImportError:
    serial = None  # type: ignore


CSV_FIELDS = [
    "timestamp",
    "block",
    "index",
    "result",
    "read_status",
    "read_error",
    "read_addr",
    "read_count",
    "data_crc",
    "cmd_crc",
    "cmd_rsp_timeout",
    "data_timeout",
    "rx_overrun",
    "tx_underrun",
    "read_size",
    "pre_card_state",
    "post_card_state",
    "wait_card_state",
    "read_ms",
    "wait_ms",
    "wait_timeout_ms",
    "buffer_inspected",
    "buffer_len",
    "sum512",
    "xor512",
    "nonzero_count512",
    "zero_count512",
    "ff_count512",
    "prefill_count512",
    "changed_count512",
    "buffer_changed",
    "all_prefill",
    "all_zero",
    "all_ff",
    "first_changed_index",
    "last_changed_index",
    "first16",
    "first32",
    "tail16",
]

BLOCK_FINGERPRINT_FIELDS = [
    ("SUM512", "sum512"),
    ("FIRST32", "first32"),
    ("TAIL16", "tail16"),
    ("CHANGED_COUNT", "changed_count512"),
    ("ZERO_COUNT", "zero_count512"),
    ("FF_COUNT", "ff_count512"),
]

FULL_GPIO_READBACK_FIELDS = [
    ("SDIO_FULL_GPIO_LAST_ERROR_PIN", "sdio_full_gpio_last_error_pin"),
    ("SDIO_FULL_GPIO_PC8_MODE", "sdio_full_gpio_pc8_mode"),
    ("SDIO_FULL_GPIO_PC8_PULL", "sdio_full_gpio_pc8_pull"),
    ("SDIO_FULL_GPIO_PC8_SPEED", "sdio_full_gpio_pc8_speed"),
    ("SDIO_FULL_GPIO_PC8_AF", "sdio_full_gpio_pc8_af"),
    ("SDIO_FULL_GPIO_PC9_MODE", "sdio_full_gpio_pc9_mode"),
    ("SDIO_FULL_GPIO_PC9_PULL", "sdio_full_gpio_pc9_pull"),
    ("SDIO_FULL_GPIO_PC9_SPEED", "sdio_full_gpio_pc9_speed"),
    ("SDIO_FULL_GPIO_PC9_AF", "sdio_full_gpio_pc9_af"),
    ("SDIO_FULL_GPIO_PC10_MODE", "sdio_full_gpio_pc10_mode"),
    ("SDIO_FULL_GPIO_PC10_PULL", "sdio_full_gpio_pc10_pull"),
    ("SDIO_FULL_GPIO_PC10_SPEED", "sdio_full_gpio_pc10_speed"),
    ("SDIO_FULL_GPIO_PC10_AF", "sdio_full_gpio_pc10_af"),
    ("SDIO_FULL_GPIO_PC11_MODE", "sdio_full_gpio_pc11_mode"),
    ("SDIO_FULL_GPIO_PC11_PULL", "sdio_full_gpio_pc11_pull"),
    ("SDIO_FULL_GPIO_PC11_SPEED", "sdio_full_gpio_pc11_speed"),
    ("SDIO_FULL_GPIO_PC11_AF", "sdio_full_gpio_pc11_af"),
    ("SDIO_FULL_GPIO_PC12_MODE", "sdio_full_gpio_pc12_mode"),
    ("SDIO_FULL_GPIO_PC12_PULL", "sdio_full_gpio_pc12_pull"),
    ("SDIO_FULL_GPIO_PC12_SPEED", "sdio_full_gpio_pc12_speed"),
    ("SDIO_FULL_GPIO_PC12_AF", "sdio_full_gpio_pc12_af"),
    ("SDIO_FULL_GPIO_PD2_MODE", "sdio_full_gpio_pd2_mode"),
    ("SDIO_FULL_GPIO_PD2_PULL", "sdio_full_gpio_pd2_pull"),
    ("SDIO_FULL_GPIO_PD2_SPEED", "sdio_full_gpio_pd2_speed"),
    ("SDIO_FULL_GPIO_PD2_AF", "sdio_full_gpio_pd2_af"),
]


def parse_blocks(text: str) -> List[int]:
    values: List[int] = []
    for item in text.split(","):
        token = item.strip()
        if not token or not token.isdecimal():
            raise argparse.ArgumentTypeError(
                "--blocks 仅支持逗号分隔的十进制非负 block 地址"
            )
        value = int(token, 10)
        if value > 0xFFFFFFFF:
            raise argparse.ArgumentTypeError("block 地址超出 uint32 范围")
        if value not in values:
            values.append(value)
    if not values:
        raise argparse.ArgumentTypeError("--blocks 不能为空")
    return values


def sanitize_tag(text: str) -> str:
    tag = re.sub(r"[^A-Za-z0-9_-]+", "_", text.strip()).strip("_")
    if not tag:
        raise argparse.ArgumentTypeError("--tag 必须包含字母、数字、下划线或连字符")
    return tag


def parse_kv(text: str) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r"\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def get_int(values: Dict[str, str], key: str, default: int = -1) -> int:
    text = values.get(key)
    if text is None:
        return default
    try:
        return int(text, 0)
    except ValueError:
        return default


def classify_read(text: str, values: Dict[str, str]) -> str:
    read_ok = "SD ATK1BREAD: block read OK" in text
    status_ok = (
        get_int(values, "atk_1bit_last_read_status") == 0
        and get_int(values, "atk_1bit_last_read_size") == 512
    )
    if read_ok or status_ok:
        return "PASS"
    if (
        get_int(values, "atk_1bit_read_error_is_data_crc_fail", 0) == 1
        or get_int(values, "atk_1bit_last_read_error") == 2
    ):
        return "DATA_CRC_FAIL"
    if get_int(values, "atk_1bit_init_ready", 0) == 0:
        return "NOT_READY"
    return "OTHER_FAIL"


def ordered_unique(values: Sequence[str]) -> List[str]:
    result: List[str] = []
    seen = set()
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


class UartSession:
    def __init__(
        self,
        port: str,
        baud: int,
        command_timeout: float,
        log: List[str],
    ) -> None:
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
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        time.sleep(0.2)
        startup = self.drain(0.8)
        if startup:
            self.log.append("===== STARTUP RX =====")
            self.log.append(startup.decode("utf-8", errors="replace").rstrip())

    def close(self) -> None:
        if self.ser.is_open:
            self.ser.close()

    def drain(self, max_time: float = 0.2) -> bytes:
        deadline = time.monotonic() + max_time
        chunks: List[bytes] = []
        while time.monotonic() < deadline:
            data = self.ser.read(self.ser.in_waiting or 1)
            if data:
                chunks.append(data)
                deadline = time.monotonic() + 0.1
        return b"".join(chunks)

    def read_until_quiet(self) -> bytes:
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
        return b"".join(chunks)

    def cli(self, command: str) -> str:
        stale = self.drain()
        if stale:
            self.log.append("===== RX DRAINED BEFORE %s =====" % command)
            self.log.append(stale.decode("utf-8", errors="replace").rstrip())
        self.log.append(
            "\n===== %s CLI >>> %s ====="
            % (dt.datetime.now().isoformat(timespec="seconds"), command)
        )
        self.ser.write((command + "\r\n").encode("ascii"))
        self.ser.flush()
        text = self.read_until_quiet().decode("utf-8", errors="replace")
        self.log.append(text.rstrip())
        return text


def make_summary() -> Dict[str, str]:
    summary: Dict[str, str] = {
        "SD_ONLY_BOOT": "-1",
        "SD_ONLY_BOOT_SUPPORTED": "-1",
        "PRE_TAKEOVER_SD_INIT_BLOCKED": "UNKNOWN",
        "SNAPSHOT_PREPARE": "UNKNOWN",
        "TAKEOVER_ENTER": "UNKNOWN",
        "TAKEOVER_ENTER_ERROR_CODE": "-1",
        "TAKEOVER_ENTER_ERROR_TEXT": "UNKNOWN",
        "TAKEOVER_PRECHECK_SUCCESS": "-1",
        "SNAPSHOT_PAUSE_CONFIRMED": "-1",
        "CONFLICT_PIN_RELEASE_READY": "-1",
        "SDIO_FULL_GPIO_AF12_SELECTED": "-1",
        "SDIO_AF12_SELECTED": "-1",
        "ATK1B_INIT": "UNKNOWN",
        "ATK1B_INIT_READY": "-1",
        "ATK1B_CLOCK_DIV": "-1",
        "ATK1B_BUS_WIDTH": "-1",
        "ATK1B_LAST_CARD_STATE": "-1",
        "READ_REPEAT": "SKIP",
    }
    for summary_key, _ in FULL_GPIO_READBACK_FIELDS:
        summary[summary_key] = "-1"
    for block in (0, 2048):
        add_empty_block_summary(summary, block)
    summary.update(
        {
            "DUMP_GUARD": "UNKNOWN",
            "TAKEOVER_EXIT": "UNKNOWN",
            "SNAPSHOT_RESTORE": "UNKNOWN",
            "SYSTEM_STATUS": "UNKNOWN",
            "IWDG_SKIP": "-1",
            "HOOK_FAULT": "-1",
            "UART_DMA_ERROR": "-1",
            "STREAM_OVERFLOW": "-1",
        }
    )
    return summary


def add_empty_block_summary(summary: Dict[str, str], block: int) -> None:
    prefix = "BLOCK_%d_" % block
    summary[prefix + "RESULTS"] = ""
    summary[prefix + "PASS_COUNT"] = "0"
    summary[prefix + "DATA_CRC_FAIL_COUNT"] = "0"
    summary[prefix + "OTHER_FAIL_COUNT"] = "0"
    summary[prefix + "NOT_READY_COUNT"] = "0"
    summary[prefix + "TOTAL"] = "0"
    summary[prefix + "PASS_RATE"] = "0.00%"
    for label, _ in BLOCK_FINGERPRINT_FIELDS:
        summary[prefix + label + "_UNIQUE"] = "0"
        summary[prefix + label + "_VALUES"] = ""


def update_block_summary(
    summary: Dict[str, str],
    block: int,
    rows: Sequence[Dict[str, str]],
) -> None:
    prefix = "BLOCK_%d_" % block
    block_rows = [row for row in rows if row["block"] == str(block)]
    results = [row["result"] for row in block_rows]
    total = len(results)
    pass_count = results.count("PASS")
    summary[prefix + "RESULTS"] = ",".join(results)
    summary[prefix + "PASS_COUNT"] = str(pass_count)
    summary[prefix + "DATA_CRC_FAIL_COUNT"] = str(
        results.count("DATA_CRC_FAIL")
    )
    summary[prefix + "OTHER_FAIL_COUNT"] = str(results.count("OTHER_FAIL"))
    summary[prefix + "NOT_READY_COUNT"] = str(results.count("NOT_READY"))
    summary[prefix + "TOTAL"] = str(total)
    summary[prefix + "PASS_RATE"] = (
        "%.2f%%" % (100.0 * pass_count / total) if total else "0.00%"
    )
    for label, field in BLOCK_FINGERPRINT_FIELDS:
        unique = ordered_unique([row[field] for row in block_rows])
        summary[prefix + label + "_UNIQUE"] = str(len(unique))
        summary[prefix + label + "_VALUES"] = " | ".join(unique)


def read_once(session: UartSession, block: int, index: int) -> Dict[str, str]:
    read_text = session.cli("SD ATK1BREAD %d" % block)
    status_text = session.cli("SD ATK1BSTATUS")
    combined = read_text + "\n" + status_text
    values = parse_kv(combined)
    return {
        "timestamp": dt.datetime.now().isoformat(timespec="seconds"),
        "block": str(block),
        "index": str(index),
        "result": classify_read(combined, values),
        "read_status": str(get_int(values, "atk_1bit_last_read_status")),
        "read_error": str(get_int(values, "atk_1bit_last_read_error")),
        "read_addr": str(get_int(values, "atk_1bit_last_read_addr")),
        "read_count": str(get_int(values, "atk_1bit_last_read_count")),
        "data_crc": str(
            get_int(values, "atk_1bit_read_error_is_data_crc_fail")
        ),
        "cmd_crc": str(get_int(values, "atk_1bit_read_error_is_cmd_crc_fail")),
        "cmd_rsp_timeout": str(
            get_int(values, "atk_1bit_read_error_is_cmd_rsp_timeout")
        ),
        "data_timeout": str(
            get_int(values, "atk_1bit_read_error_is_data_timeout")
        ),
        "rx_overrun": str(
            get_int(values, "atk_1bit_read_error_is_rx_overrun")
        ),
        "tx_underrun": str(
            get_int(values, "atk_1bit_read_error_is_tx_underrun")
        ),
        "read_size": str(get_int(values, "atk_1bit_last_read_size")),
        "pre_card_state": str(
            get_int(values, "atk_1bit_read_pre_card_state")
        ),
        "post_card_state": str(
            get_int(values, "atk_1bit_read_post_card_state")
        ),
        "wait_card_state": str(
            get_int(values, "atk_1bit_read_wait_card_state")
        ),
        "read_ms": str(get_int(values, "atk_1bit_last_read_operation_ms")),
        "wait_ms": str(get_int(values, "atk_1bit_read_wait_operation_ms")),
        "wait_timeout_ms": str(
            get_int(values, "atk_1bit_read_wait_timeout_ms")
        ),
        "buffer_inspected": str(get_int(values, "atk_1bit_buffer_inspected")),
        "buffer_len": str(get_int(values, "atk_1bit_buffer_len")),
        "sum512": str(get_int(values, "atk_1bit_buffer_sum512")),
        "xor512": str(get_int(values, "atk_1bit_buffer_xor512")),
        "nonzero_count512": str(
            get_int(values, "atk_1bit_buffer_nonzero_count512")
        ),
        "zero_count512": str(
            get_int(values, "atk_1bit_buffer_zero_count512")
        ),
        "ff_count512": str(get_int(values, "atk_1bit_buffer_ff_count512")),
        "prefill_count512": str(
            get_int(values, "atk_1bit_buffer_prefill_count512")
        ),
        "changed_count512": str(
            get_int(values, "atk_1bit_buffer_changed_count512")
        ),
        "buffer_changed": str(get_int(values, "atk_1bit_buffer_changed")),
        "all_prefill": str(get_int(values, "atk_1bit_buffer_all_prefill")),
        "all_zero": str(get_int(values, "atk_1bit_buffer_all_zero")),
        "all_ff": str(get_int(values, "atk_1bit_buffer_all_ff")),
        "first_changed_index": str(
            get_int(values, "atk_1bit_buffer_first_changed_index")
        ),
        "last_changed_index": str(
            get_int(values, "atk_1bit_buffer_last_changed_index")
        ),
        "first16": values.get("atk_1bit_buffer_first16", "NA"),
        "first32": values.get("atk_1bit_buffer_first32", "NA"),
        "tail16": values.get("atk_1bit_buffer_tail16", "NA"),
    }


def safe_cli(session: UartSession, command: str, log: List[str]) -> str:
    try:
        return session.cli(command)
    except Exception as exc:  # Continue best-effort recovery after serial errors.
        log.append("%s 异常：%r" % (command, exc))
        return ""


def run_cleanup(
    session: UartSession,
    summary: Dict[str, str],
    log: List[str],
) -> None:
    dump_text = safe_cli(session, "DUMP", log)
    dump_guard_markers = (
        "DUMP blocked: snapshot software guard active",
        "DUMP blocked: SD_ONLY_BOOT_NO_CAMERA",
        "SD_ONLY_BOOT_NO_CAMERA",
    )
    summary["DUMP_GUARD"] = (
        "PASS" if any(marker in dump_text for marker in dump_guard_markers) else "FAIL"
    )

    exit_text = safe_cli(session, "SD TAKEOVER EXIT", log)
    summary["TAKEOVER_EXIT"] = (
        "PASS"
        if "full SDIO GPIO restored, conflict pins restored to DCMI AF13"
        in exit_text
        else "FAIL"
    )

    restore_text = safe_cli(session, "SNAPSHOT RESTORE", log)
    summary["SNAPSHOT_RESTORE"] = (
        "PASS"
        if "SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet."
        in restore_text
        else "FAIL"
    )

    status_text = safe_cli(session, "STATUS", log)
    status = parse_kv(status_text)
    summary["SYSTEM_STATUS"] = "PASS" if status_text.strip() else "FAIL"
    summary["IWDG_SKIP"] = str(get_int(status, "iwdg_refresh_skip_count"))
    summary["HOOK_FAULT"] = str(get_int(status, "hook_fault_code"))
    summary["UART_DMA_ERROR"] = str(get_int(status, "uart_dma_error_count"))
    summary["STREAM_OVERFLOW"] = str(
        get_int(status, "stream_buffer_overflow_bytes")
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Stage 11C-5O-3 SD-only full-GPIO ATK1B block-read statistics"
    )
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--read-count", type=int, default=20)
    parser.add_argument("--blocks", type=parse_blocks, default=parse_blocks("0,2048"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--tag", type=sanitize_tag, default="atk1b_repeat")
    parser.add_argument("--captures", default="captures")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.read_count <= 0:
        raise SystemExit("--read-count 必须大于 0")
    if args.timeout <= 0.0:
        raise SystemExit("--timeout 必须大于 0")

    output_dir = Path(args.captures)
    output_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    stem = "sd_%s_%s" % (args.tag, timestamp)
    log_path = output_dir / (stem + "_log.txt")
    csv_path = output_dir / (stem + ".csv")
    summary_path = output_dir / (stem + "_summary.txt")

    log: List[str] = []
    rows: List[Dict[str, str]] = []
    summary = make_summary()
    for block in args.blocks:
        if "BLOCK_%d_RESULTS" % block not in summary:
            add_empty_block_summary(summary, block)

    session: Optional[UartSession] = None
    exit_code = 0
    try:
        session = UartSession(args.port, args.baud, args.timeout, log)

        boot_status_text = session.cli("STATUS")
        boot_status = parse_kv(boot_status_text)
        summary["SD_ONLY_BOOT"] = str(get_int(boot_status, "sd_only_boot"))
        summary["SD_ONLY_BOOT_SUPPORTED"] = str(
            get_int(boot_status, "sd_only_boot_supported")
        )

        precheck_text = session.cli("SD INIT")
        summary["PRE_TAKEOVER_SD_INIT_BLOCKED"] = (
            "YES" if "need SDIO takeover" in precheck_text else "NO"
        )

        prepare_text = session.cli("SNAPSHOT PREPARE")
        prepare_markers = (
            "SNAPSHOT PREPARE: DCMI stop OK",
            "SNAPSHOT PREPARE: SD-only boot, virtual camera pause OK",
            "virtual camera pause OK",
        )
        summary["SNAPSHOT_PREPARE"] = (
            "PASS"
            if any(marker in prepare_text for marker in prepare_markers)
            else "FAIL"
        )

        enter_text = session.cli("SD TAKEOVER ENTER")
        enter_values = parse_kv(enter_text)
        enter_success = (
            "SD TAKEOVER ENTER: full SDIO GPIO switched to AF12" in enter_text
        )
        snapshot_pause_confirmed = get_int(
            enter_values, "snapshot_pause_confirmed", 0
        )
        full_gpio_af12_selected = get_int(
            enter_values, "sdio_full_gpio_af12_selected", 0
        )
        summary["TAKEOVER_ENTER_ERROR_CODE"] = str(
            get_int(enter_values, "last_takeover_error_code")
        )
        summary["TAKEOVER_ENTER_ERROR_TEXT"] = enter_values.get(
            "last_takeover_error_text", "UNKNOWN"
        )
        summary["TAKEOVER_PRECHECK_SUCCESS"] = str(
            get_int(enter_values, "takeover_precheck_success_count")
        )
        summary["SNAPSHOT_PAUSE_CONFIRMED"] = str(snapshot_pause_confirmed)
        summary["CONFLICT_PIN_RELEASE_READY"] = str(
            get_int(enter_values, "conflict_pin_release_ready")
        )
        summary["SDIO_FULL_GPIO_AF12_SELECTED"] = str(full_gpio_af12_selected)
        summary["SDIO_AF12_SELECTED"] = str(
            get_int(enter_values, "sdio_af12_selected")
        )
        for summary_key, status_key in FULL_GPIO_READBACK_FIELDS:
            summary[summary_key] = str(get_int(enter_values, status_key))
        if enter_success:
            summary["TAKEOVER_ENTER"] = "PASS"
        elif full_gpio_af12_selected == 1 and snapshot_pause_confirmed == 1:
            summary["TAKEOVER_ENTER"] = "PARTIAL"
        else:
            summary["TAKEOVER_ENTER"] = "FAIL"

        if enter_success:
            session.cli("SD ATK1BSTATUS")
            init_text = session.cli("SD ATK1BINIT")
            init_status_text = session.cli("SD ATK1BSTATUS")
            init_values = parse_kv(init_text + "\n" + init_status_text)
            init_ready = get_int(init_values, "atk_1bit_init_ready", 0)
            init_ok = (
                "SD ATK1BINIT: HAL_SD_Init OK" in init_text or init_ready == 1
            )
            summary["ATK1B_INIT"] = "PASS" if init_ok else "FAIL"
            summary["ATK1B_INIT_READY"] = str(init_ready)
            summary["ATK1B_CLOCK_DIV"] = str(
                get_int(init_values, "atk_1bit_clock_div")
            )
            summary["ATK1B_BUS_WIDTH"] = str(
                get_int(init_values, "atk_1bit_bus_width")
            )
            summary["ATK1B_LAST_CARD_STATE"] = str(
                get_int(init_values, "atk_1bit_last_card_state")
            )

            if init_ok:
                summary["READ_REPEAT"] = "RUN"
                for block in args.blocks:
                    for index in range(1, args.read_count + 1):
                        rows.append(read_once(session, block, index))
            else:
                summary["READ_REPEAT"] = "SKIP"
                exit_code = 1
        else:
            summary["ATK1B_INIT"] = "FAIL"
            summary["READ_REPEAT"] = "SKIP"
            exit_code = 1
    except Exception as exc:
        log.append("\n脚本异常：%r" % (exc,))
        summary["SCRIPT_EXCEPTION"] = repr(exc)
        if summary["ATK1B_INIT"] == "UNKNOWN":
            summary["ATK1B_INIT"] = "FAIL"
        summary["READ_REPEAT"] = "PARTIAL" if rows else "SKIP"
        exit_code = 2
    finally:
        for block in ordered_unique(["0", "2048"] + [str(v) for v in args.blocks]):
            update_block_summary(summary, int(block), rows)

        if session is not None:
            run_cleanup(session, summary, log)
            try:
                session.close()
            except Exception as exc:
                log.append("关闭串口异常：%r" % (exc,))

        with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()
            writer.writerows(rows)

        summary["CSV_FILE"] = str(csv_path)
        summary["LOG_FILE"] = str(log_path)
        summary["SUMMARY_FILE"] = str(summary_path)
        log_path.write_text("\n".join(log) + "\n", encoding="utf-8")
        summary_text = "\n".join(
            "%s=%s" % (key, value) for key, value in summary.items()
        )
        summary_path.write_text(summary_text + "\n", encoding="utf-8")
        print(summary_text)

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
