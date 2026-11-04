#!/Users/strato/.espressif/python_env/idf5.4_py3.13_env/bin/python
"""touch_calib.py - run the nine-crosshair touch calibration on a board and
fit the map from its log (firmware: director `ota calib`, update mode).

The board is found by its USB SERIAL, never by port name. If it is running
the tank it is sent `ota check` first (it saves and restarts into update
mode), then `ota calib` once per pass: nine crosses, a press each. The log's
"calib N: target X,Y press at X,Y" lines are collected and fitted per axis,
press = gain * target + offset, with the residuals.

  tools/touch_calib.py <usb serial>                         # the round board, two passes
  tools/touch_calib.py <usb serial> --passes 3  # the watch
  tools/touch_calib.py <serial> --flip               # the picture turned over: the board held the other way up
  tools/touch_calib.py <serial> --bias 0             # the y bias inside the log's press (director `touch` says)

The press in the log is what the port hands the pages: the board's map is
already in it (the y bias, any calibration). Uncalibrated, the fit IS the
panel's error; calibrated, gain ~1 and offset ~0 is the pass mark. The board
is reset back to the tank at the end (--stay leaves it in update mode).
"""
import argparse, re, sys, time
import serial
from serial.tools import list_ports

ap = argparse.ArgumentParser()
ap.add_argument("serial")
ap.add_argument("--passes", type=int, default=2)
ap.add_argument("--bias", type=float, default=None, help="y bias already subtracted in the logged press: printed back in as the raw y")
ap.add_argument("--flip", action="store_true", help="the picture turned over (director `ota flip on`): hold the board the other way up")
ap.add_argument("--stay", action="store_true", help="leave the board in update mode (on its keyboard page) instead of resetting it back to the tank")
ap.add_argument("--timeout", type=float, default=600)
ap.add_argument("--out")
a = ap.parse_args()

def find():
    for p in list_ports.comports():
        if (p.serial_number or "").upper() == a.serial.upper():
            return p.device.replace("/dev/tty.", "/dev/cu.")
    return None

def port_open():
    end = time.time() + 30
    while time.time() < end:
        dev = find()
        if dev:
            try:
                s = serial.Serial(); s.port, s.baudrate, s.timeout = dev, 115200, 0.1
                s.open(); s.reset_input_buffer()       # a plain open: DTR/RTS untouched, the board keeps running
                return s
            except (OSError, serial.SerialException):
                pass
        time.sleep(0.5)
    sys.exit(f"touch_calib: no board with USB serial {a.serial} on USB")

def ask(s, cmd, secs, want):
    s.write((cmd + "\n").encode()); s.flush()
    end = time.time() + secs; got = []
    while time.time() < end:
        try: raw = s.readline()
        except (OSError, serial.SerialException): break
        if raw:
            t = raw.decode("utf-8", "replace").rstrip(); got.append(t)
            if want in t: break
    return got

s = port_open()
print(f"touch_calib: {a.serial} is {s.port}", flush=True)
page = ask(s, "ota page", 3, "update mode:")
if not any("update mode: active" in l for l in page):
    print("touch_calib: the tank is running - saving, restarting into update mode", flush=True)
    ask(s, "ota check", 2, "restarting")
    s.close(); time.sleep(4)
    end = time.time() + 40
    while True:
        s = port_open()
        if any("update mode: active" in l for l in ask(s, "ota page", 3, "update mode:")): break
        s.close()
        if time.time() > end: sys.exit("touch_calib: the board never reached update mode")
        time.sleep(1)

pat = re.compile(r"calib (\d): target (-?\d+),(-?\d+) press at (-?\d+),(-?\d+)")
rows = []                                   # (pass, index, tx, ty, px, py)
deadline = time.time() + a.timeout
if a.flip: ask(s, "ota flip on", 2, "ota flip")
for n in range(a.passes):
    time.sleep(0.8)                         # the last cross's finger is off the glass
    ask(s, "ota calib", 2, "ota calib")
    print(f"touch_calib: pass {n + 1} of {a.passes} - nine crosses are up, tap each one", flush=True)
    seen = 0
    while seen < 9:
        if time.time() > deadline: sys.exit(f"touch_calib: timed out with {len(rows)} presses")
        try: raw = s.readline()
        except (OSError, serial.SerialException): sys.exit("touch_calib: the port went away")
        m = pat.search(raw.decode("utf-8", "replace")) if raw else None
        if m:
            i, tx, ty, px, py = map(int, m.groups())
            rows.append((n, i, tx, ty, px, py)); seen += 1
            print(f"  pass {n + 1} cross {i + 1}: target {tx:3d},{ty:3d}  press {px:3d},{py:3d}  off {px - tx:+4d},{py - ty:+4d}", flush=True)
if a.flip: time.sleep(0.8); ask(s, "ota flip off", 2, "ota flip")
if not a.stay:                              # a plain reset (RTS = the chip's EN, DTR low = a normal boot): update mode is one boot only, the tank comes back
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
    print("touch_calib: reset - back to the tank", flush=True)
s.close()

def fit(ts, ps):                            # least squares p = g * t + o
    n = len(ts); mt, mp = sum(ts) / n, sum(ps) / n
    g = sum((t - mt) * (p - mp) for t, p in zip(ts, ps)) / sum((t - mt) ** 2 for t in ts)
    o = mp - g * mt
    res = [p - (g * t + o) for t, p in zip(ts, ps)]
    return g, o, (sum(r * r for r in res) / n) ** 0.5, max(abs(r) for r in res)

lines = []
for name, ti, pi in (("x", 2, 4), ("y", 3, 5)):
    ts = [r[ti] for r in rows]; ps = [r[pi] for r in rows]
    g, o, rms, worst = fit(ts, ps)
    lines.append(f"{name}: press = {g:.4f} * target {o:+.1f}   (rms {rms:.1f} px, worst {worst:.1f}; mean off {sum(p - t for t, p in zip(ts, ps)) / len(ts):+.1f})")
    for t in sorted(set(ts)):
        v = [p for tt, p in zip(ts, ps) if tt == t]
        lines.append(f"   target {t:3d} -> press mean {sum(v) / len(v):6.1f}  ({min(v)}..{max(v)}, n {len(v)})")
    if name == "y" and a.bias is not None:
        lines.append(f"   raw y (bias {a.bias:g} px back in): press = {g:.4f} * target {o + a.bias:+.1f}")
print("\n".join(lines))
if a.out:
    with open(a.out, "w") as f:
        f.write(f"# touch_calib {a.serial} {time.strftime('%Y-%m-%d %H:%M')}: pass, cross, target x y, press x y\n")
        for r in rows: f.write(" ".join(map(str, r)) + "\n")
        f.write("\n".join("# " + l for l in lines) + "\n")
