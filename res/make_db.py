#!/usr/bin/env python3
"""Builds the two resource archives the ID6Term manifest names:

  ID6Term_ALL_Texts.zip   <lang>.txt files with "id=text" lines (the format of BMW's PluginsCoded apps)
  ID6Term_ALL_Images.zip  <imageId>.png: 55001 = 63x54 entry-row icon, 55002 = 549x484 preview

The text ids are shared with res/ID6Term.xml (textIdModel textId=...) and src/id6term_app.cpp (kTextAbout,
kQuickCommands). Images are drawn here (a dark terminal window with a green prompt), so no binary assets are
needed; replace the PNGs in the zip if you want your own.

Usage: python3 res/make_db.py [outdir]   (default: res/)
"""
import os
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else HERE

LANGS = ["de", "en-UK", "en-US", "es-ES", "es-US", "fr-FR", "fr-CA", "it", "pt", "ru"]
LANG_NAMES = {"de": "Deutsch", "en-UK": "English (UK)", "en-US": "English (US)", "es-ES": "Espanol",
              "es-US": "Espanol (US)", "fr-FR": "Francais", "fr-CA": "Francais (CA)", "it": "Italiano",
              "pt": "Portugues", "ru": "Russian"}

QUICK_COMMANDS = ["uname -a", "pidin arg", "ifconfig", "df", "ls -la /", "ls /mnt /fs /dev/shmem",
                  "sloginfo | tail -n 40", "date"]   # must match kQuickCommands in id6term_app.cpp

AUTHOR = "Joseph Robinson"   # watermark: menu title, About, console banner (kAuthor in id6term_app.cpp)

ABOUT = ("ID6Term by " + AUTHOR + "\\n"
         "A shell terminal inside iDrive.\\n"
         "Each line runs through /bin/sh on the head unit, as the HMI's user.\\n"
         "Console shows the output, Enter command opens the speller, a suggestion row runs that command.\\n"
         "Be careful: a wrong command can break the head unit. There is no undo.\\n"
         "Built with the RHMI PluginApp framework, following NBT-AAD (github.com/Sq00ky/Supra-iDrive6-Android-Auto).")

TEXTS = {
    1: "Terminal",                     # entry row
    10: "Console",
    11: "Enter command",
    12: "Run last command again",
    13: "Quick commands",
    14: "Stop running command",
    15: "Clear console",
    16: "About",
    17: ABOUT,                        # About page (Richtext list; "\n" in the .txt is a line break)
    18: "Terminal - " + AUTHOR,        # title of our menu / preview screen (the entry row itself stays "Terminal")
    20: "Command",                    # speller title
}
for i, cmd in enumerate(QUICK_COMMANDS):
    TEXTS[30 + i] = cmd

LANG_TEXTS = {
    "de": {1: "Terminal", 10: "Konsole", 11: "Befehl eingeben", 12: "Letzten Befehl wiederholen",
           13: "Schnellbefehle", 14: "Laufenden Befehl beenden", 15: "Konsole leeren", 16: "Info", 20: "Befehl"},
    "es-ES": {10: "Consola", 11: "Introducir comando", 12: "Repetir el ultimo comando", 13: "Comandos rapidos",
              14: "Detener el comando en curso", 15: "Borrar la consola", 16: "Acerca de", 20: "Comando"},
    "fr-FR": {10: "Console", 11: "Saisir une commande", 12: "Relancer la derniere commande", 13: "Commandes rapides",
              14: "Arreter la commande en cours", 15: "Effacer la console", 16: "A propos", 20: "Commande"},
    "it": {10: "Console", 11: "Inserisci comando", 12: "Ripeti l'ultimo comando", 13: "Comandi rapidi",
           14: "Interrompi il comando in corso", 15: "Svuota la console", 16: "Informazioni", 20: "Comando"},
    "pt": {10: "Consola", 11: "Introduzir comando", 12: "Repetir o ultimo comando", 13: "Comandos rapidos",
           14: "Parar o comando em curso", 15: "Limpar a consola", 16: "Sobre", 20: "Comando"},
}
LANG_TEXTS["es-US"] = LANG_TEXTS["es-ES"]
LANG_TEXTS["fr-CA"] = LANG_TEXTS["fr-FR"]


# ---- a tiny PNG writer (RGBA, no dependencies) -----------------------------------------------------------------
def png_bytes(width, height, pixel):
    """pixel(x, y) -> (r, g, b, a)"""
    raw = bytearray()
    for y in range(height):
        raw.append(0)                       # filter: none
        for x in range(width):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def terminal_image(width, height):
    """A dark rounded window with a thin light frame and a green '>_' prompt, scaled to the box."""
    margin = max(1, min(width, height) // 18)
    radius = max(2, min(width, height) // 9)
    frame = max(1, min(width, height) // 40)
    bg = (24, 28, 34, 255)
    edge = (150, 160, 175, 255)
    green = (80, 220, 110, 255)
    clear = (0, 0, 0, 0)

    def in_round_rect(x, y, x0, y0, x1, y1, r):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = x0 + r if x < x0 + r else (x1 - r if x > x1 - r else x)
        cy = y0 + r if y < y0 + r else (y1 - r if y > y1 - r else y)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r

    # prompt geometry (">" chevron and "_" underline), proportional
    gw, gh = width - 2 * margin, height - 2 * margin
    stroke = max(2, gh // 10)
    chev_x0 = margin + gw * 0.18
    chev_x1 = margin + gw * 0.42
    chev_y0 = margin + gh * 0.30
    chev_y1 = margin + gh * 0.70
    chev_ym = (chev_y0 + chev_y1) / 2
    ul_x0 = margin + gw * 0.50
    ul_x1 = margin + gw * 0.80
    ul_y = margin + gh * 0.70

    def on_segment(px, py, ax, ay, bx, by, w):
        vx, vy = bx - ax, by - ay
        L2 = vx * vx + vy * vy
        t = 0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / L2))
        dx, dy = px - (ax + t * vx), py - (ay + t * vy)
        return dx * dx + dy * dy <= (w / 2) ** 2

    def pixel(x, y):
        if not in_round_rect(x, y, margin, margin, width - 1 - margin, height - 1 - margin, radius):
            return clear
        if not in_round_rect(x, y, margin + frame, margin + frame, width - 1 - margin - frame,
                             height - 1 - margin - frame, max(1, radius - frame)):
            return edge
        if (on_segment(x, y, chev_x0, chev_y0, chev_x1, chev_ym, stroke)
                or on_segment(x, y, chev_x1, chev_ym, chev_x0, chev_y1, stroke)
                or on_segment(x, y, ul_x0, ul_y, ul_x1, ul_y, stroke)):
            return green
        return bg

    return png_bytes(width, height, pixel)


def main():
    texts_zip = os.path.join(OUT, "ID6Term_ALL_Texts.zip")
    with zipfile.ZipFile(texts_zip, "w", zipfile.ZIP_DEFLATED) as z:
        for lang in LANGS:
            texts = dict(TEXTS)
            texts.update(LANG_TEXTS.get(lang, {}))
            body = "100000=%s\n" % LANG_NAMES[lang] + "".join("%d=%s\n" % (k, v) for k, v in sorted(texts.items()))
            z.writestr(lang + ".txt", body.encode("utf-8"))
    images_zip = os.path.join(OUT, "ID6Term_ALL_Images.zip")
    with zipfile.ZipFile(images_zip, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("55001.png", terminal_image(63, 54))      # entry-row icon (imageIdModel 4)
        z.writestr("55002.png", terminal_image(549, 484))    # content preview (imageIdModel 12)
    print("wrote %s, %s (max text id %d)" % (texts_zip, images_zip, max(TEXTS)))


if __name__ == "__main__":
    main()
