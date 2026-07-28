import struct
import sys
import time
import zlib
import serial

PORT = "COM4"
BAUD = 115200
TIMEOUT_SECONDS = 8.0
REQUEST_COUNT = 20
REQUEST_INTERVAL_SECONDS = 0.2
START_SEQ = 1
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
    """检查单帧 OV56RGB5 响应的 header、长度和 payload CRC。"""
    if len(response) != IMAGE_FRAME_SIZE:
        return False, 0, "接收长度错误"
    # 总长度正确后再解析固定 22 B header，避免短数据触发解包错误。
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

def main():
    success_count = 0
    failure_count = 0
    elapsed_times = []
    received_frame_ids = []
    print(f"串口：{PORT}")
    print(f"波特率：{BAUD}")
    print(f"连续请求次数：{REQUEST_COUNT}")
    print(f"请求间隔：{REQUEST_INTERVAL_SECONDS}秒")
    print(f"预期响应长度：{IMAGE_FRAME_SIZE} B")

    try:
        with serial.Serial(port=PORT, baudrate=BAUD, timeout=0.2) as ser:
            # 保持硬件流控关闭，并让控制线进入本开发板已验证的安全状态。
            ser.setRTS(False)
            ser.setDTR(False)
            time.sleep(0.2)
            ser.reset_output_buffer()

            for index in range(REQUEST_COUNT):
                seq = (START_SEQ + index) & 0xFFFF
                request = build_request(seq)
                ser.reset_input_buffer()
                start_time = time.monotonic()
                ser.write(request)
                ser.flush()
                response = read_exact(ser, IMAGE_FRAME_SIZE, TIMEOUT_SECONDS)
                end_time = time.monotonic()
                elapsed_ms = (end_time - start_time) * 1000.0
                success, frame_id, error_message = check_response(response)
                if success:
                    success_count += 1
                    elapsed_times.append(elapsed_ms)
                    received_frame_ids.append(frame_id)
                    print(f"[{index + 1:02d}/{REQUEST_COUNT:02d}] seq=0x{seq:04X} "
                          f"frame_id={frame_id} 接收={len(response)} B "
                          f"耗时={elapsed_ms:.1f} ms PASS")
                else:
                    failure_count += 1
                    print(f"[{index + 1:02d}/{REQUEST_COUNT:02d}] seq=0x{seq:04X} "
                          f"接收={len(response)} B 耗时={elapsed_ms:.1f} ms "
                          f"FAIL：{error_message}")
                # 单次失败也继续下一次请求，用于观察接收链路能否恢复。
                time.sleep(REQUEST_INTERVAL_SECONDS)
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
    success_rate = success_count * 100.0 / REQUEST_COUNT
    print("\n========== 测试汇总 ==========")
    print(f"请求总数：{REQUEST_COUNT}")
    print(f"成功次数：{success_count}")
    print(f"失败次数：{failure_count}")
    print(f"成功率：{success_rate:.2f}%")
    if elapsed_times:
        average_ms = sum(elapsed_times) / len(elapsed_times)
        print(f"平均耗时：{average_ms:.2f} ms")
        print(f"最短耗时：{min(elapsed_times):.2f} ms")
        print(f"最长耗时：{max(elapsed_times):.2f} ms")
    else:
        print("平均耗时：无")
        print("最短耗时：无")
        print("最长耗时：无")
    frame_ids_continuous = True
    for index in range(1, len(received_frame_ids)):
        previous_id = received_frame_ids[index - 1]
        current_id = received_frame_ids[index]
        if current_id != ((previous_id + 1) & 0xFFFFFFFF):
            frame_ids_continuous = False
            break
    if received_frame_ids:
        print(f"首个 frame_id：{received_frame_ids[0]}")
        print(f"最后 frame_id：{received_frame_ids[-1]}")
        print(f"frame_id 是否连续：{'是' if frame_ids_continuous else '否'}")
        if not frame_ids_continuous:
            print("警告：frame_id 不连续")
    else:
        print("首个 frame_id：无")
        print("最后 frame_id：无")
        print("frame_id 是否连续：无")
    if success_count == REQUEST_COUNT and failure_count == 0:
        print("测试结果：PASS")
        return 0
    print("测试结果：FAIL")
    return 1

if __name__ == "__main__":
    sys.exit(main())
