# Busca las zonas donde más difieren dos capturas BMP y arma una imagen con las N peores, ampliadas:
# por cada zona, de izquierda a derecha: antes | después | diferencia x4.
# Uso (Python de Blender): python zonas_distintas.py a.bmp b.bmp salida.png [zonas=4] [ancho=96] [alto=64] [escala=4]
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")
from comparar_capturas import read_bmp, write_png  # noqa: E402


def main():
    a = read_bmp(sys.argv[1]).astype(np.int16)
    b = read_bmp(sys.argv[2]).astype(np.int16)
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    w = int(sys.argv[5]) if len(sys.argv) > 5 else 96
    h = int(sys.argv[6]) if len(sys.argv) > 6 else 64
    k = int(sys.argv[7]) if len(sys.argv) > 7 else 4
    diff = np.abs(a - b).max(axis=2).astype(np.float64)
    # Suma de diferencias en ventanas w x h (imagen integral)
    ii = np.pad(diff, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    H, W = diff.shape
    score = ii[h:, w:] - ii[:-h, w:] - ii[h:, :-w] + ii[:-h, :-w]
    rows = []
    for _ in range(n):
        y, x = np.unravel_index(np.argmax(score), score.shape)
        if score[y, x] <= 0:
            break
        print(f"zona en x={x} y={y} ({w}x{h}): diferencia total {score[y, x]:.0f}")
        crop = lambda img: np.repeat(np.repeat(img[y:y + h, x:x + w], k, axis=0), k, axis=1)
        d = np.clip(np.abs(a - b) * 4, 0, 255)
        sep = np.full((h * k, 4, 3), 255, dtype=np.int16)
        rows.append(np.concatenate([crop(a), sep, crop(b), sep, crop(d)], axis=1))
        # no volver a elegir una zona que se superpone con esta
        score[max(0, y - h):y + h, max(0, x - w):x + w] = 0
    hsep = np.full((4, rows[0].shape[1], 3), 255, dtype=np.int16)
    out = rows[0]
    for r in rows[1:]:
        out = np.concatenate([out, hsep, r], axis=0)
    write_png(sys.argv[3], out.astype(np.uint8))
    print(f"Imagen: {sys.argv[3]}")


if __name__ == "__main__":
    main()
