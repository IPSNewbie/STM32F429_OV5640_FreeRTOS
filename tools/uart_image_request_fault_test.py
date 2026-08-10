"""UART 二进制图像请求协议故障注入与恢复验证工具。"""

import struct
import sys
import time
import zlib
import serial
PORT = "COM4"
BAUD = 115200
TIMEOUT_SECONDS = 8.0
SILENCE_WAIT_SECONDS = 0.4
RECOVERY_INTERVAL_SECONDS = 0.1
DISCARD_TIMEOUT_WAIT_SECONDS = 0.3
REQUEST_FRAME_SIZE = 14
IMAGE_HEADER_SIZE = 22
IMAGE_PAYLOAD_SIZE = 38400
IMAGE_CRC_SIZE = 4
IMAGE_FRAME_SIZE = 38426
ERROR_CASE_COUNT = 5

def build_request(seq, version=0x01, msg_type=0x20, payload_len=0,
                  corrupt_crc=False, eof0=0x0D, eof1=0x0A):
    """构造合法请求，或只破坏调用方指定的一个字段。"""
    if not 0 <= seq <= 0xFFFF:
        raise ValueError("请求 seq 必须在 0 到 65535 之间。")
    if not 0 <= version <= 0xFF or not 0 <= msg_type <= 0xFF:
        raise ValueError("version 和 msg_type 必须是单字节整数。")
    if not 0 <= payload_len <= 0xFFFF:
        raise ValueError("payload_len 必须在 0 到 65535 之间。")
    if not 0 <= eof0 <= 0xFF or not 0 <= eof1 <= 0xFF:
        raise ValueError("EOF 必须是单字节整数。")
    body = struct.pack("<BBHH", version, msg_type, seq, payload_len)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    if corrupt_crc:
        crc ^= 0x00000001
    request = b"\xA5\x5A" + body + struct.pack("<I", crc) + bytes((eof0, eof1))
    if len(request) != REQUEST_FRAME_SIZE:
        raise ValueError("请求帧长度错误。")
    return request

def read_exact(ser, expected_size, timeout_seconds):
    """循环接收，直到达到目标长度或总超时。"""
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while len(data) < expected_size:
        if time.monotonic() >= deadline:
            break
        chunk = ser.read(expected_size - len(data))
        if chunk:
            data.extend(chunk)
    return bytes(data)

def check_response(response):
    """检查 OV56RGB5 响应的 header、长度和 payload CRC。"""
    if len(response) != IMAGE_FRAME_SIZE:
        return False, 0, "合法请求接收长度不足 38426 B"
    magic = response[0:8]
    version = response[8]
    pixel_format = response[9]
    width, height = struct.unpack("<HH", response[10:14])
    payload_len, frame_id = struct.unpack("<II", response[14:22])
    if magic != b"OV56RGB5":
        return False, 0, "响应 magic 错误"
    if version != 1:
        return False, 0, "响应 version 错误"
    if pixel_format != 1:
        return False, 0, "响应 pixel_format 错误"
    if width != 160 or height != 120:
        return False, 0, "响应图像尺寸错误"
    if payload_len != IMAGE_PAYLOAD_SIZE:
        return False, 0, "响应 payload_len 错误"
    payload = response[IMAGE_HEADER_SIZE:IMAGE_HEADER_SIZE + payload_len]
    if len(payload) != IMAGE_PAYLOAD_SIZE:
        return False, 0, "实际 payload 长度错误"
    crc_start = IMAGE_HEADER_SIZE + payload_len
    crc_end = crc_start + IMAGE_CRC_SIZE
    if crc_end > len(response):
        return False, 0, "响应末尾缺少 CRC"
    received_crc = struct.unpack("<I", response[crc_start:crc_end])[0]
    calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if received_crc != calculated_crc:
        return False, 0, "payload CRC错误"
    return True, frame_id, ""

def expect_silence(ser, request):
    """发送错误请求，并检查等待期间 STM32 是否完全静默。"""
    ser.reset_input_buffer()
    ser.write(request)
    ser.flush()
    time.sleep(SILENCE_WAIT_SECONDS)
    received = ser.read_all()
    return len(received) == 0, received

def send_valid_request(ser, seq):
    """发送一条合法请求，并返回图像校验结果和耗时。"""
    request = build_request(seq)
    ser.reset_input_buffer()
    start_time = time.monotonic()
    ser.write(request)
    ser.flush()
    response = read_exact(ser, IMAGE_FRAME_SIZE, TIMEOUT_SECONDS)
    elapsed_ms = (time.monotonic() - start_time) * 1000.0
    success, frame_id, error_message = check_response(response)
    return success, frame_id, elapsed_ms, error_message

def main():
    """解析参数并执行完整测试流程。"""
    baseline_success = False
    silence_success_count = 0
    silence_failure_count = 0
    recovery_success_count = 0
    recovery_failure_count = 0
    timeout_recovery_success = False
    print(f"串口：{PORT}")
    print(f"波特率：{BAUD}")
    print(f"错误帧静默等待：{SILENCE_WAIT_SECONDS}秒")
    print(f"截断帧超时等待：{DISCARD_TIMEOUT_WAIT_SECONDS}秒")

    try:
        ser = serial.Serial()
        try:
            ser.port = PORT
            ser.baudrate = BAUD
            ser.timeout = 0.2
            ser.write_timeout = 2.0
            ser.rtscts = False
            ser.dsrdtr = False
            # open 前关闭控制线，避免 CH340 自动下载电路切换 BOOT0 或复位 MCU。
            ser.dtr = False
            ser.rts = False
            ser.open()

            print(f"DTR状态：{ser.dtr}")
            print(f"RTS状态：{ser.rts}")
            time.sleep(0.2)
            ser.reset_output_buffer()

            success, frame_id, elapsed_ms, error_message = send_valid_request(ser, 0x5000)
            baseline_success = success
            if success:
                print(f"基线合法请求：PASS，frame_id={frame_id}，耗时={elapsed_ms:.1f} ms")
            else:
                print(f"基线合法请求：FAIL：{error_message}")
                print("请先检查 COM4、固件版本和开发板状态。")

            error_cases = [
                ("VERSION_ERROR", build_request(0x5101, version=0x02), 0x5201),
                ("TYPE_ERROR", build_request(0x5102, msg_type=0x21), 0x5202),
                ("LENGTH_ERROR", build_request(0x5103, payload_len=1), 0x5203),
                ("CRC_ERROR", build_request(0x5104, corrupt_crc=True), 0x5204),
                ("EOF_ERROR", build_request(0x5105, eof0=0x00), 0x5205),
            ]

            for index, error_case in enumerate(error_cases):
                name, bad_request, recovery_seq = error_case
                print(f"\n[{index + 1}/{ERROR_CASE_COUNT}] {name}")
                silent, unexpected = expect_silence(ser, bad_request)
                if silent:
                    silence_success_count += 1
                    print("  错误帧静默：PASS，收到 0 B")
                else:
                    silence_failure_count += 1
                    print(f"  错误帧静默：FAIL，意外收到 {len(unexpected)} B")
                    print(f"  意外数据：{unexpected[:64].hex(' ')}")

                time.sleep(RECOVERY_INTERVAL_SECONDS)
                success, frame_id, elapsed_ms, error_message = send_valid_request(
                    ser, recovery_seq
                )
                if success:
                    recovery_success_count += 1
                    print(f"  合法请求恢复：PASS，frame_id={frame_id}，耗时={elapsed_ms:.1f} ms")
                else:
                    recovery_failure_count += 1
                    print(f"  合法请求恢复：FAIL：{error_message}")

            # 截断帧在 version 错误后不再发送剩余尾部，用于验证隔离超时恢复。
            print("\n截断错误帧超时恢复")
            ser.reset_input_buffer()
            ser.write(b"\xA5\x5A\x02")
            ser.flush()
            time.sleep(DISCARD_TIMEOUT_WAIT_SECONDS)
            unexpected = ser.read_all()
            truncated_silent = len(unexpected) == 0
            if truncated_silent:
                print("  截断帧等待期间静默：PASS，收到 0 B")
            else:
                print(f"  截断帧等待期间静默：FAIL，意外收到 {len(unexpected)} B")
                print(f"  意外数据：{unexpected[:64].hex(' ')}")

            success, frame_id, elapsed_ms, error_message = send_valid_request(ser, 0x5300)
            timeout_recovery_success = truncated_silent and success
            if success:
                print(f"  合法请求恢复：PASS，frame_id={frame_id}，耗时={elapsed_ms:.1f} ms")
            else:
                print(f"  合法请求恢复：FAIL：{error_message}")
        finally:
            if ser.is_open:
                ser.close()

    except serial.SerialException as error:
        print(f"串口错误：{error}")
        print(f"无法打开或使用 {PORT}，请确认 MobaXterm 已经关闭。")
        print("测试结果：FAIL")
        return 1
    except OSError as error:
        print(f"系统错误：{error}")
        print("测试结果：FAIL")
        return 1
    except Exception as error:
        # 兜底仅用于显示未预期错误，不用于掩盖正常校验失败。
        print(f"未预期错误：{error}")
        print("测试结果：FAIL")
        return 1

    print("\n========== 测试汇总 ==========")
    print(f"基线合法请求：{'PASS' if baseline_success else 'FAIL'}")
    print(f"错误类型总数：{ERROR_CASE_COUNT}")
    print(f"错误帧静默成功：{silence_success_count}")
    print(f"错误帧静默失败：{silence_failure_count}")
    print(f"错误后恢复成功：{recovery_success_count}")
    print(f"错误后恢复失败：{recovery_failure_count}")
    print(f"截断帧超时恢复：{'PASS' if timeout_recovery_success else 'FAIL'}")

    passed = (
        baseline_success
        and silence_success_count == ERROR_CASE_COUNT
        and silence_failure_count == 0
        and recovery_success_count == ERROR_CASE_COUNT
        and recovery_failure_count == 0
        and timeout_recovery_success
    )
    print(f"测试结果：{'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
