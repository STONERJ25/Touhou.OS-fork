"""The threading, queue and concurrent.futures modules: threads that run and join, locks, conditions, events, semaphores, barriers, thread-local data, timers, a producer
and a consumer, a thread pool, exceptions in threads, and many threads at once. The order of the output does not depend on how the threads are scheduled."""
import concurrent.futures
import queue
import sys
import threading
import time


def show(label, value=""):
    print(f"{label}: {value}")


print("--- threads")
results = []
t = threading.Thread(target=lambda: results.append("ran"), name="worker-1")
show("before start", (t.is_alive(), t.name, t.daemon))
t.start()
t.join()
show("after join", (results, t.is_alive()))
show("main thread", (threading.current_thread() is threading.main_thread(), threading.current_thread().name, threading.active_count()))
box = {}
t = threading.Thread(target=lambda a, b=0: box.update(sum=a + b), args=(3,), kwargs={"b": 4})
t.start()
t.join()
show("arguments", box)
ident = {}
t = threading.Thread(target=lambda: ident.update(tid=threading.get_ident(), native=threading.get_native_id()))
t.start()
t.join()
show("identities", (ident["tid"] != threading.get_ident(), ident["native"] != threading.get_native_id()))
t = threading.Thread(target=time.sleep, args=(0.3,))
t.start()
show("join with a time-out", (t.join(0.001), t.is_alive()))
t.join()
show("daemon", threading.Thread(daemon=True).daemon)


def failing():
    raise ValueError("boom in a thread")


old_hook = threading.excepthook
caught = []
threading.excepthook = lambda args: caught.append((args.exc_type.__name__, str(args.exc_value), args.thread.name))
t = threading.Thread(target=failing, name="failing-thread")
t.start()
t.join()
threading.excepthook = old_hook
show("excepthook", caught)

print("--- locks")
counter = 0
lock = threading.Lock()


def bump(n):
    global counter
    for _ in range(n):
        with lock:
            counter += 1


threads = [threading.Thread(target=bump, args=(2000,)) for _ in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()
show("8 threads x 2000 increments", counter)
show("lock states", (lock.locked(), lock.acquire(False), lock.acquire(False), lock.locked()))
lock.release()
rl = threading.RLock()
with rl:
    with rl:
        show("RLock twice", rl.acquire(False))
        rl.release()
show("acquire with a time-out", (lock.acquire(timeout=0.01), lock.acquire(timeout=0.01)))
lock.release()
try:
    lock.release()
except RuntimeError as e:
    show("release of a free lock", e)

print("--- events, conditions, semaphores, barriers")
ev = threading.Event()
order = []


def waiter(n):
    ev.wait()
    order.append(n)


ws = [threading.Thread(target=waiter, args=(n,)) for n in range(4)]
for w in ws:
    w.start()
show("event is clear", (ev.is_set(), ev.wait(0.01)))
ev.set()
for w in ws:
    w.join()
show("event released", (sorted(order), ev.is_set()))
cond = threading.Condition()
items = []


def consumer():
    with cond:
        while not items:
            cond.wait()
        items.append("seen")


c = threading.Thread(target=consumer)
c.start()
with cond:
    items.append("item")
    cond.notify()
c.join()
show("condition", items)
done = []
cond2 = threading.Condition()


def ready_waiter(n):
    with cond2:
        cond2.wait_for(lambda: len(done) >= 0 and flag[0])
        done.append(n)


flag = [False]
ts = [threading.Thread(target=ready_waiter, args=(n,)) for n in range(3)]
for t in ts:
    t.start()
with cond2:
    flag[0] = True
    cond2.notify_all()
for t in ts:
    t.join()
show("notify_all", sorted(done))
sem = threading.Semaphore(2)
show("semaphore", (sem.acquire(False), sem.acquire(False), sem.acquire(False)))
sem.release()
show("semaphore released", sem.acquire(False))
bs = threading.BoundedSemaphore(1)
bs.acquire()
bs.release()
try:
    bs.release()
except ValueError as e:
    show("BoundedSemaphore", e)
barrier = threading.Barrier(3)
passed = []


def at_barrier(n):
    passed.append(("before", n))
    barrier.wait()
    passed.append(("after", n))


ts = [threading.Thread(target=at_barrier, args=(n,)) for n in range(3)]
for t in ts:
    t.start()
for t in ts:
    t.join()
show("barrier", ([p[0] for p in passed[:3]], [p[0] for p in passed[3:]], barrier.n_waiting, barrier.parties))
local = threading.local()
local.value = "main"
seen = {}


def uses_local(n):
    local.value = f"thread {n}"
    time.sleep(0.01)
    seen[n] = local.value


ts = [threading.Thread(target=uses_local, args=(n,)) for n in range(4)]
for t in ts:
    t.start()
for t in ts:
    t.join()
show("thread-local", (local.value, [seen[n] for n in range(4)]))
fired = []
tm = threading.Timer(0.05, fired.append, args=("fired",))
tm.start()
tm.join()
show("Timer", fired)
tm = threading.Timer(30, fired.append, args=("never",))
tm.start()
tm.cancel()
tm.join()
show("cancelled Timer", fired)

print("--- queues")
q = queue.Queue()
out = []


def producer():
    for n in range(5):
        q.put(n)
    q.put(None)


def consumer2():
    while True:
        item = q.get()
        if item is None:
            break
        out.append(item * item)
        q.task_done()


p, c = threading.Thread(target=producer), threading.Thread(target=consumer2)
p.start()
c.start()
p.join()
c.join()
show("producer and consumer", out)
q = queue.Queue(maxsize=2)
q.put(1)
q.put(2)
try:
    q.put(3, block=False)
except queue.Full:
    show("queue full", q.qsize())
try:
    queue.Queue().get(timeout=0.01)
except queue.Empty:
    show("queue empty", "Empty")
pq = queue.PriorityQueue()
for n in (5, 1, 3):
    pq.put(n)
show("priority queue", [pq.get() for _ in range(3)])
lq = queue.LifoQueue()
for n in (1, 2, 3):
    lq.put(n)
show("lifo queue", [lq.get() for _ in range(3)])
jq = queue.Queue()
for n in range(6):
    jq.put(n)
total = []


def drain():
    while True:
        try:
            total.append(jq.get_nowait())
        except queue.Empty:
            return
        jq.task_done()


ts = [threading.Thread(target=drain) for _ in range(3)]
for t in ts:
    t.start()
jq.join()
for t in ts:
    t.join()
show("task_done and join", sorted(total))

print("--- executors")
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
    show("map", list(ex.map(lambda n: n * n, range(10))))
    futs = [ex.submit(pow, 2, n) for n in range(8)]
    show("submit", [f.result() for f in futs])
    f = ex.submit(lambda: 1 / 0)
    show("exception", (type(f.exception()).__name__, f.done()))
    futs = {ex.submit(lambda n=n: n + 100): n for n in range(5)}
    show("as_completed", sorted(f.result() for f in concurrent.futures.as_completed(futs)))
    done, not_done = concurrent.futures.wait([ex.submit(time.sleep, 0.01) for _ in range(3)])
    show("wait", (len(done), len(not_done)))
try:
    ex.submit(len, "x")
except RuntimeError as e:
    show("submit after shutdown", e)
with concurrent.futures.ThreadPoolExecutor(max_workers=2, thread_name_prefix="pool") as ex:
    names = sorted(set(ex.map(lambda n: threading.current_thread().name.rsplit("_", 1)[0], range(6))))
    show("thread names", names)
f = concurrent.futures.Future()
threading.Timer(0.05, f.set_result, args=("late",)).start()
show("Future", f.result(timeout=30))
f = concurrent.futures.Future()
show("cancel", (f.cancel(), f.cancelled()))

print("--- many threads")
n_threads = 40
latch = threading.Barrier(n_threads + 1)
got = []
glock = threading.Lock()


def many(n):
    latch.wait()
    with glock:
        got.append(n)


ts = [threading.Thread(target=many, args=(n,)) for n in range(n_threads)]
for t in ts:
    t.start()
latch.wait()
for t in ts:
    t.join()
show("40 threads released together", (len(got), sorted(got) == list(range(n_threads)), threading.active_count()))
sys.setswitchinterval(0.0005)
acc = [0, 0]


def spin(i):
    for _ in range(20000):
        acc[i] += 1


ts = [threading.Thread(target=spin, args=(i,)) for i in range(2)]
for t in ts:
    t.start()
for t in ts:
    t.join()
show("busy threads", acc)
show("stack size", threading.stack_size() >= 0)
print("--- done")
