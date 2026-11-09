import os, sys, shutil, time, ctypes, threading, queue, subprocess, winreg
from ctypes import wintypes
import tkinter as tk
from tkinter import ttk
import pywintypes
import win32api, win32clipboard, win32con, win32file, win32event

APP_NAME = "Mouse Drive Helper"
LABEL = "MOUSEDRV"       # volume name of the SD card
FORCE_DRIVE = None       # example: "E:\\"  (use if the label does not work)
RECEIVE_DIR = os.path.join(os.path.expanduser("~"), "Downloads", "FromMouse")
DEBUG = sys.stdout is not None
CHUNK = 1024 * 1024      # copy in 1 MB pieces

INSTALL_DIR = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "MouseDrive")
INSTALL_EXE = os.path.join(INSTALL_DIR, "MouseAgent.exe")
MUTEX_NAME = "MouseDriveAgentMutex"
QUIT_EVENT = "MouseDriveAgentQuit"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_NAME = "MouseDriveAgent"

try:                      # sharp text on high-DPI screens
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

def log(*a):
    if DEBUG:
        print(time.strftime("%H:%M:%S"), *a, flush=True)

# ---------------- simple system message box (used before the GUI starts) ----------------
def msgbox(text, yesno=False):
    flags = 0x40000 | 0x10000 | (0x24 if yesno else 0x40)   # topmost, foreground
    r = ctypes.windll.user32.MessageBoxW(0, text, APP_NAME, flags)
    return (r == 6) if yesno else True

# ---------------- install / single-instance helpers ----------------
def agent_running():
    try:
        h = win32event.OpenMutex(0x00100000, False, MUTEX_NAME)   # SYNCHRONIZE
        win32api.CloseHandle(h)
        return True
    except pywintypes.error:
        return False

def stop_running_agent():
    try:
        ev = win32event.OpenEvent(0x0002, False, QUIT_EVENT)       # EVENT_MODIFY_STATE
        win32event.SetEvent(ev)
        win32api.CloseHandle(ev)
    except pywintypes.error:
        pass

def startup_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, RUN_NAME)
            return True
    except OSError:
        return False

def enable_startup(path):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, RUN_NAME, 0, winreg.REG_SZ, f'"{path}"')

def disable_startup():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.DeleteValue(k, RUN_NAME)
    except OSError:
        pass

def launcher_mode():
    # True when running as the .exe from somewhere other than the install folder (e.g. the mouse drive)
    if not getattr(sys, "frozen", False):
        return False
    return os.path.dirname(os.path.abspath(sys.executable)).lower() != INSTALL_DIR.lower()

# ---------------- startup logic ----------------
if "--uninstall" in sys.argv:
    stop_running_agent()
    disable_startup()
    msgbox(f"Mouse Drive Helper stopped and removed from startup.\n\nYou can delete this folder:\n{INSTALL_DIR}")
    sys.exit(0)

if agent_running():
    if msgbox("Mouse Drive Helper is already running.\n\nDo you want to STOP it?", yesno=True):
        stop_running_agent()
    sys.exit(0)

if launcher_mode():
    add_startup = (not startup_enabled()) and msgbox(
        "Install Mouse Drive Helper on this PC?\n\n"
        "It starts automatically with Windows and shows the copy pop-ups.\n"
        "You can remove it any time in Task Manager > Startup apps.", yesno=True)
    try:
        os.makedirs(INSTALL_DIR, exist_ok=True)
        shutil.copy2(sys.executable, INSTALL_EXE)
        if add_startup:
            enable_startup(INSTALL_EXE)
        env = dict(os.environ)
        env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
        subprocess.Popen([INSTALL_EXE], env=env, creationflags=0x00000008 | 0x00000200, close_fds=True)
        msgbox("Mouse Drive Helper is running.\n\nYou can use Safely Eject on the drive as normal.")
    except Exception as e:
        msgbox(f"Install failed:\n{e}")
    sys.exit(0)

# ---------------- from here on: the helper itself ----------------
_mutex = win32event.CreateMutex(None, False, MUTEX_NAME)
_quit = win32event.CreateEvent(None, True, False, QUIT_EVENT)

root = tk.Tk()
root.withdraw()

# ---------------- hotkeys from the mouse buttons ----------------
# Ctrl+Alt+Shift+F13 = quick copy, F14 = pull all, F15 = open drive
hk_queue = queue.Queue()

def hotkey_thread():
    u = ctypes.windll.user32
    mods = 0x0002 | 0x0001 | 0x0004 | 0x4000      # CTRL | ALT | SHIFT | NOREPEAT
    for hid, vk in ((1, 0x7C), (2, 0x7D), (3, 0x7E)):
        ok = u.RegisterHotKey(None, hid, mods, vk)
        log("Hotkey", hid, "registered" if ok else "FAILED (already in use?)")
    msg = wintypes.MSG()
    while u.GetMessageW(ctypes.byref(msg), None, 0, 0) != 0:
        if msg.message == 0x0312:                  # WM_HOTKEY
            hk_queue.put(int(msg.wParam))

threading.Thread(target=hotkey_thread, daemon=True).start()

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
        if dtype in (2, 3):                        # removable or fixed only
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
        if t in (2, 3) and l.upper() == LABEL.upper():
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
    dirs, items = [], []          # items: (source path, relative path, size)
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

def copy_worker(dest, dirs, items, st):
    try:
        for d in dirs:
            os.makedirs(os.path.join(dest, d), exist_ok=True)
        for src, rel, size in items:
            if st["cancel"]:
                break
            dst = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
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
                    os.remove(dst)
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

def copy_with_progress(files, dest, to_mouse=True):
    dirs, items = build_file_list(files)
    total = sum(s for _, _, s in items)
    os.makedirs(dest, exist_ok=True)

    try:
        free = shutil.disk_usage(dest).free
        if total > free:
            return "error", (f"Not enough free space.\n"
                             f"Needed: {fmt_size(total)}\nFree: {fmt_size(free)}")
    except Exception:
        pass

    st = {"copied": 0, "name": "", "cancel": False, "done": False, "error": None}
    th = threading.Thread(target=copy_worker, args=(dest, dirs, items, st), daemon=True)

    win = tk.Toplevel(root)
    win.title("Mouse Drive")
    win.attributes("-topmost", True)
    win.resizable(False, False)
    frame = tk.Frame(win, padx=16, pady=12)
    frame.pack()
    title = f"Copying to Mouse Drive ({dest})" if to_mouse else "Copying from Mouse Drive to PC"
    tk.Label(frame, text=title, font=("Segoe UI", 10, "bold")).pack(anchor="w")
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
    note = "Use Safely Eject before removing." if to_mouse else f"Saved in:\n{RECEIVE_DIR}"
    return "ok", (f"Done! {fmt_size(total)} copied in {fmt_time(elapsed)} "
                  f"({fmt_size(total / elapsed)}/s).\n{note}")

# ---------------- button actions ----------------
def pull_all():
    drive = find_drive()
    if not drive:
        show_popup("Mouse drive not found.\nCheck it is in USB drive mode (green LED).", ask=False, timeout=5)
        return
    skip = {"system volume information", "$recycle.bin", "found.000"}
    try:
        srcs = [os.path.join(drive, n) for n in os.listdir(drive) if n.lower() not in skip]
    except Exception as e:
        show_popup(f"Cannot read the mouse drive:\n{e}", ask=False, timeout=5)
        return
    if not srcs:
        show_popup("No files on the mouse drive.", ask=False, timeout=4)
        return
    status, msg = copy_with_progress(srcs, RECEIVE_DIR, to_mouse=False)
    log("Pull result:", status, "-", msg.replace("\n", " "))
    show_popup(msg, ask=False, timeout=6)
    if status == "ok":
        try:
            os.startfile(RECEIVE_DIR)
        except Exception:
            pass

def open_drive():
    drive = find_drive()
    if drive:
        os.startfile(drive)
    else:
        show_popup("Mouse drive not found.", ask=False, timeout=4)

# ---------------- main ----------------
print("Mouse Drive agent running... (Ctrl+C in THIS window to stop)")
print("Drives found now:")
for d, t, l in list_drives():
    print("   ", d, "type", t, "label:", repr(l))
print("Looking for label:", LABEL)
print("Mouse buttons: Button 4 = quick copy | Button 5 = pull all (hold = open drive)\n")

last_seq = -1
last_sig = None          # file list we already asked about
armed_until = 0          # set when Button 4 is pressed (quick copy, no question)
last_drive = None
last_drive_check = 0

while True:
    try:
        root.update()

        # ---- stop request from a second launch ----
        if win32event.WaitForSingleObject(_quit, 0) == win32event.WAIT_OBJECT_0:
            break

        # ---- drive plugged in message ----
        if time.time() - last_drive_check > 2:
            last_drive_check = time.time()
            d = find_drive()
            if d and d != last_drive:
                show_popup(f"Mouse Drive connected ({d})\n\n"
                           "Copy: select files, press Button 4 (or Ctrl+C)\n"
                           "Get files: press Button 5", ask=False, timeout=4)
            last_drive = d

        # ---- hotkeys from the mouse buttons ----
        while not hk_queue.empty():
            hid = hk_queue.get()
            if hid == 1:
                armed_until = time.time() + 3
                log("Quick copy armed")
            elif hid == 2:
                log("Pull all")
                pull_all()
                last_seq = win32clipboard.GetClipboardSequenceNumber()
            elif hid == 3:
                log("Open drive")
                open_drive()

        # ---- clipboard watcher ----
        seq = win32clipboard.GetClipboardSequenceNumber()
        if seq != last_seq:
            last_seq = seq
            files = copied_files()
            if files:
                auto = time.time() < armed_until
                if auto:
                    armed_until = 0
                sig = tuple(sorted(f.lower() for f in files))
                if sig == last_sig and not auto:
                    log("Same files as before (clipboard refreshed) - ignoring")
                else:
                    log("Copied files detected:", files)
                    drive = find_drive()
                    if not drive:
                        log("Mouse drive NOT found. Current drives:")
                        for d, t, l in list_drives():
                            log("   ", d, "type", t, "label:", repr(l))
                        if auto:
                            show_popup("Mouse drive not found.\nCheck it is in USB drive mode (green LED).",
                                       ask=False, timeout=5)
                    else:
                        todo = [f for f in files if not f.upper().startswith(drive.upper())]
                        if todo:
                            last_sig = sig
                            names = "\n".join(os.path.basename(f) for f in todo[:5])
                            if len(todo) > 5:
                                names += f"\n... and {len(todo) - 5} more"
                            if auto or show_popup(f"Copy to Mouse Drive ({drive})?\n\n{names}"):
                                status, msg = copy_with_progress(todo, drive)
                                log("Result:", status, "-", msg.replace("\n", " "))
                                show_popup(msg, ask=False, timeout=6 if status != "ok" else 5)
                            last_seq = win32clipboard.GetClipboardSequenceNumber()
    except KeyboardInterrupt:
        break
    except Exception as e:
        log("Error:", e)
    time.sleep(0.15)