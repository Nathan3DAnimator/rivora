# (2026-11-24, item 7) The executables carry the program's icon: each exe's GLFW_ICON (the icon group the window, the
# taskbar and Explorer show) is read back from the file (LoadLibraryEx as data, the group and its RT_ICON images) and
# compared byte for byte with data/branding/icon.ico (made by tools/make_icon.py), size by size.
#   python tools/check_exe_icon.py <icon.ico> <exe> [<exe>...]      -> "exe icon check: OK ..." or FAIL lines
import ctypes
import struct
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.LoadLibraryExW.restype = wintypes.HMODULE
k32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
k32.FindResourceW.restype = wintypes.HANDLE
k32.FindResourceW.argtypes = [wintypes.HMODULE, wintypes.LPCWSTR, wintypes.LPCWSTR]
k32.LoadResource.restype = wintypes.HANDLE
k32.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
k32.LockResource.restype = ctypes.c_void_p
k32.LockResource.argtypes = [wintypes.HANDLE]
k32.SizeofResource.restype = wintypes.DWORD
k32.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
k32.FreeLibrary.argtypes = [wintypes.HMODULE]

LOAD_AS_DATA = 0x00000002 | 0x00000020  # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE
RT_ICON, RT_GROUP_ICON = 3, 14


def res(h, name, typ):
    r = k32.FindResourceW(h, name, ctypes.c_wchar_p(typ) if isinstance(typ, str) else ctypes.cast(typ, wintypes.LPCWSTR))
    if not r:
        return None
    p = k32.LockResource(k32.LoadResource(h, r))
    return ctypes.string_at(p, k32.SizeofResource(h, r))


def ico_images(path):  # {width: bytes} of a .ico
    d = open(path, "rb").read()
    _, typ, n = struct.unpack_from("<HHH", d, 0)
    out = {}
    for i in range(n):
        w, h, _, _, _, _, size, off = struct.unpack_from("<BBBBHHII", d, 6 + i * 16)
        out[w or 256] = d[off:off + size]
    return out


def exe_images(path):  # {width: bytes} of the exe's GLFW_ICON group
    h = k32.LoadLibraryExW(path, None, LOAD_AS_DATA)
    if not h:
        return None
    try:
        grp = res(h, "GLFW_ICON", RT_GROUP_ICON)
        if not grp:
            return None
        _, _, n = struct.unpack_from("<HHH", grp, 0)
        out = {}
        for i in range(n):
            w, hh, _, _, _, _, size, rid = struct.unpack_from("<BBBBHHIH", grp, 6 + i * 14)
            data = res(h, ctypes.cast(rid, wintypes.LPCWSTR), RT_ICON)
            if data is not None:
                out[w or 256] = data
        return out
    finally:
        k32.FreeLibrary(h)


def main(argv):
    if len(argv) < 3:
        print("usage: check_exe_icon.py <icon.ico> <exe>...")
        return 2
    want = ico_images(argv[1])
    bad = 0
    for exe in argv[2:]:
        got = exe_images(exe)
        name = exe.replace("\\", "/").rsplit("/", 1)[-1]
        if got is None:
            print("FAIL: %s has no GLFW_ICON" % name)
            bad += 1
            continue
        for s, data in sorted(want.items()):
            if got.get(s) != data:
                print("FAIL: %s: the %d px image is not the one of %s" % (name, s, argv[1]))
                bad += 1
        print("%s: %s px" % (name, ", ".join(str(s) for s in sorted(got))))
    if bad:
        return 1
    print("exe icon check: OK (%d executables, %s px each)" % (len(argv) - 2, ", ".join(str(s) for s in sorted(want))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
