# Compara dos capturas BMP (24 bits) del laboratorio y genera una imagen con: original | con fila | diferencia x8
# Uso (con el Python de Blender, que trae numpy):
#   python comparar_capturas.py antes.bmp despues.bmp salida.png
#   python comparar_capturas.py antes.bmp despues.bmp salida.png x0 y0 x1 y1 escala   (recorte ampliado, uno sobre otro)
import struct
import sys
import zlib

import numpy as np


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path}: se esperaba BMP de 24 bits")
    row = (width * 3 + 3) & ~3
    top_down = height < 0
    height = abs(height)
    img = np.frombuffer(data, dtype=np.uint8, count=row * height, offset=offset).reshape(height, row)[:, : width * 3]
    img = img.reshape(height, width, 3)[:, :, ::-1]  # BGR -> RGB
    return img if top_down else img[::-1]


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    a = read_bmp(sys.argv[1]).astype(np.int16)
    b = read_bmp(sys.argv[2]).astype(np.int16)
    diff = np.abs(a - b)
    per_pixel = diff.max(axis=2)
    total = per_pixel.size
    print(f"Diferencia media por canal: {diff.mean():.3f} (de 255)")
    print(f"Pixeles identicos: {100.0 * (per_pixel == 0).sum() / total:.2f}%")
    print(f"Pixeles con diferencia > 8: {100.0 * (per_pixel > 8).sum() / total:.3f}%")
    print(f"Pixeles con diferencia > 32: {100.0 * (per_pixel > 32).sum() / total:.3f}%")
    if len(sys.argv) >= 9:
        # Recorte ampliado (vecino más cercano), antes arriba y después abajo
        x0, y0, x1, y1, k = (int(v) for v in sys.argv[4:9])
        crop = lambda img: np.repeat(np.repeat(img[y0:y1, x0:x1], k, axis=0), k, axis=1)
        sep = np.full((6, (x1 - x0) * k, 3), 255, dtype=np.int16)
        strip = np.concatenate([crop(a), sep, crop(b)], axis=0).astype(np.uint8)
    else:
        # Imagen de comparación a media resolución: original | con fila | diferencia amplificada
        half = lambda img: img[::2, ::2]
        amp = np.clip(diff * 8, 0, 255)
        strip = np.concatenate([half(a), half(b), half(amp)], axis=1).astype(np.uint8)
    write_png(sys.argv[3], strip)
    print(f"Imagen de comparacion: {sys.argv[3]}")


if __name__ == "__main__":
    main()
