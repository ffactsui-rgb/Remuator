#!/usr/bin/env python3
"""Upload root web assets through the existing file manager, without erasing LittleFS.
Usage: python tools/upload_web.py http://192.168.4.1:8080
Programs in /prog and execution state in /state are not uploaded or removed.
"""
import argparse
import base64
import getpass
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="Адрес файлового менеджера с портом 8080")
    parser.add_argument("--user", default="admin", help="Имя пользователя менеджера")
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.url)
    if url.scheme not in ("http", "https") or not url.hostname or url.username:
        parser.error("Укажите http://АДРЕС:8080 без пароля в URL")
    password = getpass.getpass("Пароль файлового менеджера: ")
    auth = base64.b64encode(f"{args.user}:{password}".encode()).decode()
    directory = Path(__file__).resolve().parents[1] / "data"
    files = sorted(
        (p for p in directory.iterdir() if p.is_file()),
        key=lambda p: (p.name == "filemanager.html", p.name),
    )
    endpoint = urllib.parse.urlunsplit((url.scheme, url.netloc, "/api/upload", "dir=/", ""))
    for file in files:
        boundary = "remuator" + uuid.uuid4().hex
        payload = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{file.name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + file.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
        request = urllib.request.Request(
            endpoint,
            data=payload,
            method="POST",
            headers={
                "Authorization": "Basic " + auth,
                "Content-Type": "multipart/form-data; boundary=" + boundary,
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                response.read()
        except (urllib.error.URLError, OSError) as error:
            print(f"Не удалось загрузить {file.name}: {error}", file=sys.stderr)
            print("Устраните причину и повторите команду. Программы /prog не затронуты.", file=sys.stderr)
            return 1
        print(f"Загружен /{file.name}")
    print(f"Готово: {len(files)} веб-файлов в корне. Обновите страницу Remuage-VRN: версия считывается из самой прошивки.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
