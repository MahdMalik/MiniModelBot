# laptop_sender.py
# Uses pygame (webcam capture + preview window) and
# Pillow (JPEG encoding) instead of cv2.
#
# Install dependencies:
#   pip install pygame Pillow websockets

import asyncio
import io
import pygame
import pygame.camera
from PIL import Image
import websockets

ESP32_IP      = "10.217.157.98"   # ← Replace with your ESP32's IP (printed in Serial Monitor)
ESP32_PORT    = 8765
TARGET_FPS    = 10                # Lower = less load on ESP32
JPEG_QUALITY  = 50                # 0-100, lower = smaller frames
CAM_WIDTH     = 320
CAM_HEIGHT    = 240

async def stream_webcam():
    uri = f"ws://{ESP32_IP}:{ESP32_PORT}/ws"

    # ── Init pygame and camera ────────────────────────────────────────────────
    pygame.init()
    pygame.camera.init()

    cam_list = pygame.camera.list_cameras()
    if not cam_list:
        print("ERROR: No cameras found.")
        return
    print(f"Using camera: {cam_list[0]}")

    cam = pygame.camera.Camera(cam_list[0], (CAM_WIDTH, CAM_HEIGHT))
    cam.start()
    print("Camera started.")

    # Preview window
    screen = pygame.display.set_mode((CAM_WIDTH, CAM_HEIGHT))
    pygame.display.set_caption("Sending to ESP32")

    print(f"Connecting to ESP32 at {uri} ...")

    try:
        async with websockets.connect(uri) as ws:
            print("Connected! Streaming frames. Close window or press Ctrl+C to stop.")
            frame_interval = 1.0 / TARGET_FPS

            while True:
                loop_start = asyncio.get_event_loop().time()

                # ── Handle pygame window close event ─────────────────────────
                for event in pygame.event.get():
                    if event.type == pygame.QUIT:
                        print("Window closed. Stopping.")
                        cam.stop()
                        pygame.quit()
                        return

                # ── Capture frame from webcam ─────────────────────────────────
                if not cam.query_image():
                    await asyncio.sleep(0.01)
                    continue

                surface = cam.get_image()

                # ── Show local preview ────────────────────────────────────────
                screen.blit(surface, (0, 0))
                pygame.display.flip()

                # ── Encode frame as JPEG using Pillow ─────────────────────────
                # pygame surface → raw RGB bytes → Pillow Image → JPEG bytes
                raw_str = pygame.image.tostring(surface, "RGB")
                pil_img = Image.frombytes("RGB", (CAM_WIDTH, CAM_HEIGHT), raw_str)

                buf = io.BytesIO()
                pil_img.save(buf, format="JPEG", quality=JPEG_QUALITY)
                jpeg_bytes = buf.getvalue()

                # ── Send binary JPEG to ESP32 ─────────────────────────────────
                await ws.send(jpeg_bytes)

                # ── Wait for ACK from ESP32 (comment out for max speed) ───────
                try:
                    ack = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    # print(f"ACK: {ack}")  # Uncomment to debug
                except asyncio.TimeoutError:
                    pass  # Continue even if no ACK arrives

                # ── Pace to target FPS ────────────────────────────────────────
                elapsed = asyncio.get_event_loop().time() - loop_start
                sleep_time = frame_interval - elapsed
                if sleep_time > 0:
                    await asyncio.sleep(sleep_time)

    except websockets.exceptions.ConnectionClosed as e:
        print(f"Connection closed: {e}")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        cam.stop()
        pygame.quit()
        print("Camera released. Done.")

if __name__ == "__main__":
    asyncio.run(stream_webcam())