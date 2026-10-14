# Traduce las direcciones "samc.asi+0x..." de un informe de muestras a nombres de función usando
# samc.map, y suma las muestras por función.
# Uso (Python de Blender): python simbolos.py codigo\bin\samc.map resultados\<etiqueta>_muestras.txt
import bisect
import re
import sys


def load_map(path):
    syms = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^([0-9a-f]{8}) ([0-9a-f]{8})\s+\d+\s{9,}(\S.*)$", line.rstrip("\n"))
        if m and int(m.group(2), 16) == 0:
            syms.append((int(m.group(1), 16), m.group(3).strip()))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    addrs, names = load_map(sys.argv[1])
    per_func = {}
    for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
        m = re.search(r"^\s*([\d.]+)%\s+(\d+)\s+\S+\s+samc\.asi\+0x([0-9A-Fa-f]+)", line)
        if not m:
            continue
        rva = int(m.group(3), 16)
        i = bisect.bisect_right(addrs, rva) - 1
        name = names[i] if i >= 0 else "?"
        per_func[name] = per_func.get(name, 0.0) + float(m.group(1))
        print(f"{float(m.group(1)):6.2f}%  +0x{rva:05X}  {name[:110]}")
    print("\nPor función (solo las instrucciones listadas en el informe):")
    for name, pct in sorted(per_func.items(), key=lambda kv: -kv[1]):
        print(f"{pct:6.2f}%  {name[:120]}")


if __name__ == "__main__":
    main()
