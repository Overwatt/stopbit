#!/usr/bin/env python3
"""Print reload-button changes for a while.  tests/button_watch.py HOST [SECONDS]"""
import json, sys, time, urllib.request
end, last = time.time() + float(sys.argv[2] if len(sys.argv) > 2 else 60), None
while time.time() < end:
    b = json.load(urllib.request.urlopen(f"http://{sys.argv[1]}/api/hardware", timeout=5))["button"]
    if b != last: print(time.strftime("%H:%M:%S"), b, flush=True); last = b
    time.sleep(.2)
