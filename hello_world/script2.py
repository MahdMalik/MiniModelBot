# inference_client.py
# Drop this anywhere you like — it has no dependency on the other project files.
# It replaces the role of script.py entirely:
#   - Captures webcam frames using pygame + Pillow (no cv2 needed)
#   - Sends each frame as a JPEG binary over WebSocket to the ESP32
#   - Receives the inference result (JSON) back from the ESP32
#   - Displays the live preview with the result overlaid on screen
#
# Install dependencies (in whichever Python env you're using):
#   pip install pygame Pillow websockets
#
# Run:
#   python inference_client.py

import asyncio
import io
import json
import pygame
import pygame.camera
from PIL import Image
import websockets

# ─── CONFIG — edit these to match your setup ──────────────────────────────────
ESP32_IP     = "10.217.157.98"  # IP printed to serial monitor on ESP32 boot
ESP32_PORT   = 8765
TARGET_FPS   = 10               # frames per second sent to ESP32
JPEG_QUALITY = 50               # 0-100, lower = smaller/faster
CAM_WIDTH    = 320
CAM_HEIGHT   = 240
# ──────────────────────────────────────────────────────────────────────────────

# Holds the most recent result sent back by the ESP32
latest_label = "waiting for ESP32…"
latest_class = -1

async def receive_results(ws):
    """Background task: listen for JSON results coming back from the ESP32."""
    global latest_label, latest_class
    async for message in ws:
        try:
            data = json.loads(message)
            if "label" in data and "class" in data:
                latest_class = data["class"]
                latest_label = data["label"]
                print(f"[ESP32 result]  class={latest_class}  label={latest_label}")
            elif "error" in data:
                print(f"[ESP32 error]   {data['error']}")
            # "READY" and "ACK" are plain strings, json.loads will raise — caught below
        except (json.JSONDecodeError, TypeError):
            # Plain text messages like "READY" / "ACK" — nothing to do
            pass

async def run():
    global latest_label, latest_class

    uri = f"ws://{ESP32_IP}:{ESP32_PORT}/ws"

    # ── Initialise pygame and camera ──────────────────────────────────────────
    pygame.init()
    pygame.camera.init()

    cameras = pygame.camera.list_cameras()
    if not cameras:
        print("ERROR: No camera found. Is your webcam connected?")
        return
    print(f"[Camera] Using: {cameras[0]}")

    cam = pygame.camera.Camera(cameras[0], (CAM_WIDTH, CAM_HEIGHT))
    cam.start()

    screen = pygame.display.set_mode((CAM_WIDTH, CAM_HEIGHT))
    pygame.display.set_caption("ESP32-S3 Live Inference")
    font = pygame.font.SysFont("monospace", 17, bold=True)

    print(f"[WS] Connecting to {uri} …")

    try:
        async with websockets.connect(uri) as ws:

            # Wait for the ESP32 to say it's ready
            try:
                ready_msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
                print(f"[ESP32] {ready_msg}")
            except asyncio.TimeoutError:
                print("[WS] Timed out waiting for READY. Continuing anyway.")

            # Start the background receiver
            recv_task = asyncio.create_task(receive_results(ws))

            frame_interval = 1.0 / TARGET_FPS
            running = True

            while running:
                t0 = asyncio.get_event_loop().time()

                # ── Handle window close ───────────────────────────────────────
                for event in pygame.event.get():
                    if event.type == pygame.QUIT:
                        running = False

                # ── Capture frame from webcam ─────────────────────────────────
                if not cam.query_image():
                    await asyncio.sleep(0.005)
                    continue

                surface = cam.get_image()

                # ── Encode as JPEG with Pillow ────────────────────────────────
                raw_bytes = pygame.image.tostring(surface, "RGB")
                pil_image = Image.frombytes("RGB", (CAM_WIDTH, CAM_HEIGHT), raw_bytes)
                buf = io.BytesIO()
                pil_image.save(buf, format="JPEG", quality=JPEG_QUALITY)
                jpeg_bytes = buf.getvalue()

                # ── Send frame to ESP32 ───────────────────────────────────────
                await ws.send(jpeg_bytes)

                # ── Draw preview + overlay result ─────────────────────────────
                screen.blit(surface, (0, 0))

                # Dark bar behind text
                bar = pygame.Surface((CAM_WIDTH, 28), pygame.SRCALPHA)
                bar.fill((0, 0, 0, 170))
                screen.blit(bar, (0, 0))

                label_text = f"Class {latest_class}: {latest_label}" \
                             if latest_class >= 0 else latest_label
                text_surf = font.render(label_text, True, (0, 255, 80))
                screen.blit(text_surf, (5, 5))
                pygame.display.flip()

                # ── Pace to target FPS ────────────────────────────────────────
                elapsed = asyncio.get_event_loop().time() - t0
                wait    = frame_interval - elapsed
                if wait > 0:
                    await asyncio.sleep(wait)

            recv_task.cancel()

    except websockets.exceptions.ConnectionClosed as e:
        print(f"[WS] Connection closed: {e}")
    except OSError as e:
        print(f"[WS] Could not connect to ESP32 at {uri}: {e}")
        print("     → Check the IP address and that the ESP32 is running.")
    except Exception as e:
        print(f"[Error] {e}")
    finally:
        cam.stop()
        pygame.quit()
        print("[Done]")

asyncio.run(run())