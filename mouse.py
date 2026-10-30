import os, shutil, time, ctypes, threading
import tkinter as tk
from tkinter import ttk
import win32api, win32clipboard, win32con, win32file

LABEL = "MOUSEDRV"       # volume name of the SD card
FORCE_DRIVE = None       # example: "E:\\"  (use if the label does not work)
DEBUG = True
CHUNK = 1024 * 1024      # copy in 1 MB pieces

try:                      # sharp text on high-DPI screens
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

root = tk.Tk()
root.withdraw()

def log(*a):
    if DEBUG:
        print(time.strftime("%H:%M:%S"), *a, flush=True)

# ---------------- drive / clipboard ----------------
def list_drives():
    drives = []
    for d in win32api.GetLogicalDriveStrings().split("\x00"):
        if not d:
            continue
        try:
            dtype = win32file.GetDriveType(d)
        except Exception:
            dtype = 0
        label = ""
        try:
            label = win32api.GetVolumeInformation(d)[0]
        except Exception:
            pass
        drives.append((d, dtype, label))
    return drives

def find_drive():
    if FORCE_DRIVE and os.path.exists(FORCE_DRIVE):
        return FORCE_DRIVE
    for d, t, l in list_drives():
        if l.upper() == LABEL.upper():
            return d
    return None

def copied_files():
    for _ in range(5):
        try:
            win32clipboard.OpenClipboard()
            try:
                if win32clipboard.IsClipboardFormatAvailable(win32con.CF_HDROP):
                    return list(win32clipboard.GetClipboardData(win32con.CF_HDROP))
                return []
            finally:
                win32clipboard.CloseClipboard()
        except Exception:
            time.sleep(0.1)
    return []

# ---------------- window helpers ----------------
def bring_front(win):
    # a tiny Alt key press lets this window take focus from the active app
    ctypes.windll.user32.keybd_event(0x12, 0, 0, 0)
    ctypes.windll.user32.keybd_event(0x12, 0, 2, 0)
    win.lift()
    win.attributes("-topmost", True)
    win.focus_force()

def place_near_cursor(win):
    win.update_idletasks()
    w, h = win.winfo_width(), win.winfo_height()
    sw, sh = win.winfo_screenwidth(), win.winfo_screenheight()
    px, py = win.winfo_pointerxy()
    x = min(max(px + 20, 0), sw - w - 10)
    y = min(max(py + 20, 0), sh - h - 60)
    win.geometry(f"+{x}+{y}")

def show_popup(text, ask=True, timeout=20):
    result = {"yes": False}
    win = tk.Toplevel(root)
    win.title("Mouse Drive")
    win.attributes("-topmost", True)
    win.resizable(False, False)

    frame = tk.Frame(win, padx=16, pady=12)
    frame.pack()
    tk.Label(frame, text=text, justify="left", font=("Segoe UI", 10)).pack(anchor="w")

    def close(v):
        result["yes"] = v
        if win.winfo_exists():
            win.destroy()

    if ask:
        bf = tk.Frame(frame)
        bf.pack(pady=(12, 0), anchor="e")
        tk.Button(bf, text="Copy to Mouse", width=14, command=lambda: close(True)).pack(side="left", padx=4)
        tk.Button(bf, text="No", width=8, command=lambda: close(False)).pack(side="left")
        win.bind("<Return>", lambda e: close(True))
        win.bind("<Escape>", lambda e: close(False))
    win.protocol("WM_DELETE_WINDOW", lambda: close(False))

    place_near_cursor(win)
    bring_front(win)
    win.after(int(timeout * 1000), lambda: close(False))
    win.wait_window()
    return result["yes"]

# ---------------- copy with progress ----------------
def fmt_size(n):
    if n >= 1024 ** 3:
        return f"{n / 1024 ** 3:.2f} GB"
    if n >= 1024 ** 2:
        return f"{n / 1024 ** 2:.1f} MB"
    return f"{n / 1024:.0f} KB"

def fmt_time(s):
    s = int(s)
    return f"{s // 60}m {s % 60:02d}s" if s >= 60 else f"{s}s"

def build_file_list(files):
    dirs, items = [], []          # items: (source path, path on drive, size)
    for f in files:
        base = os.path.basename(f.rstrip("\\/"))
        if os.path.isdir(f):
            for root_dir, _, fnames in os.walk(f):
                rel = os.path.relpath(root_dir, f)
                rel_base = base if rel == "." else os.path.join(base, rel)
                dirs.append(rel_base)
                for fn in fnames:
                    src = os.path.join(root_dir, fn)
                    try:
                        size = os.path.getsize(src)
                    except OSError:
                        size = 0
                    items.append((src, os.path.join(rel_base, fn), size))
        else:
            items.append((f, base, os.path.getsize(f)))
    return dirs, items

def copy_worker(drive, dirs, items, st):
    try:
        for d in dirs:
            os.makedirs(os.path.join(drive, d), exist_ok=True)
        for src, rel, size in items:
            if st["cancel"]:
                break
            dst = os.path.join(drive, rel)
            st["name"] = os.path.basename(src)
            with open(src, "rb") as fi, open(dst, "wb") as fo:
                while not st["cancel"]:
                    buf = fi.read(CHUNK)
                    if not buf:
                        break
                    fo.write(buf)
                    st["copied"] += len(buf)
                if not st["cancel"]:
                    fo.flush()
                    os.fsync(fo.fileno())
            if st["cancel"]:
                try:
                    os.remove(dst)            # remove the half-copied file
                except OSError:
                    pass
                break
            try:
                shutil.copystat(src, dst)
            except OSError:
                pass
    except Exception as e:
        st["error"] = str(e)
    st["done"] = True

def copy_with_progress(files, drive):
    dirs, items = build_file_list(files)
    total = sum(s for _, _, s in items)

    try:
        free = shutil.disk_usage(drive).free
        if total > free:
            return "error", (f"Not enough space on the mouse drive.\n"
                             f"Needed: {fmt_size(total)}\nFree: {fmt_size(free)}")
    except Exception:
        pass

    st = {"copied": 0, "name": "", "cancel": False, "done": False, "error": None}
    th = threading.Thread(target=copy_worker, args=(drive, dirs, items, st), daemon=True)

    win = tk.Toplevel(root)
    win.title("Mouse Drive")
    win.attributes("-topmost", True)
    win.resizable(False, False)
    frame = tk.Frame(win, padx=16, pady=12)
    frame.pack()
    tk.Label(frame, text=f"Copying to Mouse Drive ({drive})",
             font=("Segoe UI", 10, "bold")).pack(anchor="w")
    name_lbl = tk.Label(frame, text="Starting...", font=("Segoe UI", 9), anchor="w", width=48)
    name_lbl.pack(anchor="w", pady=(6, 4))
    bar = ttk.Progressbar(frame, length=380, maximum=100, mode="determinate")
    bar.pack()
    info_lbl = tk.Label(frame, text="", font=("Segoe UI", 9), anchor="w", width=48)
    info_lbl.pack(anchor="w", pady=(4, 0))

    def cancel():
        st["cancel"] = True
        cancel_btn.config(state="disabled", text="Cancelling...")
    cancel_btn = tk.Button(frame, text="Cancel", width=10, command=cancel)
    cancel_btn.pack(anchor="e", pady=(10, 0))
    win.protocol("WM_DELETE_WINDOW", cancel)

    place_near_cursor(win)
    bring_front(win)

    start = time.time()
    th.start()
    while not st["done"]:
        copied = st["copied"]
        elapsed = max(time.time() - start, 0.001)
        speed = copied / elapsed
        pct = (copied * 100.0 / total) if total else 0
        bar["value"] = pct
        name_lbl.config(text=st["name"][:60])
        eta = ((total - copied) / speed) if speed > 0 else 0
        info_lbl.config(text=f"{pct:.0f}%   {fmt_size(copied)} of {fmt_size(total)}   "
                             f"{fmt_size(speed)}/s   {fmt_time(eta)} left")
        win.update()
        time.sleep(0.05)
    th.join()
    elapsed = max(time.time() - start, 0.001)
    win.destroy()

    if st["error"]:
        return "error", f"Copy failed:\n{st['error']}"
    if st["cancel"]:
        return "cancelled", "Copy cancelled. The half-copied file was removed."
    return "ok", (f"Done! {fmt_size(total)} copied in {fmt_time(elapsed)} "
                  f"({fmt_size(total / elapsed)}/s).\nUse Safely Eject before removing.")

# ---------------- main ----------------
print("Mouse Drive agent running... (Ctrl+C in THIS window to stop)")
print("Drives found now:")
for d, t, l in list_drives():
    print("   ", d, "type", t, "label:", repr(l))
print("Looking for label:", LABEL)
print("Now go to File Explorer, select a file and press Ctrl+C there.\n")

last_seq = -1
last_sig = None          # the file list we already asked about

while True:
    try:
        root.update()
        seq = win32clipboard.GetClipboardSequenceNumber()
        if seq != last_seq:
            last_seq = seq
            files = copied_files()
            if files:
                sig = tuple(sorted(f.lower() for f in files))
                if sig == last_sig:
                    log("Same files as before (clipboard refreshed) - ignoring")
                else:
                    log("Copied files detected:", files)
                    drive = find_drive()
                    if not drive:
                        log("Mouse drive NOT found. Current drives:")
                        for d, t, l in list_drives():
                            log("   ", d, "type", t, "label:", repr(l))
                    else:
                        todo = [f for f in files if not f.upper().startswith(drive.upper())]
                        if todo:
                            last_sig = sig
                            names = "\n".join(os.path.basename(f) for f in todo[:5])
                            if len(todo) > 5:
                                names += f"\n... and {len(todo) - 5} more"
                            if show_popup(f"Copy to Mouse Drive ({drive})?\n\n{names}"):
                                status, msg = copy_with_progress(todo, drive)
                                log("Result:", status, "-", msg.replace("\n", " "))
                                show_popup(msg, ask=False, timeout=6 if status != "ok" else 5)
                            last_seq = win32clipboard.GetClipboardSequenceNumber()
    except KeyboardInterrupt:
        break
    except Exception as e:
        log("Error:", e)
    time.sleep(0.3)