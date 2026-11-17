"""The first script: the interpreter starts, finds its library, imports a few modules and prints what a program may rely on about its surroundings."""
import os
import platform
import sys

print(1 + 1)
print(sys.version_info[:3], sys.implementation.name, sys.byteorder, sys.maxsize)
print(platform.system(), platform.machine(), os.name, sys.platform)
print(sys.getdefaultencoding(), sys.getfilesystemencoding(), sys.stdout.encoding, sys.stdin.encoding)
print(os.sep, os.pathsep, os.linesep == "\n", os.curdir, os.pardir, os.devnull)
# the environment is the one the test sets (and nothing else: the interpreter adds no variable of its own)
for key in sorted(os.environ):
    print(f"{key}={os.environ[key]}")
print(sys.prefix, sys.exec_prefix, sys.base_prefix)
print(sys.path[1:])                     # sys.path[0] is the directory of the script
print(sys.flags.utf8_mode, sys.flags.hash_randomization, sys.flags.optimize)
print(__name__, len(sys.argv), os.path.basename(sys.argv[0]))
print(sorted(sys.builtin_module_names)[:12])
import encodings, json, re, hashlib, zlib, socket, select, threading, subprocess, asyncio, ctypes, sqlite3, ssl, decimal, mmap, multiprocessing  # noqa: F401,E402
print("imported", len(sys.modules) > 100)
