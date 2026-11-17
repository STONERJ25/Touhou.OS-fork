"""Time passing: sleep and its precision (only lower bounds: a loaded machine may sleep longer), the monotonic clocks never going back, the wall clock, the CPU clocks counting work,
threads that sleep in parallel, timeouts of locks and conditions, the interval of a timer, and the order of sleeps."""
import os
import threading
import time


def show(label, value=""):
    print(f"{label}: {value}")


print("--- sleeping")
for pause in (0.001, 0.01, 0.05, 0.2):
    t0 = time.monotonic()
    time.sleep(pause)
    elapsed = time.monotonic() - t0
    show(f"sleep({pause})", elapsed >= pause * 0.95 and elapsed < pause + 30)
t0 = time.perf_counter()
time.sleep(0)
show("sleep(0)", time.perf_counter() - t0 < 30)
try:
    time.sleep(-1)
except ValueError as e:
    show("negative sleep", e)
t0 = time.time()
time.sleep(0.3)
show("wall clock advances", 0.25 <= time.time() - t0 < 30)
show("ns functions", (time.monotonic_ns() > 0, time.time_ns() > 1_700_000_000 * 10 ** 9, time.perf_counter_ns() > 0))

print("--- clocks")
readings = [time.monotonic() for _ in range(1000)]
show("monotonic never goes back", all(b >= a for a, b in zip(readings, readings[1:])))
readings = [time.perf_counter_ns() for _ in range(1000)]
show("perf_counter never goes back", all(b >= a for a, b in zip(readings, readings[1:])))
show("time is recent", 1_700_000_000 < time.time() < 4_000_000_000)
a, b = time.monotonic(), time.monotonic_ns()
show("monotonic and monotonic_ns agree", abs(a - b / 1e9) < 5)
c0 = time.process_time()
x = 0
t0 = time.monotonic()
while time.monotonic() - t0 < 0.3:
    x += sum(range(1000))
cpu = time.process_time() - c0
show("process_time counts a busy loop", 0.05 < cpu < 30)
c0 = time.process_time()
time.sleep(0.3)
show("process_time does not count a sleep", time.process_time() - c0 < 0.25)
t0 = time.thread_time()
y = sum(i * i for i in range(300000))
show("thread_time counts work", time.thread_time() - t0 >= 0)
times = os.times()
show("os.times", (times.user >= 0, times.system >= 0, times.elapsed > 0))
res = {}


def burn(n):
    s = time.thread_time()
    z = 0
    for _ in range(n):
        z += sum(range(2000))
    res[n] = time.thread_time() - s


ts = [threading.Thread(target=burn, args=(n,)) for n in (200, 800)]
for t in ts:
    t.start()
for t in ts:
    t.join()
show("a longer job uses more CPU", res[800] > res[200])
for clk in ("CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_BOOTTIME", "CLOCK_MONOTONIC_RAW", "CLOCK_PROCESS_CPUTIME_ID", "CLOCK_THREAD_CPUTIME_ID"):
    cid = getattr(time, clk)
    v1 = time.clock_gettime(cid)
    v2 = time.clock_gettime(cid)
    show(clk, (v1 >= 0, v2 >= v1, time.clock_gettime_ns(cid) > 0))
show("realtime and time agree", abs(time.clock_gettime(time.CLOCK_REALTIME) - time.time()) < 5)

print("--- sleeping in parallel")
t0 = time.monotonic()
ts = [threading.Thread(target=time.sleep, args=(0.5,)) for _ in range(10)]
for t in ts:
    t.start()
for t in ts:
    t.join()
elapsed = time.monotonic() - t0
show("ten threads sleeping 0.5 s take about 0.5 s, not 5", 0.45 <= elapsed < 4.0)
order = []
lock = threading.Lock()


def sleeper(n):
    time.sleep(0.4 * n)
    with lock:
        order.append(n)


ts = [threading.Thread(target=sleeper, args=(n,)) for n in (3, 1, 4, 2)]
for t in ts:
    t.start()
for t in ts:
    t.join()
show("sleeps end in order", order)

print("--- time-outs")
lk = threading.Lock()
lk.acquire()
t0 = time.monotonic()
show("lock time-out", (lk.acquire(timeout=0.3), time.monotonic() - t0 >= 0.28))
ev = threading.Event()
t0 = time.monotonic()
show("event time-out", (ev.wait(0.3), time.monotonic() - t0 >= 0.28))
cv = threading.Condition()
with cv:
    t0 = time.monotonic()
    show("condition time-out", (cv.wait(0.3), time.monotonic() - t0 >= 0.28))
t0 = time.monotonic()
th = threading.Thread(target=time.sleep, args=(1,))
th.start()
th.join(0.3)
show("join time-out", (th.is_alive(), time.monotonic() - t0 >= 0.28))
th.join()
ticks = []
stop = threading.Event()
interval = 0.1


def ticker():
    nxt = time.monotonic()
    while not stop.is_set() and len(ticks) < 6:
        nxt += interval
        stop.wait(max(0, nxt - time.monotonic()))
        ticks.append(time.monotonic())


tk = threading.Thread(target=ticker)
t0 = time.monotonic()
tk.start()
tk.join()
gaps = [b - a for a, b in zip(ticks, ticks[1:])]
show("a ticker", (len(ticks), all(g >= 0 for g in gaps), 0.5 <= ticks[-1] - t0 < 5))
print("--- done")
