"""The ctypes module on top of libffi: calls into the C library and the math library, structures, arrays, pointers, callbacks that C calls back (qsort with a Python comparison), strings and
buffers, errno, and the interpreter's own C API."""
import ctypes
import ctypes.util
import math
import os


def show(label, value=""):
    print(f"{label}: {value}")


libc = ctypes.CDLL("libc.so.6", use_errno=True)
libm = ctypes.CDLL("libm.so.6")
show("loaded", (libc._name, libm._name))

print("--- calling C functions")
libc.strlen.argtypes = [ctypes.c_char_p]
libc.strlen.restype = ctypes.c_size_t
show("strlen", libc.strlen(b"hello, world"))
libc.abs.argtypes = [ctypes.c_int]
show("abs", (libc.abs(-42), libc.abs(7)))
libc.toupper.argtypes = [ctypes.c_int]
show("toupper", chr(libc.toupper(ord("q"))))
libc.atoi.argtypes = [ctypes.c_char_p]
show("atoi", libc.atoi(b"12345abc"))
libc.strtol.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]
libc.strtol.restype = ctypes.c_long
end = ctypes.c_char_p()
show("strtol with an end pointer", (libc.strtol(b"ff rest", ctypes.byref(end), 16), end.value))
libm.cos.argtypes = [ctypes.c_double]
libm.cos.restype = ctypes.c_double
libm.pow.argtypes = [ctypes.c_double, ctypes.c_double]
libm.pow.restype = ctypes.c_double
show("libm", (libm.cos(0.0), libm.pow(2.0, 10.0), libm.cos(math.pi) == -1.0))
libm.sqrt.argtypes = [ctypes.c_double]
libm.sqrt.restype = ctypes.c_double
show("sqrt", libm.sqrt(2.0) == math.sqrt(2.0))
libm.fabsf.argtypes = [ctypes.c_float]
libm.fabsf.restype = ctypes.c_float
show("float", libm.fabsf(-1.5))
libc.getpid.restype = ctypes.c_int
show("getpid", libc.getpid() == os.getpid())
buf = ctypes.create_string_buffer(64)
libc.sprintf(buf, b"%d-%s-%.2f", 42, b"text", ctypes.c_double(3.14159))
show("sprintf (variadic)", buf.value)
libc.getenv.restype = ctypes.c_char_p
show("getenv", (libc.getenv(b"TZ"), libc.getenv(b"NO_SUCH_VARIABLE")))
libc.setenv(b"FFI_VARIABLE", b"set through ctypes", 1)
show("setenv", libc.getenv(b"FFI_VARIABLE"))
ctypes.set_errno(0)
libc.open(b"no-such-file-here", 0)
show("errno", (ctypes.get_errno(), os.strerror(ctypes.get_errno())))
libc.write.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
libc.write(1, b"written by write(2) through ctypes\n", 35)

print("--- types, structures, arrays")
show("sizes", (ctypes.sizeof(ctypes.c_int), ctypes.sizeof(ctypes.c_long), ctypes.sizeof(ctypes.c_void_p), ctypes.sizeof(ctypes.c_double), ctypes.sizeof(ctypes.c_char * 7)))
i = ctypes.c_int(2 ** 31 - 1)
i.value += 1
show("c_int wraps", i.value)
show("c_ubyte", ctypes.c_ubyte(300).value)
show("c_char_p", (ctypes.c_char_p(b"text").value, ctypes.c_wchar_p("wide").value))


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int)]


class Rect(ctypes.Structure):
    _fields_ = [("a", Point), ("b", Point), ("tag", ctypes.c_char * 8), ("weight", ctypes.c_double)]


r = Rect(Point(1, 2), Point(3, 4), b"label", 2.5)
show("structure", (r.a.x, r.b.y, r.tag, r.weight, ctypes.sizeof(r), Rect.weight.offset, Rect.tag.offset))
show("bytes of a structure", bytes(Point(1, 2)))
p = Point.from_buffer_copy(b"\x05\x00\x00\x00\x06\x00\x00\x00")
show("from_buffer_copy", (p.x, p.y))
arr = (ctypes.c_int * 5)(5, 4, 3, 2, 1)
show("array", (list(arr), len(arr), arr[2], sum(arr)))
show("array slice", arr[1:4])
pt = ctypes.pointer(ctypes.c_int(10))
pt.contents.value = 20
show("pointer", (pt[0], pt.contents.value))
ip = ctypes.cast(arr, ctypes.POINTER(ctypes.c_int))
show("cast", (ip[0], ip[4], ctypes.cast(ip, ctypes.c_void_p).value != 0))


class Union(ctypes.Union):
    _fields_ = [("i", ctypes.c_uint32), ("f", ctypes.c_float), ("b", ctypes.c_ubyte * 4)]


u = Union(f=1.0)
show("union", (hex(u.i), list(u.b)))


class Bits(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint, 3), ("b", ctypes.c_uint, 5), ("c", ctypes.c_uint, 8)]


b = Bits(5, 17, 200)
show("bit fields", (b.a, b.b, b.c, ctypes.sizeof(b), bytes(b)))
raw = ctypes.create_string_buffer(b"abcdef", 16)
show("create_string_buffer", (raw.value, len(raw), raw.raw[:8]))
ctypes.memset(raw, ord("x"), 3)
ctypes.memmove(ctypes.addressof(raw) + 3, b"YZ", 2)
show("memset and memmove", raw.value)
show("string_at", (ctypes.string_at(raw, 4), ctypes.wstring_at(ctypes.create_unicode_buffer("wide text"), 4)))
big = ctypes.create_string_buffer(1 << 20)
ctypes.memset(big, 7, 1 << 20)
show("1 MB buffer", (big[0], big[(1 << 20) - 1], sum(big.raw[:1000])))

print("--- callbacks")
CMP = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
calls = []


def compare(a, b):
    calls.append(1)
    return a[0] - b[0]


libc.qsort.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, CMP]
data = (ctypes.c_int * 8)(5, 3, 9, 1, 7, 2, 8, 4)
libc.qsort(data, len(data), ctypes.sizeof(ctypes.c_int), CMP(compare))
show("qsort with a Python comparison", (list(data), len(calls) >= 7))
words = (ctypes.c_char_p * 4)(b"pear", b"apple", b"fig", b"banana")
SCMP = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p))
libc.qsort.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, SCMP]
libc.qsort(words, 4, ctypes.sizeof(ctypes.c_char_p), SCMP(lambda a, b: (a[0] > b[0]) - (a[0] < b[0])))
libc.qsort.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, CMP]
show("sorted words", list(words))
keep = [CMP(compare) for _ in range(50)]                                  # many closures at once: each needs its own piece of executable memory
show("50 callbacks", len({ctypes.cast(c, ctypes.c_void_p).value for c in keep}))
total = []
CB = ctypes.CFUNCTYPE(None, ctypes.c_int)
libc.bsearch.restype = ctypes.c_void_p
lookup = (ctypes.c_int * 6)(1, 3, 5, 7, 9, 11)
key = ctypes.c_int(7)
libc.bsearch.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, CMP]
found = libc.bsearch(ctypes.byref(key), lookup, 6, 4, CMP(compare))
show("bsearch", (found - ctypes.addressof(lookup)) // 4)


def failing_callback(a, b):
    raise ValueError("callback failed")


import sys  # noqa: E402

saved = sys.stderr
sys.stderr = open(os.devnull, "w")                                        # an exception in a callback is printed and ignored
libc.qsort(data, 2, 4, CMP(failing_callback))
sys.stderr = saved
show("an exception in a callback does not crash", "ok")

print("--- the interpreter's C API")
api = ctypes.pythonapi
api.Py_GetVersion.restype = ctypes.c_char_p
show("Py_GetVersion", api.Py_GetVersion().split()[0])
api.PyLong_FromLong.restype = ctypes.py_object
api.PyLong_FromLong.argtypes = [ctypes.c_long]
show("PyLong_FromLong", api.PyLong_FromLong(2 ** 40))
obj = ctypes.py_object([1, 2, 3])
show("py_object", (obj.value, ctypes.sizeof(obj)))
api.PyObject_Repr.restype = ctypes.py_object
api.PyObject_Repr.argtypes = [ctypes.py_object]
show("PyObject_Repr", api.PyObject_Repr({"a": (1, 2)}))
show("find_library", ctypes.util.find_library("c") in (None, "libc.so.6"))
try:
    ctypes.CDLL("libnothing-at-all.so")
except OSError as e:
    show("missing library", str(e).split(":")[0])
try:
    libc.no_such_function_in_libc
except AttributeError as e:
    show("missing symbol", "AttributeError")
print("--- done")
