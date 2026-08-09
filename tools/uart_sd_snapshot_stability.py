#!/usr/bin/env python3
import argparse
import csv
import re
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    serial = None


DEFAULT_PORT = "COM4"
DEFAULT_BAUD = 115200
DEFAULT_COUNT = 50
DEFAULT_INTERVAL_SECONDS = 0.5
DEFAULT_TIMEOUT_SECONDS = 15.0
DEFAULT_OUTPUT_DIRECTORY = Path("captures")
SERIAL_IDLE_SECONDS = 0.25

SNAPSHOT_HEADER = "SD SNAPSHOT:"
SD_STATUS_HEADER = "SD STATUS:"
STATUS_HEADER = "STATUS:"
EXPECTED_BMP_BYTES = 57654
FILE_NAME_PATTERN = re.compile(r"^IMG([0-9]{4})\.BMP$")

SNAPSHOT_FIELDS = (
    "result",
    "file",
    "bytes",
    "source",
    "source_bytes",
    "source_nonzero",
    "source_sum32",
    "prepare",
    "prepare_retry",
    "format",
    "width",
    "height",
    "mount",
    "write",
    "cleanup",
    "restore",
    "total_ms",
    "prepare_ms",
    "write_ms",
    "cleanup_ms",
    "error",
)

NUMERIC_FIELDS = {
    "bytes",
    "source_bytes",
    "source_nonzero",
    "source_sum32",
    "prepare_retry",
    "width",
    "height",
    "total_ms",
    "prepare_ms",
    "write_ms",
    "cleanup_ms",
}

CSV_FIELDS = (
    "index",
    "timestamp",
    "result",
    "file",
    "file_index",
    "bytes",
    "source",
    "source_bytes",
    "source_nonzero",
    "source_sum32",
    "prepare",
    "prepare_retry",
    "format",
    "width",
    "height",
    "mount",
    "write",
    "cleanup",
    "restore",
    "total_ms",
    "prepare_ms",
    "write_ms",
    "cleanup_ms",
    "error",
    "pass_check",
    "sequence_ok",
)


class RunLogger:
    """同时输出控制台信息并保存完整命令响应。"""

    def __init__(self, path):
        self.path = path
        self._file = path.open("w", encoding="utf-8")

    def close(self):
        self._file.close()

    def emit(self, message=""):
        print(message)
        self._file.write(message + "\n")
        self._file.flush()

    def transcript(self, command, lines):
        self._file.write(f"\n>>> {command}\\r\\n\n")
        if lines:
            for line in lines:
                self._file.write(line + "\n")
        else:
            self._file.write("<no response>\n")
        self._file.flush()


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="连续执行SD SNAPSHOT并统计BMP保存稳定性。"
    )
    parser.add_argument("--port", default=DEFAULT_PORT, help="串口，默认COM4。")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="波特率。")
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT, help="测试次数。")
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_INTERVAL_SECONDS,
        help="两轮SD SNAPSHOT之间的间隔秒数。",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="每条命令响应的总超时秒数。",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_DIRECTORY,
        help="CSV和日志输出目录，默认captures。",
    )
    return parser.parse_args()


def validate_arguments(args):
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


def build_output_paths(output_directory):
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    prefix = f"sd_snapshot_stability_{timestamp}"
    return (
        output_directory / f"{prefix}.csv",
        output_directory / f"{prefix}.log",
    )


def open_serial_port(port_name, baudrate):
    """在open之前固定DTR/RTS安全状态，避免开发板复位或进入ISP。"""
    port = serial.Serial()
    port.port = port_name
    port.baudrate = baudrate
    port.timeout = 0.1
    port.write_timeout = 2.0
    port.rtscts = False
    port.dsrdtr = False
    port.dtr = False
    port.rts = False

    try:
        port.open()
        time.sleep(0.2)
        port.reset_input_buffer()
        port.reset_output_buffer()
    except Exception:
        if port.is_open:
            port.close()
        raise
    return port


def run_text_command(port, command, timeout_seconds, expected_marker, logger):
    """发送CRLF命令并读取到目标行后的空闲间隔或总超时。"""
    port.reset_input_buffer()
    port.write((command + "\r\n").encode("ascii"))
    port.flush()

    lines = []
    marker_found = False
    last_receive_time = None
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        port.timeout = min(0.1, max(remaining, 0.0))
        raw_line = port.readline()
        now = time.monotonic()
        if raw_line:
            line = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
            lines.append(line)
            last_receive_time = now
            if line.strip() == expected_marker:
                marker_found = True
            continue

        if (
            marker_found
            and last_receive_time is not None
            and (now - last_receive_time) >= SERIAL_IDLE_SECONDS
        ):
            break

    logger.transcript(command, lines)
    return lines, marker_found


def parse_key_value_block(lines, header):
    """解析指定标题后连续缩进的key=value字段。"""
    fields = {}
    block_found = False
    for line in lines:
        if not block_found:
            if line.strip() == header:
                block_found = True
            continue

        if not line:
            continue
        if not line[0].isspace():
            break
        stripped = line.strip()
        if "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields, block_found


def parse_all_indented_fields(lines):
    """STATUS包含多个子标题，因此收集响应中的全部缩进字段。"""
    fields = {}
    for line in lines:
        if not line or not line[0].isspace():
            continue
        stripped = line.strip()
        if "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields


def parse_integer(value):
    if value is None or value == "":
        return None
    try:
        return int(str(value), 0)
    except ValueError:
        return None


def parse_snapshot_fields(lines):
    parsed, block_found = parse_key_value_block(lines, SNAPSHOT_HEADER)
    result = {field: parsed.get(field, "") for field in SNAPSHOT_FIELDS}
    for field in NUMERIC_FIELDS:
        result[field] = parse_integer(result[field])
    return result, block_found


def extract_file_index(file_name):
    match = FILE_NAME_PATTERN.fullmatch(file_name or "")
    if match is None:
        return None
    file_index = int(match.group(1))
    return file_index if 1 <= file_index <= 9999 else None


def validate_snapshot(fields, block_found, communication_error):
    reasons = []
    if communication_error:
        reasons.append(communication_error)
    if not block_found:
        reasons.append("SNAPSHOT_BLOCK_NOT_FOUND")
    if fields["result"] != "PASS":
        reasons.append("RESULT")
    if extract_file_index(fields["file"]) is None:
        reasons.append("FILE")
    if fields["bytes"] != EXPECTED_BMP_BYTES:
        reasons.append("BYTES")
    if fields["source_nonzero"] is None or fields["source_nonzero"] <= 0:
        reasons.append("SOURCE_NONZERO")
    if fields["source_sum32"] is None or fields["source_sum32"] <= 0:
        reasons.append("SOURCE_SUM32")
    for field in ("prepare", "mount", "write", "cleanup", "restore"):
        if fields[field] != "PASS":
            reasons.append(field.upper())
    if fields["error"] not in ("", "OK"):
        reasons.append("ERROR")
    return reasons


def status_health_ok(fields):
    names = ("hook_fault", "uart_dma_error", "stream_overflow", "refresh_skip")
    return all(parse_integer(fields.get(name)) == 0 for name in names)


def sd_status_health_ok(fields):
    return (
        fields.get("last_snapshot") == "PASS"
        and fields.get("save_error") == "OK"
        and fields.get("last_error") == "OK"
    )


def metric_summary(values):
    if not values:
        return "N/A", "N/A", "N/A"
    return min(values), max(values), f"{sum(values) / len(values):.2f}"


def main():
    args = parse_arguments()
    if serial is None:
        print("[ERROR] 缺少pyserial，请先安装后再运行本工具。")
        return 1
    try:
        validate_arguments(args)
        args.output.mkdir(parents=True, exist_ok=True)
    except (ValueError, OSError) as error:
        print(f"[ERROR] 参数或输出目录错误：{error}")
        return 1

    csv_path, log_path = build_output_paths(args.output)
    try:
        logger = RunLogger(log_path)
    except OSError as error:
        print(f"[ERROR] 无法创建日志文件：{error}")
        return 1
    port = None
    rows = []
    preflight_ok = False
    final_status_ok = False
    final_sd_status_ok = False
    final_sd_fields = {}
    sequence_all_ok = True
    previous_file_index = None
    fatal_error = ""

    logger.emit(
        f"[INFO] port={args.port} baud={args.baud} count={args.count} "
        f"interval={args.interval} timeout={args.timeout}"
    )
    logger.emit(f"[INFO] csv={csv_path}")
    logger.emit(f"[INFO] log={log_path}")

    try:
        with csv_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()

            try:
                port = open_serial_port(args.port, args.baud)
                logger.emit(f"[INFO] serial_opened DTR={port.dtr} RTS={port.rts}")

                logger.emit("[INFO] checking HELP...")
                help_lines, help_ok = run_text_command(
                    port, "HELP", args.timeout, "SD SNAPSHOT", logger
                )
                help_ok = help_ok and any(
                    line.strip() == "SD SNAPSHOT" for line in help_lines
                )

                logger.emit("[INFO] checking initial STATUS...")
                initial_status_lines, initial_status_found = run_text_command(
                    port, "STATUS", args.timeout, STATUS_HEADER, logger
                )

                logger.emit("[INFO] checking initial SD STATUS...")
                initial_sd_lines, initial_sd_found = run_text_command(
                    port, "SD STATUS", args.timeout, SD_STATUS_HEADER, logger
                )
                preflight_ok = help_ok and initial_status_found and initial_sd_found
                if not preflight_ok:
                    fatal_error = "PREFLIGHT_FAILED"
                    logger.emit(
                        "[ERROR] HELP/STATUS/SD STATUS预检查失败，未开始snapshot循环。"
                    )
                else:
                    digits = max(3, len(str(args.count)))
                    for request_index in range(1, args.count + 1):
                        communication_error = ""
                        try:
                            response_lines, block_marker_found = run_text_command(
                                port,
                                "SD SNAPSHOT",
                                args.timeout,
                                SNAPSHOT_HEADER,
                                logger,
                            )
                        except (serial.SerialException, OSError) as error:
                            response_lines = []
                            block_marker_found = False
                            communication_error = "SERIAL_ERROR"
                            fatal_error = f"SERIAL_ERROR: {error}"

                        fields, parsed_block_found = parse_snapshot_fields(
                            response_lines
                        )
                        block_found = block_marker_found and parsed_block_found
                        if not block_found and not communication_error:
                            communication_error = "TIMEOUT_OR_NO_BLOCK"

                        reasons = validate_snapshot(
                            fields, block_found, communication_error
                        )
                        file_index = extract_file_index(fields["file"])
                        if file_index is None:
                            sequence_ok = False
                        elif previous_file_index is None:
                            sequence_ok = True
                        else:
                            sequence_ok = file_index == previous_file_index + 1
                        if file_index is not None:
                            previous_file_index = file_index
                        if not sequence_ok:
                            reasons.append("FILE_SEQUENCE")
                            sequence_all_ok = False

                        passed = not reasons
                        error_value = fields["error"]
                        if not error_value and communication_error:
                            error_value = communication_error
                        row = {
                            "index": request_index,
                            "timestamp": datetime.now().astimezone().isoformat(
                                timespec="seconds"
                            ),
                            "file_index": "" if file_index is None else file_index,
                            "error": error_value,
                            "pass_check": "PASS" if passed else "FAIL",
                            "sequence_ok": str(sequence_ok),
                        }
                        for field in SNAPSHOT_FIELDS:
                            if field != "error":
                                value = fields[field]
                                row[field] = "" if value is None else value
                        writer.writerow(row)
                        csv_file.flush()
                        rows.append(row)

                        result_text = "PASS" if passed else "FAIL"
                        logger.emit(
                            f"[{request_index:0{digits}d}/{args.count:0{digits}d}] "
                            f"{result_text} file={fields['file'] or 'NONE'} "
                            f"bytes={fields['bytes'] if fields['bytes'] is not None else ''} "
                            f"total_ms={fields['total_ms'] if fields['total_ms'] is not None else ''} "
                            f"write_ms={fields['write_ms'] if fields['write_ms'] is not None else ''}"
                        )
                        if reasons:
                            logger.emit(f"[WARN] checks={','.join(reasons)}")

                        if fatal_error:
                            break
                        if request_index < args.count:
                            time.sleep(args.interval)

                if port is not None and port.is_open and not fatal_error:
                    logger.emit("[INFO] checking final STATUS...")
                    final_status_lines, final_status_found = run_text_command(
                        port, "STATUS", args.timeout, STATUS_HEADER, logger
                    )
                    final_status_fields = parse_all_indented_fields(
                        final_status_lines
                    )
                    final_status_ok = final_status_found and status_health_ok(
                        final_status_fields
                    )

                    logger.emit("[INFO] checking final SD STATUS...")
                    final_sd_lines, final_sd_found = run_text_command(
                        port, "SD STATUS", args.timeout, SD_STATUS_HEADER, logger
                    )
                    final_sd_fields, parsed_final_sd = parse_key_value_block(
                        final_sd_lines, SD_STATUS_HEADER
                    )
                    final_sd_status_ok = (
                        final_sd_found
                        and parsed_final_sd
                        and sd_status_health_ok(final_sd_fields)
                    )

            except (serial.SerialException, OSError) as error:
                fatal_error = f"SERIAL_ERROR: {error}"
                logger.emit(f"[ERROR] 无法打开或使用串口：{error}")
                logger.emit(
                    f"[ERROR] 请确认{args.port}存在且未被其他串口工具占用。"
                )
            finally:
                if port is not None and port.is_open:
                    port.close()

    except OSError as error:
        fatal_error = f"OUTPUT_ERROR: {error}"
        logger.emit(f"[ERROR] 写入CSV失败：{error}")

    total_ms_values = [
        int(row["total_ms"])
        for row in rows
        if isinstance(row.get("total_ms"), int)
    ]
    write_ms_values = [
        int(row["write_ms"])
        for row in rows
        if isinstance(row.get("write_ms"), int)
    ]
    total_min, total_max, total_avg = metric_summary(total_ms_values)
    write_min, write_max, write_avg = metric_summary(write_ms_values)
    pass_count = sum(row["pass_check"] == "PASS" for row in rows)
    fail_count = args.count - pass_count
    pass_rate = pass_count * 100.0 / args.count
    valid_files = [row["file"] for row in rows if row.get("file")]
    sequence_complete_ok = sequence_all_ok and len(rows) == args.count
    error_found = (
        any(row.get("error") not in ("", "OK") for row in rows)
        or final_sd_fields.get("save_error", "OK") != "OK"
        or final_sd_fields.get("last_error", "OK") != "OK"
    )
    final_result = (
        preflight_ok
        and len(rows) == args.count
        and pass_count == args.count
        and sequence_complete_ok
        and final_status_ok
        and final_sd_status_ok
        and not fatal_error
    )

    logger.emit("")
    logger.emit("SUMMARY:")
    logger.emit(f"  total={args.count}")
    logger.emit(f"  completed={len(rows)}")
    logger.emit(f"  pass={pass_count}")
    logger.emit(f"  fail={fail_count}")
    logger.emit(f"  pass_rate={pass_rate:.2f}%")
    logger.emit(f"  first_file={valid_files[0] if valid_files else 'NONE'}")
    logger.emit(f"  last_file={valid_files[-1] if valid_files else 'NONE'}")
    logger.emit(f"  sequence_ok={sequence_complete_ok}")
    logger.emit(f"  error_found={error_found}")
    logger.emit(f"  final_status_ok={final_status_ok}")
    logger.emit(f"  final_sd_status_ok={final_sd_status_ok}")
    logger.emit(f"  total_ms_min={total_min}")
    logger.emit(f"  total_ms_max={total_max}")
    logger.emit(f"  total_ms_avg={total_avg}")
    logger.emit(f"  write_ms_min={write_min}")
    logger.emit(f"  write_ms_max={write_max}")
    logger.emit(f"  write_ms_avg={write_avg}")
    logger.emit(f"  csv={csv_path}")
    logger.emit(f"  log={log_path}")
    if fatal_error:
        logger.emit(f"  fatal_error={fatal_error}")
    logger.emit(f"  result={'PASS' if final_result else 'FAIL'}")
    logger.close()
    return 0 if final_result else 1


if __name__ == "__main__":
    sys.exit(main())
