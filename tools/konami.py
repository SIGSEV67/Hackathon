#!/usr/bin/env python3
"""Dispara el easter egg (el kiwi) en una placa por USB, sin hacer el codigo.

Uso: tools/konami.py [puerto]     (default /dev/ttyACM0)

Alcanza con una placa: al explotar le avisa al resto de la mesh. Necesita
pyserial (viene con el entorno de ESP-IDF: `. ~/Hackathon/esp-idf/export.sh`).
La placa tiene que tener CONFIG_SIGSEV_EASTER_EGG prendido (menuconfig).
"""
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("Falta pyserial: activa el entorno de ESP-IDF o `pip install pyserial`.")

LISTEN_S = 2.0


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    # Se abre sin tocar DTR/RTS: esa combinacion es la que resetea la placa.
    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        s.write(b"konami\n")
        s.flush()
        out = b""
        start = time.time()
        while time.time() - start < LISTEN_S:
            out += s.read(4096)
    text = out.decode(errors="replace")
    sys.stdout.write(text)
    if "konami from the computer" in text:
        print("\n>> 6 7 ... BOOM")
        return 0
    print("\n>> La placa no respondio: esta el easter egg prendido? es el puerto correcto?")
    return 1


if __name__ == "__main__":
    sys.exit(main())
