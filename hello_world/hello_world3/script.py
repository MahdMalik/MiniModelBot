# script.py — Laptop webcam sender for ESP32-S3 WebSocket CV node
import asyncio
import cv2
import websockets

# ─── CONFIG ───────────────────────────────────────────────
ESP32_IP      = "10.217.157.203"  # ← IP printed in Serial Monitor
ESP32_PORT    = 8765
TARGET_FPS    = 10                # Lower = less load on ESP32
JPEG_QUALITY  = 50                # 0-100, lower = smaller frames
# ──────────────────────────────────────────────────────────

# IMPORTANT: must match WS_ENDPOINT in websocket.h ("/ws")
URI = f"ws://{ESP32_IP}:{ESP32_PORT}/ws"

async def stream_webcam():
    cap = cv2.VideoCapture(0)   # 0 = default webcam
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  320)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

    if not cap.isOpened():
        print("ERROR: Could not open webcam.")
        return

    print(f"Connecting to ESP32 at {URI} ...")

    try:
        async with websockets.connect(
            URI,
            ping_interval=20,    # send WebSocket pings every 20 s
            ping_timeout=10,     # drop connection if no pong in 10 s
            open_timeout=10      # fail fast if ESP32 unreachable
        ) as ws:

            # ── Wait for READY signal from ESP32 ──────────
            print("Waiting for READY signal from ESP32...")
            try:
                ready = await asyncio.wait_for(ws.recv(), timeout=5.0)
                if ready == "READY":
                    print("ESP32 says READY — starting stream.")
                else:
                    print(f"Unexpected handshake message: '{ready}' — continuing anyway.")
            except asyncio.TimeoutError:
                print("WARNING: No READY signal received within 5 s — continuing anyway.")

            # ── Streaming loop ────────────────────────────
            frame_interval = 1.0 / TARGET_FPS
            frame_count = 0
            print(f"Streaming at up to {TARGET_FPS} FPS. Press Ctrl+C to stop.")

            while True:
                loop_start = asyncio.get_event_loop().time()

                ret, frame = cap.read()
                if not ret:
                    print("ERROR: Failed to capture frame.")
                    break

                # Encode frame as JPEG bytes
                ok, buf = cv2.imencode(
                    ".jpg", frame,
                    [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY]
                )
                if not ok:
                    continue

                # Send binary JPEG to ESP32
                await ws.send(buf.tobytes())
                frame_count += 1
                if frame_count % TARGET_FPS == 0:
                    print(f"  Sent {frame_count} frames... (Ctrl+C to stop)")

                # Wait for ACK so ESP32 can pace us
                try:
                    ack = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    if ack != "ACK":
                        print(f"Unexpected message from ESP32: '{ack}'")
                except asyncio.TimeoutError:
                    pass  # No ACK in time — just keep going

                # Pace to target FPS
                elapsed    = asyncio.get_event_loop().time() - loop_start
                sleep_time = frame_interval - elapsed
                if sleep_time > 0:
                    await asyncio.sleep(sleep_time)

    except websockets.exceptions.InvalidURI:
        print(f"ERROR: Invalid URI — {URI}")
        print("Check ESP32_IP and ESP32_PORT in this script.")

    except websockets.exceptions.ConnectionClosedError as e:
        print(f"ERROR: Connection was rejected or closed by ESP32: {e}")
        print("Make sure the ESP32 is running and the IP/port match the Serial Monitor output.")

    except (ConnectionRefusedError, OSError) as e:
        print(f"ERROR: Could not reach ESP32 at {URI}")
        print(f"  Detail: {e}")
        print("  → Is the ESP32 powered on and connected to the same network?")
        print(f"  → Confirm IP in Serial Monitor matches ESP32_IP = '{ESP32_IP}'")
        print(f"  → Confirm port matches WS_PORT = {ESP32_PORT}")

    finally:
        cap.release()
        print("Stream stopped.")

if __name__ == "__main__":
    asyncio.run(stream_webcam()) 