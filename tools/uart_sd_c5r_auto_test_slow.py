#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 11 SD Snapshot C5R 慢速自动板测脚本

用途：
1. 代替手工串口输入 C5R 基线恢复测试命令。
2. 在 SNAPSHOT PREPARE 和 SD TAKEOVER ENTER 后增加等待时间，尽量模拟人工输入节奏。
3. 自动判断 SD INIT / CardInfo / READTEST / EXIT / RESTORE / 图像链路结果。

默认：
- COM4
- 115200
- DTR=False、RTS=False，且 rtscts/dsrdtr=False
- 默认 repeat=0，只做一次 RESTORE 后图像请求，省时间

常用运行：
python tools/uart_sd_c5r_auto_test_slow.py --port COM4 --baud 115200

如果需要 20 次图像稳定性：
python tools/uart_sd_c5r_auto_test_slow.py --port COM4 --baud 115200 --repeat 20
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
except ImportError:
    print("错误：未安装 pyserial，请先执行：pip install pyserial")
    sys.exit(2)

FRAME_MAGIC = b"OV56RGB5"
FRAME_HEADER_LEN = 22
EXPECTED_WIDTH = 160
EXPECTED_HEIGHT = 120
EXPECTED_PAYLOAD_LEN = EXPECTED_WIDTH * EXPECTED_HEIGHT * 2
REQ_MAGIC = b"\xA5\x5A"
REQ_VERSION = 0x01
REQ_TYPE_IMAGE = 0x20
REQ_EOF = b"\x0D\x0A"


@dataclass
class FrameResult:
    """保存一帧 OV56RGB5 响应的解析结果。"""
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
    """同时写入终端和文件的测试日志器。"""
    def __init__(self, log_path: str):
        """初始化测试会话及其运行状态。"""
        self.log_path = log_path
        self._fp = open(log_path, "w", encoding="utf-8", newline="\n")

    def close(self) -> None:
        """关闭串口或日志等会话资源。"""
        self._fp.close()

    def write(self, text: str = "") -> None:
        """写入日志并立即刷新，保留故障现场。"""
        print(text)
        self._fp.write(text + "\n")
        self._fp.flush()

    def section(self, title: str) -> None:
        """在日志中输出测试分节标题。"""
        line = "=" * 72
        self.write("\n" + line)
        self.write(title)
        self.write(line)

    def command(self, cmd: str, response: str) -> None:
        """记录并执行一条测试命令。"""
        self.section(f"CLI > {cmd}")
        self.write(response.rstrip() if response else "<无输出>")


_FIELD_RE_CACHE: Dict[str, re.Pattern[str]] = {}


def get_field(text: str, key: str) -> Optional[str]:
    """读取 CLI 响应中的指定字段。"""
    pat = _FIELD_RE_CACHE.get(key)
    if pat is None:
        pat = re.compile(rf"^\s*{re.escape(key)}\s*=\s*(.*?)\s*$", re.MULTILINE)
        _FIELD_RE_CACHE[key] = pat
    matches = pat.findall(text)
    return matches[-1].strip() if matches else None


def get_int_field(text: str, key: str) -> Optional[int]:
    """读取并转换 CLI 响应中的整数字段。"""
    val = get_field(text, key)
    if val is None:
        return None
    try:
        if val.lower().startswith("0x"):
            return int(val, 16)
        return int(val, 10)
    except ValueError:
        return None


def crc32_u32(data: bytes) -> int:
    """计算 CRC32，使请求或图像负载的传输损坏可被协议检测。"""
    return binascii.crc32(data) & 0xFFFFFFFF


def make_image_request(seq: int) -> bytes:
    """构造固定格式的二进制图像请求帧。"""
    body = bytes([REQ_VERSION, REQ_TYPE_IMAGE]) + struct.pack("<HH", seq & 0xFFFF, 0)
    crc = crc32_u32(body)
    return REQ_MAGIC + body + struct.pack("<I", crc) + REQ_EOF


def open_serial(port: str, baud: int) -> "serial.Serial":
    """以不会触发开发板自动复位的控制线状态打开串口。"""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.write_timeout = 2.0
    ser.rtscts = False
    ser.dsrdtr = False
    # 必须在 open 前设置，避免串口打开瞬间拉动 DTR/RTS 造成板子复位。
    ser.dtr = False
    ser.rts = False
    ser.open()
    # 某些驱动会在 open() 时重写控制线，打开后再次锁定安全状态。
    ser.dtr = False
    ser.rts = False
    return ser


def drain_serial(ser: "serial.Serial", duration_s: float = 0.25) -> bytes:
    """清空串口中本轮测试前的残留输入。"""
    deadline = time.monotonic() + max(duration_s, 0.0)
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
        total_timeout_s: float,
        idle_timeout_s: float,
        reset_input: bool = True,
) -> str:
    """发送 CLI 命令并等待响应收敛。"""
    if reset_input:
        drain_serial(ser, 0.08)
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
    """在总截止时间内读取指定字节数。"""
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
    """接收 OV56RGB5 图像帧并校验长度与 CRC32。"""
    start = time.monotonic()
    deadline = start + timeout_s
    buf = bytearray()

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

    remain_header = FRAME_HEADER_LEN - len(buf)
    if remain_header > 0:
        buf.extend(read_exact_with_timeout(ser, remain_header, max(0.1, deadline - time.monotonic())))

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
    if width != EXPECTED_WIDTH or height != EXPECTED_HEIGHT or payload_len != EXPECTED_PAYLOAD_LEN:
        return FrameResult(False, f"图像参数异常：{width}x{height}, payload={payload_len}", total_len=len(buf)), bytes(buf)

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
    return FrameResult(ok, err, len(buf), frame_id, width, height, payload_len, rx_crc, calc_crc, elapsed_ms), bytes(buf)


def send_image_request(ser: "serial.Serial", seq: int, timeout_s: float) -> FrameResult:
    """发送二进制图像请求并返回校验结果。"""
    drain_serial(ser, 0.08)
    ser.reset_input_buffer()
    ser.write(make_image_request(seq))
    ser.flush()
    result, _raw = receive_ov56rgb5_frame(ser, timeout_s)
    return result


def main() -> int:
    """解析参数并执行完整测试流程。"""
    parser = argparse.ArgumentParser(description="Stage 11 C5R 慢速自动板测脚本")
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--startup-drain", type=float, default=1.2)
    parser.add_argument("--cli-timeout", type=float, default=10.0)
    parser.add_argument("--cli-idle", type=float, default=0.75)
    parser.add_argument("--command-gap", type=float, default=0.6, help="每条 CLI 命令后的额外等待")
    parser.add_argument("--after-prepare-delay", type=float, default=1.5, help="SNAPSHOT PREPARE 后等待")
    parser.add_argument("--after-enter-delay", type=float, default=2.0, help="SD TAKEOVER ENTER 后等待再 SD INIT")
    parser.add_argument("--after-exit-delay", type=float, default=1.0, help="SD TAKEOVER EXIT 后等待")
    parser.add_argument("--after-restore-delay", type=float, default=1.0, help="SNAPSHOT RESTORE 后等待")
    parser.add_argument("--frame-timeout", type=float, default=10.0)
    parser.add_argument("--repeat", type=int, default=0, help="默认 0；需要稳定性时设为 20")
    parser.add_argument("--skip-image", action="store_true")
    parser.add_argument("--skip-readtest", action="store_true")
    parser.add_argument("--output-dir", default="captures")
    parser.add_argument("--tag", default="sd_c5r_slow_auto")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(args.output_dir, f"{args.tag}_{ts}_log.txt")
    summary_path = os.path.join(args.output_dir, f"{args.tag}_{ts}_summary.txt")
    logger = TestLogger(log_path)
    summary_lines: List[str] = []

    def add_summary(line: str) -> None:
        """把当前测试结果追加到汇总集合。"""
        summary_lines.append(line)
        logger.write(line)

    ser: Optional["serial.Serial"] = None
    exit_code = 0

    try:
        logger.section("Stage 11 C5R 慢速自动测试开始")
        logger.write(f"串口：{args.port}")
        logger.write(f"波特率：{args.baud}")
        logger.write("DTR状态：False")
        logger.write("RTS状态：False")
        logger.write(f"日志文件：{log_path}")

        ser = open_serial(args.port, args.baud)
        startup = drain_serial(ser, args.startup_drain).decode("utf-8", errors="replace")
        if startup.strip():
            logger.section("启动/残留输出")
            logger.write(startup.rstrip())

        def run(cmd: str, timeout: Optional[float] = None) -> str:
            """执行自动化测试主体并收集结果。"""
            text = send_cli_command(
                ser,  # type: ignore[arg-type]
                cmd,
                total_timeout_s=timeout if timeout is not None else args.cli_timeout,
                idle_timeout_s=args.cli_idle,
            )
            logger.command(cmd, text)
            time.sleep(max(args.command_gap, 0.0))
            return text

        logger.section("一、SD 初始化与 takeover")
        pre_init = run("SD INIT")
        prepare = run("SNAPSHOT PREPARE")
        logger.write(f"等待 SNAPSHOT PREPARE 后稳定：{args.after_prepare_delay:.2f}s")
        time.sleep(max(args.after_prepare_delay, 0.0))
        snapshot_after_prepare = run("SNAPSHOT STATUS")

        enter = run("SD TAKEOVER ENTER")
        logger.write(f"等待 SDIO GPIO 切换后稳定：{args.after_enter_delay:.2f}s")
        time.sleep(max(args.after_enter_delay, 0.0))
        takeover_after_enter = run("SD TAKEOVER STATUS")

        sd_init = run("SD INIT", timeout=max(args.cli_timeout, 12.0))
        sd_status_after_init = run("SD STATUS")
        cardinfo_after_init = run("SD CARDINFO")
        init_text = sd_init + "\n" + sd_status_after_init + "\n" + cardinfo_after_init

        init_ok = "SD INIT: HAL_SD_Init OK" in sd_init and get_int_field(init_text, "card_info_read_success_count") == 1
        init_failed = "SD INIT: HAL_SD_Init failed" in sd_init
        add_summary(f"SD_INIT={'PASS' if init_ok else 'FAIL'}")
        add_summary(f"SD_INIT_FAIL_REASON={'NONE' if init_ok else 'HAL_SD_Init failed' if init_failed else 'unknown'}")
        add_summary(f"IS_INITIALIZED={get_int_field(init_text, 'is_initialized')}")
        add_summary(f"SDIO_READY={get_int_field(init_text, 'sdio_ready')}")
        add_summary(f"HAL_INIT_SUCCESS={get_int_field(init_text, 'sdio_hal_init_success_count')}")
        add_summary(f"HAL_INIT_ERROR={get_int_field(init_text, 'sdio_hal_init_error_count')}")
        add_summary(f"LAST_HAL_INIT_STATUS={get_int_field(init_text, 'last_hal_sd_init_status')}")
        add_summary(f"LAST_HAL_ERROR={get_int_field(init_text, 'last_hal_sd_error')}")
        add_summary(f"CARDINFO_SUCCESS={get_int_field(init_text, 'card_info_read_success_count')}")
        add_summary(f"CARD_BLOCK_SIZE={get_int_field(init_text, 'card_block_size')}")
        add_summary(f"CARD_LOG_BLOCK_SIZE={get_int_field(init_text, 'card_log_block_size')}")
        if not init_ok:
            exit_code = 1

        if init_ok and not args.skip_readtest:
            logger.section("二、SD READTEST")
            read0 = run("SD READTEST 0")
            readinfo0 = run("SD READINFO")
            read2048 = run("SD READTEST 2048")
            readinfo2048 = run("SD READINFO")
            read0_text = read0 + "\n" + readinfo0
            read2048_text = read2048 + "\n" + readinfo2048
            read0_ok = "SD READTEST: block read OK" in read0
            read2048_ok = "SD READTEST: block read OK" in read2048
            read0_crc = get_int_field(read0_text, "last_block_read_error_is_data_crc_fail") == 1
            read2048_crc = get_int_field(read2048_text, "last_block_read_error_is_data_crc_fail") == 1
            add_summary(f"READTEST_0={'PASS' if read0_ok else 'DATA_CRC_FAIL' if read0_crc else 'FAIL'}")
            add_summary(f"READTEST_0_LAST_ERROR={get_int_field(read0_text, 'last_block_read_error')}")
            add_summary(f"READTEST_2048={'PASS' if read2048_ok else 'DATA_CRC_FAIL' if read2048_crc else 'FAIL'}")
            add_summary(f"READTEST_2048_LAST_ERROR={get_int_field(read2048_text, 'last_block_read_error')}")
        else:
            logger.section("二、跳过 SD READTEST")
            if not init_ok:
                logger.write("原因：SD INIT 未成功，READTEST 不会真正读卡。")
            else:
                logger.write("原因：用户设置 --skip-readtest。")
            add_summary("READTEST_0=SKIP")
            add_summary("READTEST_2048=SKIP")

        logger.section("三、退出 takeover 与恢复 snapshot")
        dump_guard = run("DUMP", timeout=4.0)
        exit_text = run("SD TAKEOVER EXIT")
        logger.write(f"等待 SD TAKEOVER EXIT 后稳定：{args.after_exit_delay:.2f}s")
        time.sleep(max(args.after_exit_delay, 0.0))
        sd_status_after_exit = run("SD STATUS")
        takeover_after_exit = run("SD TAKEOVER STATUS")
        readinfo_after_exit = run("SD READINFO")
        cardinfo_after_exit = run("SD CARDINFO")
        restore = run("SNAPSHOT RESTORE")
        logger.write(f"等待 SNAPSHOT RESTORE 后稳定：{args.after_restore_delay:.2f}s")
        time.sleep(max(args.after_restore_delay, 0.0))
        snapshot_after_restore = run("SNAPSHOT STATUS")
        final_status = run("STATUS")

        guard_ok = "DUMP blocked: snapshot software guard active" in dump_guard
        exit_ok = "SD TAKEOVER EXIT: HAL_SD_DeInit status=0" in exit_text and "conflict pins restored to DCMI AF13" in exit_text
        restore_ok = get_int_field(snapshot_after_restore, "software_guard_active") == 0 and get_int_field(snapshot_after_restore, "dump_block_required") == 0
        iwdg_skip = get_int_field(final_status, "iwdg_refresh_skip_count")
        hook_fault = get_int_field(final_status, "hook_fault_code")
        uart_dma_err = get_int_field(final_status, "uart_dma_error_count")
        overflow = get_int_field(final_status, "stream_buffer_overflow_bytes")
        system_ok = iwdg_skip == 0 and hook_fault == 0 and uart_dma_err == 0 and overflow == 0

        add_summary(f"DUMP_GUARD={'PASS' if guard_ok else 'FAIL'}")
        add_summary(f"TAKEOVER_EXIT={'PASS' if exit_ok else 'FAIL'}")
        add_summary(f"SNAPSHOT_RESTORE={'PASS' if restore_ok else 'FAIL'}")
        add_summary(f"SYSTEM_STATUS={'PASS' if system_ok else 'FAIL'}")
        add_summary(f"IWDG_SKIP={iwdg_skip}")
        add_summary(f"HOOK_FAULT={hook_fault}")
        add_summary(f"UART_DMA_ERROR={uart_dma_err}")
        add_summary(f"STREAM_OVERFLOW={overflow}")

        if not args.skip_image:
            logger.section("四、RESTORE 后图像链路")
            first = send_image_request(ser, 0x1234, args.frame_timeout)  # type: ignore[arg-type]
            logger.write(
                f"BASIC: {'PASS' if first.ok else 'FAIL'} frame_id={first.frame_id} "
                f"len={first.total_len} crc_rx=0x{first.rx_crc32:08X} "
                f"crc_calc=0x{first.calc_crc32:08X} time={first.elapsed_ms:.1f}ms {first.error}"
            )
            add_summary(f"BASIC_IMAGE={'PASS' if first.ok else 'FAIL'}")
            if not first.ok:
                exit_code = 1

            if args.repeat > 0:
                logger.section(f"五、重复图像请求 {args.repeat} 次")
                results: List[FrameResult] = []
                for i in range(1, args.repeat + 1):
                    r = send_image_request(ser, i, args.frame_timeout)  # type: ignore[arg-type]
                    results.append(r)
                    logger.write(
                        f"[{i:02d}/{args.repeat:02d}] seq=0x{i:04X} frame_id={r.frame_id} "
                        f"len={r.total_len} time={r.elapsed_ms:.1f}ms {'PASS' if r.ok else 'FAIL'} {r.error}"
                    )
                    time.sleep(0.2)
                success = sum(1 for r in results if r.ok)
                frame_ids = [r.frame_id for r in results if r.ok]
                continuous = bool(frame_ids) and frame_ids == list(range(frame_ids[0], frame_ids[0] + len(frame_ids)))
                avg_ms = sum(r.elapsed_ms for r in results if r.ok) / success if success else 0.0
                add_summary(f"REPEAT_SUCCESS={success}/{args.repeat}")
                add_summary(f"REPEAT_FRAME_ID_CONTINUOUS={'YES' if continuous else 'NO'}")
                add_summary(f"REPEAT_AVG_MS={avg_ms:.2f}")
                if success != args.repeat or not continuous:
                    exit_code = 1
        else:
            add_summary("BASIC_IMAGE=SKIP")

        logger.section("测试结论")
        logger.write("测试结果：PASS" if exit_code == 0 else "测试结果：FAIL / PARTIAL，请看 summary。")

    except KeyboardInterrupt:
        logger.write("\n用户中断。")
        exit_code = 130
    except Exception as exc:
        logger.write(f"脚本异常：{exc!r}")
        exit_code = 1
    finally:
        if ser is not None and ser.is_open:
            # 关闭前保持控制线为低，避免退出脚本时再次触发自动下载电路。
            ser.dtr = False
            ser.rts = False
            ser.close()
        with open(summary_path, "w", encoding="utf-8", newline="\n") as fp:
            fp.write("\n".join(summary_lines) + "\n")
        logger.write(f"\n完整日志：{log_path}")
        logger.write(f"摘要文件：{summary_path}")
        logger.close()

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
