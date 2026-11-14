"""Upload the stock logos in data/img/*.tvb to the board's LittleFS over HTTP.

    python tools/push_logos.py 192.168.1.81            # only the missing ones
    python tools/push_logos.py 192.168.1.81 --force    # restore originals

Uses the web UI's own /upload endpoint, so images already on the board are
left alone and nothing needs reflashing. Without --force a logo that exists
on the board (e.g. one you replaced from the web UI) is skipped.
"""
import base64
import json
import pathlib
import sys
import urllib.request

if len(sys.argv) < 2:
    sys.exit(__doc__)
host = sys.argv[1]
force = "--force" in sys.argv[2:]
base = f"http://{host}"

data = pathlib.Path(__file__).resolve().parent.parent / "data" / "img"
on_board = {i["n"] for i in json.load(urllib.request.urlopen(base + "/list", timeout=10))}

for f in sorted(data.glob("*.tvb")):
    name = f.stem
    if name in on_board and not force:
        print(f"skip   {name} (already on board)")
        continue
    body = base64.b64encode(f.read_bytes())
    req = urllib.request.Request(f"{base}/upload?name={name}", data=body, method="POST",
                                 headers={"Content-Type": "text/plain"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            print(f"upload {name}: {r.read().decode()}")
    except urllib.error.HTTPError as e:
        print(f"upload {name}: FAILED {e.code} {e.read().decode()}")
