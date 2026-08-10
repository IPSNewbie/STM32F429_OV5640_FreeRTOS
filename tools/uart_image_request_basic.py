"""UART 二进制图像请求协议基础验证工具。"""

import struct
import sys
import time
import zlib

import serial


PORT = "COM4"
BAUD = 115200
TIMEOUT_SECONDS = 8.0
REQUEST_SEQ = 0x1234

REQUEST_FRAME_SIZE = 14
IMAGE_HEADER_SIZE = 22
IMAGE_PAYLOAD_SIZE = 38400
IMAGE_CRC_SIZE = 4
IMAGE_FRAME_SIZE = 38426


def build_request(seq):
    """按协议动态构造一帧 14 B 图像请求。"""
    if not 0 <= seq <= 0xFFFF:
        raise ValueError("请求 seq 必须在 0 到 65535 之间。")

    body = struct.pack("<BBHH", 0x01, 0x20, seq, 0)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    request = b"\xA5\x5A" + body + struct.pack("<I", crc) + b"\x0D\x0A"
    if len(request) != REQUEST_FRAME_SIZE:
        raise ValueError("请求帧长度错误。")
    return request, crc


def read_exact(ser, expected_size, timeout_seconds):
    """循环接收，直到达到目标长度或总超时。"""
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while len(data) < expected_size and time.monotonic() < deadline:
        chunk = ser.read(expected_size - len(data))
        if chunk:
            data.extend(chunk)
    return bytes(data)


def main():
    """解析参数并执行完整测试流程。"""
    try:
        request, request_crc = build_request(REQUEST_SEQ)
        print(f"串口：{PORT}")
        print(f"波特率：{BAUD}")
        print(f"请求 seq：0x{REQUEST_SEQ:04X}")
        print(f"请求长度：{len(request)} B")
        print(f"请求十六进制：{request.hex(' ')}")
        print(f"请求 CRC32：0x{request_crc:08X}")

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
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            ser.write(request)
            ser.flush()
            print("\n开始接收 OV56RGB5 图像帧……")
            response = read_exact(ser, IMAGE_FRAME_SIZE, TIMEOUT_SECONDS)
        finally:
            if ser.is_open:
                ser.close()

        print(f"响应长度：{len(response)} B")
        if len(response) != IMAGE_FRAME_SIZE:
            raise TimeoutError(f"接收超时，只收到 {len(response)}/{IMAGE_FRAME_SIZE} B。")

        # 22 B header 使用小端序解析，seq 与响应 frame_id 相互独立。
        magic = response[0:8]
        version = response[8]
        pixel_format = response[9]
        width, height = struct.unpack("<HH", response[10:14])
        payload_len, frame_id = struct.unpack("<II", response[14:22])

        if magic != b"OV56RGB5":
            raise ValueError("响应 magic 错误。")
        if version != 1 or pixel_format != 1:
            raise ValueError("响应版本或像素格式错误。")
        if width != 160 or height != 120:
            raise ValueError("响应图像尺寸错误。")
        if payload_len != IMAGE_PAYLOAD_SIZE:
            raise ValueError("响应 payload_len 错误。")
        if IMAGE_HEADER_SIZE + payload_len + IMAGE_CRC_SIZE != len(response):
            raise ValueError("响应总长度与 header 中的 payload_len 不一致。")

        payload = response[IMAGE_HEADER_SIZE:IMAGE_HEADER_SIZE + payload_len]
        if len(payload) != IMAGE_PAYLOAD_SIZE:
            raise ValueError("实际 payload 长度错误。")
        received_crc = struct.unpack("<I", response[-IMAGE_CRC_SIZE:])[0]
        calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF

        print(f"magic：{magic}")
        print(f"version：{version}")
        print(f"pixel_format：{pixel_format}")
        print(f"图像尺寸：{width}x{height}")
        print(f"payload_len：{payload_len}")
        print(f"frame_id：{frame_id}")
        print(f"接收 CRC32：0x{received_crc:08X}")
        print(f"计算 CRC32：0x{calculated_crc:08X}")
        print(f"CRC 是否一致：{'是' if received_crc == calculated_crc else '否'}")
        if received_crc != calculated_crc:
            raise ValueError("payload CRC 校验失败。")

        print("测试结果：PASS")
        return 0
    except serial.SerialException as error:
        print(f"串口错误：{error}")
        print(f"无法打开或使用 {PORT}，请确认 MobaXterm 已经关闭。")
    except OSError as error:
        print(f"系统或超时错误：{error}")
    except Exception as error:
        # 兜底仅用于让初学版本显示明确错误，不用于隐藏程序问题。
        print(f"程序错误：{error}")

    print("测试结果：FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
