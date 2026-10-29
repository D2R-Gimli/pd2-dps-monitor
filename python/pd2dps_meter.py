"""PD2 DPS Meter - Python front end.

The damage hook itself must run inside the game, so PD2DpsHook.dll (C++) is still needed.
This script can
  * inject the DLL into Game.exe (like PD2Injector.exe, works from 32 and 64 bit Python), and
  * show the live statistics in its own normal application window. That window is a
    separate program, so OBS can capture it with "Window Capture" -> [python.exe]: PD2 DPS Meter (Python).
    Use a "Color Key" filter with the background colour to make the background transparent.

Usage examples
    python pd2dps_meter.py                     # show the meter (DLL already injected)
    python pd2dps_meter.py --inject            # inject ..\\bin\\Release\\PD2DpsHook.dll, then show
    python pd2dps_meter.py --inject --dll <folder>\\PD2DpsHook.dll
    python pd2dps_meter.py --inject-only       # only inject, no window

Settings: right-click the window to pick the background, label and value colours, toggle
"always on top" etc. Everything (also the window position) is saved in pd2dps_meter.json next
to this script and restored on the next start. Command line options override the saved values
and are saved as well.

Tip: set Mode=hidden in PD2DpsMeter.ini if you only want this window and no in-game overlay.
Requires: Windows, Python 3.13 (standard library only; tkinter is part of the python.org installer).
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys
import time

# ----------------------------------------------------------------------------------------
#  Shared memory layout - must match PD2DpsHook/SharedState.h
# ----------------------------------------------------------------------------------------
SHARED_MAGIC = 0x53504432
SHARED_VERSION = 1
SHARED_FORMAT = '<4I12d4I'
SHARED_SIZE = struct.calcsize(SHARED_FORMAT)      # 128 bytes
SHARED_FIELDS = ('magic', 'version', 'sequence', 'command_reset',
                 'dps1', 'dps5', 'dps10', 'dps20', 'session_dps', 'session_seconds', 'total_damage',
                 'last_hit', 'max_hit', 'hits_per_second', 'last_kill_seconds', 'target_hp',
                 'hits', 'kills', 'target_class_id', 'session_number')
OFFSET_SEQUENCE = 8
OFFSET_RESET = 12

DLL_NAME = 'PD2DpsHook.dll'
HERE = os.path.dirname(os.path.abspath(__file__))

# ----------------------------------------------------------------------------------------
#  Win32 API
# ----------------------------------------------------------------------------------------
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
PROCESS_ALL_FOR_INJECT = 0x0002 | 0x0400 | 0x0008 | 0x0020 | 0x0010   # create thread, query, vm op/write/read
MEM_COMMIT_RESERVE = 0x3000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0xF001F
INVALID_HANDLE = ctypes.c_void_p(-1).value


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD),
                ('szExeFile', wt.WCHAR * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('th32ModuleID', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('GlblcntUsage', wt.DWORD), ('ProccntUsage', wt.DWORD), ('modBaseAddr', ctypes.c_void_p),
                ('modBaseSize', wt.DWORD), ('hModule', wt.HMODULE), ('szModule', wt.WCHAR * 256),
                ('szExePath', wt.WCHAR * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.Process32FirstW.argtypes = k32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Module32FirstW.argtypes = k32.Module32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                                   wt.DWORD, ctypes.c_void_p]
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]


def find_processes(name='Game.exe'):
    pids = []
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE:
        return pids
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == name.lower():
            pids.append(pe.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return pids


def list_modules(pid):
    mods = {}
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == INVALID_HANDLE:
        return mods
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(me)
    ok = k32.Module32FirstW(snap, ctypes.byref(me))
    while ok:
        mods.setdefault(me.szModule.lower(), (me.modBaseAddr or 0, me.szExePath))
        ok = k32.Module32NextW(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return mods


# ----------------------------------------------------------------------------------------
#  Injection (LoadLibraryW in the 32 bit game, address resolved from the game's own kernel32)
# ----------------------------------------------------------------------------------------
def pe_export_rva(path, func):
    """RVA of an exported function in a PE file, or ('forward', 'DLL.Func')."""
    with open(path, 'rb') as f:
        data = f.read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (96 if magic == 0x10B else 112)
    exp_rva, exp_size = struct.unpack_from('<II', data, dd)
    sections = []
    sec = opt + struct.unpack_from('<H', data, pe + 20)[0]
    for i in range(nsec):
        vsize, va, rsize, raw = struct.unpack_from('<IIII', data, sec + 40 * i + 8)
        sections.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError('rva outside sections')

    e = off(exp_rva)
    nfuncs, nnames, afuncs, anames, aords = struct.unpack_from('<IIIII', data, e + 20)
    for i in range(nnames):
        name_rva = struct.unpack_from('<I', data, off(anames) + 4 * i)[0]
        n = data[off(name_rva):data.index(b'\0', off(name_rva))].decode()
        if n == func:
            ordinal = struct.unpack_from('<H', data, off(aords) + 2 * i)[0]
            rva = struct.unpack_from('<I', data, off(afuncs) + 4 * ordinal)[0]
            if exp_rva <= rva < exp_rva + exp_size:   # forwarder string
                return ('forward', data[off(rva):data.index(b'\0', off(rva))].decode())
            return rva
    raise KeyError(func)


def remote_proc_address(pid, module, func):
    mods = list_modules(pid)
    base, path = mods.get(module.lower(), (None, None))
    if not base:
        raise RuntimeError(f'{module} not loaded in process {pid}')
    rva = pe_export_rva(path, func)
    if isinstance(rva, tuple):
        dll, fn = rva[1].split('.', 1)
        return remote_proc_address(pid, dll + '.dll', fn)
    return base + rva


def inject(pid, dll_path):
    if DLL_NAME.lower() in list_modules(pid):
        return 'already running'
    load_library = remote_proc_address(pid, 'kernel32.dll', 'LoadLibraryW')
    proc = k32.OpenProcess(PROCESS_ALL_FOR_INJECT, False, pid)
    if not proc:
        raise OSError(ctypes.get_last_error(), 'cannot open the game process (run as administrator?)')
    try:
        buf = ctypes.create_unicode_buffer(dll_path)
        size = ctypes.sizeof(buf)
        remote = k32.VirtualAllocEx(proc, None, size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not remote:
            raise OSError(ctypes.get_last_error(), 'VirtualAllocEx failed')
        try:
            if not k32.WriteProcessMemory(proc, remote, buf, size, None):
                raise OSError(ctypes.get_last_error(), 'WriteProcessMemory failed')
            thread = k32.CreateRemoteThread(proc, None, 0, load_library, remote, 0, None)
            if not thread:
                raise OSError(ctypes.get_last_error(), 'CreateRemoteThread failed')
            k32.WaitForSingleObject(thread, 15000)
            code = wt.DWORD()
            k32.GetExitCodeThread(thread, ctypes.byref(code))
            k32.CloseHandle(thread)
            if code.value == 0:
                raise RuntimeError('LoadLibraryW failed inside the game (wrong path, or blocked?)')
        finally:
            k32.VirtualFreeEx(proc, remote, 0, MEM_RELEASE)
    finally:
        k32.CloseHandle(proc)
    return 'injected'


# ----------------------------------------------------------------------------------------
#  Shared memory reader
# ----------------------------------------------------------------------------------------
class SharedStats:
    def __init__(self, pid):
        self.pid = pid
        self.handle = k32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, f'Local\\PD2DpsMeter_{pid}')
        self.view = k32.MapViewOfFile(self.handle, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_SIZE) if self.handle else None

    @property
    def ok(self):
        return bool(self.view)

    def read(self):
        for _ in range(10):   # seqlock: retry while the DLL is writing
            seq1 = ctypes.c_uint32.from_address(self.view + OFFSET_SEQUENCE).value
            raw = ctypes.string_at(self.view, SHARED_SIZE)
            seq2 = ctypes.c_uint32.from_address(self.view + OFFSET_SEQUENCE).value
            if seq1 == seq2 and seq1 % 2 == 0:
                d = dict(zip(SHARED_FIELDS, struct.unpack(SHARED_FORMAT, raw)))
                if d['magic'] == SHARED_MAGIC and d['version'] == SHARED_VERSION:
                    return d
                return None
            time.sleep(0.001)
        return None

    def request_reset(self):
        ctypes.c_uint32.from_address(self.view + OFFSET_RESET).value = 1

    def close(self):
        if self.view:
            k32.UnmapViewOfFile(self.view)
        if self.handle:
            k32.CloseHandle(self.handle)
        self.view = self.handle = None


# ----------------------------------------------------------------------------------------
#  Meter window (tkinter)
# ----------------------------------------------------------------------------------------
def fmt_num(v):
    return f'{max(0.0, v):,.0f}'


def fmt_time(s):
    return f'{s:.1f} s' if s < 60 else f'{int(s // 60)}:{s % 60:04.1f}'


# ----------------------------------------------------------------------------------------
#  Persistent settings (pd2dps_meter.json next to this script)
# ----------------------------------------------------------------------------------------
SETTINGS_FILE = os.path.join(HERE, 'pd2dps_meter.json')
DEFAULT_SETTINGS = {
    'bg': '#00ff00',            # background, use it as OBS colour key
    'fg': '#ffffff',            # values
    'title_color': '#c7b377',   # title + labels
    'font': 'Exocet',
    'font_size': 20,
    'scale': 1.0,
    'topmost': False,
    'logo': 'logo.png',         # next to this script, the DLL or in PD2DpsHook/; '' = no logo
    'logo_size': 50,
    'x': None,                  # window position (None = let Windows decide)
    'y': None,
}


def load_settings():
    settings = dict(DEFAULT_SETTINGS)
    try:
        with open(SETTINGS_FILE, encoding='utf-8') as f:
            saved = json.load(f)
        settings.update({k: v for k, v in saved.items() if k in DEFAULT_SETTINGS})
    except (OSError, ValueError):
        pass   # first start or unreadable file -> defaults
    return settings


def save_settings(settings):
    try:
        with open(SETTINGS_FILE, 'w', encoding='utf-8') as f:
            json.dump(settings, f, indent=2)
    except OSError as e:
        print(f'could not save settings: {e}')


def find_logo(name, dll_path):
    if not name:
        return None
    if os.path.isabs(name):
        return name if os.path.isfile(name) else None
    for folder in (HERE, os.path.dirname(dll_path), os.path.join(HERE, '..', 'PD2DpsHook')):
        p = os.path.join(folder, name)
        if os.path.isfile(p):
            return p
    return None


def run_window(args, settings):
    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import colorchooser

    cfg = settings
    root = tk.Tk()
    root.title('PD2 DPS Meter (Python)')
    root.attributes('-topmost', bool(cfg['topmost']))
    root.resizable(False, False)
    if cfg['x'] is not None and cfg['y'] is not None:
        root.geometry(f"+{int(cfg['x'])}+{int(cfg['y'])}")

    families = set(tkfont.families())
    face = cfg['font'] if cfg['font'] in families else 'Tahoma'
    size = round(cfg['font_size'] * cfg['scale'])
    f_title = tkfont.Font(family=face, size=-round(size * 1.2), weight='bold')
    f_text = tkfont.Font(family=face, size=-size)
    f_btn = tkfont.Font(family=face, size=-round(size * 0.7), weight='bold')

    state = {'pid': args.pid, 'shared': None}
    labels = ['DPS 1s', 'DPS 5s', 'DPS 20s', 'Average', 'Time', 'Total', 'Hits', 'Last / Max', 'Kills']
    rows = []

    # logo (PNG/GIF; tkinter can only shrink by whole factors)
    logo_img = None
    logo_path = find_logo(cfg['logo'], args.dll)
    if logo_path:
        try:
            logo_img = tk.PhotoImage(file=logo_path)
            want = max(16, round(cfg['logo_size'] * cfg['scale']))
            logo_img = logo_img.subsample(max(1, round(logo_img.width() / want)))
        except tk.TclError:
            logo_img = None
    # left column: logo + RESET button in their own frame, so they never stretch the text rows
    side = tk.Frame(root, bd=0)
    side.grid(row=0, column=0, rowspan=len(labels) + 2, sticky='n', padx=(8, 4), pady=8)
    logo = tk.Label(side, image=logo_img, bd=0) if logo_img else None
    if logo:
        logo.pack(anchor='n')
    col = 1

    title = tk.Label(root, text='PD2 DPS', font=f_title, anchor='w')
    title.grid(row=0, column=col, columnspan=2, sticky='w', padx=8, pady=(6, 0))
    for i, name in enumerate(labels):
        lab = tk.Label(root, text=name, font=f_text, anchor='w')
        val = tk.Label(root, text='-', font=f_text, anchor='w', width=20)
        lab.grid(row=i + 1, column=col, sticky='w', padx=(8, 12))
        val.grid(row=i + 1, column=col + 1, sticky='w', padx=(0, 8))
        rows.append((lab, val))
    status = tk.Label(root, text='', font=f_btn, anchor='w')
    status.grid(row=len(labels) + 1, column=col + 1, sticky='w')

    def reset():
        if state['shared'] and state['shared'].ok:
            state['shared'].request_reset()

    button = tk.Button(side, text='RESET', font=f_btn, command=reset, relief='groove', bd=2)
    button.pack(anchor='n', pady=(8, 0))

    def apply_colors():
        bg = cfg['bg']
        root.configure(bg=bg)
        for w in [side, title, status] + ([logo] if logo else []) + [x for pair in rows for x in pair]:
            w.configure(bg=bg)
        title.configure(fg=cfg['title_color'])
        for lab, val in rows:
            lab.configure(fg=cfg['title_color'])
            val.configure(fg=cfg['fg'])
        status.configure(fg=cfg['fg'])
        button.configure(bg=bg, activebackground=bg, fg=cfg['fg'], activeforeground=cfg['fg'])

    def pick(key, caption):
        c = colorchooser.askcolor(cfg[key], title=caption, parent=root)
        if c and c[1]:
            cfg[key] = c[1]
            apply_colors()
            save_settings(cfg)

    topmost = tk.BooleanVar(value=bool(cfg['topmost']))

    def toggle_top():
        cfg['topmost'] = topmost.get()
        root.attributes('-topmost', cfg['topmost'])
        save_settings(cfg)

    def default_colors():
        for k in ('bg', 'fg', 'title_color'):
            cfg[k] = DEFAULT_SETTINGS[k]
        apply_colors()
        save_settings(cfg)

    def remember_position():
        cfg['x'], cfg['y'] = root.winfo_x(), root.winfo_y()

    def on_close():
        remember_position()   # also catches moves via the title bar
        save_settings(cfg)
        root.destroy()

    menu = tk.Menu(root, tearoff=0)
    menu.add_command(label='Reset statistics', command=reset)
    menu.add_checkbutton(label='Always on top', variable=topmost, command=toggle_top)
    menu.add_separator()
    menu.add_command(label='Background colour... (OBS colour key)', command=lambda: pick('bg', 'Background colour'))
    menu.add_command(label='Title / label colour...', command=lambda: pick('title_color', 'Title / label colour'))
    menu.add_command(label='Value colour...', command=lambda: pick('fg', 'Value colour'))
    menu.add_command(label='Default colours', command=default_colors)
    menu.add_separator()
    menu.add_command(label='Quit', command=on_close)
    root.bind('<Button-3>', lambda e: menu.tk_popup(e.x_root, e.y_root))
    root.protocol('WM_DELETE_WINDOW', on_close)

    # drag the window by its content as well; the position is saved when released
    def start_move(e):
        state['drag'] = (e.x_root - root.winfo_x(), e.y_root - root.winfo_y())

    def do_move(e):
        dx, dy = state.get('drag', (0, 0))
        root.geometry(f'+{e.x_root - dx}+{e.y_root - dy}')

    def end_move(_e):
        remember_position()
        save_settings(cfg)

    root.bind('<Button-1>', start_move)
    root.bind('<B1-Motion>', do_move)
    root.bind('<ButtonRelease-1>', end_move)
    apply_colors()

    def connect():
        if state['shared'] and state['shared'].ok:
            return True
        pids = [state['pid']] if state['pid'] else find_processes(args.process)
        for pid in pids:
            sh = SharedStats(pid)
            if sh.ok:
                state['shared'] = sh
                return True
            sh.close()
        return False

    def refresh():
        if not connect():
            status.configure(text='waiting for the game / meter DLL...')
        else:
            s = state['shared'].read()
            if s is None:
                status.configure(text='')
            else:
                if s['session_number'] > 1:
                    title.configure(text=f"PD2 DPS  #{s['session_number']}")
                values = [fmt_num(s['dps1']), fmt_num(s['dps5']), fmt_num(s['dps20']), fmt_num(s['session_dps']),
                          fmt_time(s['session_seconds']), fmt_num(s['total_damage']),
                          f"{s['hits']}  ({s['hits_per_second']:.1f}/s)",
                          f"{fmt_num(s['last_hit'])} / {fmt_num(s['max_hit'])}",
                          f"{s['kills']}  (TTK {fmt_time(s['last_kill_seconds'])})" if s['kills'] else '0']
                for (_, val), text in zip(rows, values):
                    val.configure(text=text)
                status.configure(text='')
            # game closed? -> reconnect later
            if not find_processes(args.process):
                state['shared'].close()
                state['shared'] = None
        root.after(100, refresh)

    refresh()
    root.mainloop()


# ----------------------------------------------------------------------------------------
def default_dll():
    for p in (os.path.join(HERE, DLL_NAME), os.path.join(HERE, '..', 'bin', 'Release', DLL_NAME)):
        if os.path.isfile(p):
            return os.path.abspath(p)
    return os.path.abspath(os.path.join(HERE, DLL_NAME))


def main():
    ap = argparse.ArgumentParser(description='PD2 DPS Meter (Python front end)')
    ap.add_argument('--inject', action='store_true', help='inject the DLL into the game first')
    ap.add_argument('--inject-only', action='store_true', help='inject and exit')
    ap.add_argument('--dll', default=default_dll(), help='path of PD2DpsHook.dll')
    ap.add_argument('--process', default='Game.exe')
    ap.add_argument('--pid', type=int, default=0, help='game process id (default: first Game.exe)')
    ap.add_argument('--wait', type=int, default=120, help='seconds to wait for the game when injecting')
    # look options: default None = use the saved setting; options given here are saved
    ap.add_argument('--bg', help='background colour, e.g. #00ff00 (OBS colour key)')
    ap.add_argument('--fg', help='value colour')
    ap.add_argument('--title-color', help='title/label colour')
    ap.add_argument('--font')
    ap.add_argument('--font-size', type=int)
    ap.add_argument('--scale', type=float)
    ap.add_argument('--logo', help="logo image (PNG/GIF), '' = none")
    ap.add_argument('--topmost', action=argparse.BooleanOptionalAction, help='keep the window above other windows')
    args = ap.parse_args()

    settings = load_settings()
    for key in ('bg', 'fg', 'title_color', 'font', 'font_size', 'scale', 'logo', 'topmost'):
        value = getattr(args, key)
        if value is not None:
            settings[key] = value

    if args.inject or args.inject_only:
        if not os.path.isfile(args.dll):
            sys.exit(f'DLL not found: {args.dll}')
        pids = [args.pid] if args.pid else find_processes(args.process)
        deadline = time.time() + args.wait
        if not pids:
            print(f'waiting for {args.process} ...')
        while not pids and time.time() < deadline:
            time.sleep(0.5)
            pids = find_processes(args.process)
            if pids:
                time.sleep(3)   # let the game load its DLLs
        if not pids:
            sys.exit(f'{args.process} is not running')
        for pid in pids:
            try:
                print(f'{args.process} (PID {pid}): {inject(pid, os.path.abspath(args.dll))}')
            except Exception as e:  # noqa: BLE001 - report and continue with other instances
                print(f'{args.process} (PID {pid}): ERROR {e}')
        if args.inject_only:
            return
        time.sleep(1)   # give the DLL a moment to create its shared memory
    save_settings(settings)
    run_window(args, settings)


if __name__ == '__main__':
    main()
