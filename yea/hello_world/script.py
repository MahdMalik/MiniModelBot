# laptop_sender.py
import asyncio
import cv2
import websockets

ESP32_IP   = "192.168.X.X"   # ← Replace with your ESP32's IP (printed in Serial Monitor)
ESP32_PORT = 8765
TARGET_FPS = 10               # Lower = less load on ESP32
JPEG_QUALITY = 50             # 0-100, lower = smaller frames

async def stream_webcam():
    uri = f"ws://{ESP32_IP}:{ESP32_PORT}"
    cap = cv2.VideoCapture(0)          # 0 = default webcam
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  320)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

    if not cap.isOpened():
        print("ERROR: Could not open webcam.")
        return

    print(f"Connecting to ESP32 at {uri} ...")
    async with websockets.connect(uri) as ws:
        print("Connected! Streaming frames. Press Ctrl+C to stop.")
        frame_interval = 1.0 / TARGET_FPS

        while True:
            loop_start = asyncio.get_event_loop().time()

            ret, frame = cap.read()
            if not ret:
                print("ERROR: Failed to capture frame.")
                break

            # Encode frame as JPEG
            ok, buf = cv2.imencode(".jpg", frame,
                                   [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            if not ok:
                continue

            # Send binary JPEG to ESP32
            await ws.send(buf.tobytes())

            # Show local preview (optional, close window to stop)
            cv2.imshow("Sending to ESP32", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

            # Wait for ACK (optional — comment out if you want max speed)
            try:
                ack = await asyncio.wait_for(ws.recv(), timeout=1.0)
            except asyncio.TimeoutError:
                pass  # Continue even if no ACK

            # Pace to target FPS
            elapsed = asyncio.get_event_loop().time() - loop_start
            sleep_time = frame_interval - elapsed
            if sleep_time > 0:
                await asyncio.sleep(sleep_time)

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    asyncio.run(stream_webcam())