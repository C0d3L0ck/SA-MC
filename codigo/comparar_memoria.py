# Compara cómo crece la memoria del juego en dos o más pruebas de paseo (<etiqueta>_memoria.csv).
# Para cada visita al aeropuerto (filas con autos cargados) muestra la memoria privada y las direcciones usadas.
# Uso (con el Python de Blender): python comparar_memoria.py resultados\a_memoria.csv resultados\b_memoria.csv
import csv
import sys


def visits(path):
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    out, cur = [], []
    for r in rows:
        if int(r["autos"]) >= 500:
            cur.append(r)
        elif cur:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    res = []
    for v in out:
        last = v[-1]
        res.append((int(float(v[0]["segundo"])), float(last["privada_mb"]), float(last["direcciones_usadas_mb"]),
                    float(last["mayor_libre_mb"]), sum(float(r["fps"]) for r in v) / len(v)))
    return rows, res


for path in sys.argv[1:]:
    rows, res = visits(path)
    print(f"\n{path}")
    print(f"  {'visita':>6} {'segundo':>8} {'privada MB':>11} {'direcciones MB':>15} {'mayor libre MB':>15} {'FPS':>6}")
    for i, (t, priv, va, free, fps) in enumerate(res, 1):
        print(f"  {i:>6} {t:>8} {priv:>11.1f} {va:>15.1f} {free:>15.1f} {fps:>6.1f}")
    if len(res) >= 3:
        span = res[-1][0] - res[1][0]
        if span > 0:
            print(f"  crecimiento desde la 2a visita: privada {60 * (res[-1][1] - res[1][1]) / span:+.1f} MB/min, "
                  f"direcciones {60 * (res[-1][2] - res[1][2]) / span:+.1f} MB/min")
    last = rows[-1]
    print(f"  final: privada {float(last['privada_mb']):.1f} MB, direcciones {float(last['direcciones_usadas_mb']):.1f} MB, "
          f"mayor libre {float(last['mayor_libre_mb']):.1f} MB")
