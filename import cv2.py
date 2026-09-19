import cv2
from PIL import Image

# Konfiguration
VIDEO_PATH = "/Users/lucahinrichs/Desktop/test.mpf.mp4"
OUTPUT_H_FILE = "/Users/lucahinrichs/Documents/PlatformIO/Projects/Waveshare_RLCD_Test/src/video_data.h"
WIDTH = 400
HEIGHT = 300

cap = cv2.VideoCapture(VIDEO_PATH)
frame_count = 0
all_bytes = []

print("Verarbeite Video-Frames...")

while cap.isOpened():
    ret, frame = cap.read()
    if not ret:
        break  # Video zu Ende

    # 1. OpenCV Frame (BGR) zu Pillow Image (RGB) konvertieren
    frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    img = Image.fromarray(frame_rgb)

    # 2. Größe exakt auf 400x300 anpassen
    img = img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

    # 3. Dithering anwenden (konvertiert in reines 1-Bit Schwarz/Weiß)
    img_bw = img.convert("1")

    # 4. Pixel in Bytes packen (8 Pixel = 1 Byte)
    # Adafruit GFX erwartet horizontal geschriebene Bytes
    frame_bytes = bytearray(img_bw.tobytes())
    all_bytes.extend(frame_bytes)
    
    frame_count += 1

cap.release()

# 5. C++ Header-Datei schreiben
with open(OUTPUT_H_FILE, "w") as f:
    f.write("#include <font_definitions.h> // Falls nötig\n")
    f.write("#include <avr/pgmspace.h>\n\n")
    f.write(f"// Frames gesamt: {frame_count}\n")
    f.write(f"// Gesamtgröße: {len(all_bytes)} Bytes\n")
    f.write(f"const int VIDEO_FRAMES_COUNT = {frame_count};\n\n")
    f.write("const unsigned char mein_video_stream[] PROGMEM = {\n")
    
    # Bytes formatiert in Zeilen rausschreiben (z.B. 16 Hex-Werte pro Zeile)
    for i in range(0, len(all_bytes), 16):
        chunk = all_bytes[i:i+16]
        hex_line = ", ".join([f"0x{b:02X}" for b in chunk])
        f.write(f"    {hex_line},\n")
        
    f.write("};\n")

print(f"Fertig! {frame_count} Frames in '{OUTPUT_H_FILE}' gespeichert.")