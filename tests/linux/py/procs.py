"""The multiprocessing module with the fork start method: processes and their exit codes, queues, pipes, pools, shared values and arrays, locks, events, semaphores, barriers, a
manager process and shared memory. The results do not depend on which process runs first."""
import multiprocessing as mp
import multiprocessing.shared_memory
import os
import queue
import signal
import time


def show(label, value=""):
    print(f"{label}: {value}")


def square(n):
    return n * n


def add_to(lock_and_counter, n):
    lock, counter = lock_and_counter
    for _ in range(n):
        with lock:
            counter.value += 1


def report(q, tag):
    q.put((tag, os.getpid() != os.getppid()))


def echo_conn(conn):
    while True:
        item = conn.recv()
        if item is None:
            break
        conn.send(repr(item))
    conn.close()


def fail():
    os.dup2(os.open(os.devnull, os.O_WRONLY), 2)                 # the traceback of an uncaught exception names the script: keep it out of the output
    raise RuntimeError("boom in a process")


def sleeper(ready, release):
    ready.set()
    release.wait(60)


def wait_at(barrier, results, n):
    barrier.wait()
    results.put(n)


def child_ids(q):
    q.put(os.getpid())


if __name__ == "__main__":
    ctx = mp.get_context("fork")
    show("start method", (mp.get_start_method(), ctx.get_start_method(), sorted(mp.get_all_start_methods())))
    show("cpu_count", mp.cpu_count() >= 1)

    print("--- processes")
    p = ctx.Process(target=os._exit, args=(7,), name="seven")
    show("before", (p.name, p.is_alive(), p.exitcode, p.pid))
    p.start()
    p.join()
    show("after", (p.is_alive(), p.exitcode, p.pid > 0))
    p = ctx.Process(target=fail)
    p.start()
    p.join()
    show("an exception ends the process with 1", p.exitcode)
    ready, release = ctx.Event(), ctx.Event()
    p = ctx.Process(target=sleeper, args=(ready, release))
    p.start()
    ready.wait(60)
    show("running", (p.is_alive(), p.exitcode))
    p.terminate()
    p.join()
    show("terminate", p.exitcode == -signal.SIGTERM)
    p = ctx.Process(target=sleeper, args=(ready, release))
    p.start()
    ready.wait(60)
    p.kill()
    p.join()
    show("kill", p.exitcode)
    p = ctx.Process(target=time.sleep, args=(60,))
    p.start()
    p.join(0.1)
    show("join with a time-out", p.is_alive())
    p.kill()
    p.join()
    p = ctx.Process(target=lambda: None)
    p.start()
    p.join()
    p.close()
    show("close", "ok")
    show("current_process", (mp.current_process().name, mp.parent_process()))
    q = ctx.Queue()
    ps = [ctx.Process(target=child_ids, args=(q,)) for _ in range(3)]
    for p in ps:
        p.start()
    seen = {q.get(timeout=60) for _ in range(3)}
    for p in ps:
        p.join()
    show("three children, three pids", (len(seen), os.getpid() in seen, seen == {p.pid for p in ps}))

    print("--- queues and pipes")
    q = ctx.Queue()
    ps = [ctx.Process(target=report, args=(q, n)) for n in range(5)]
    for p in ps:
        p.start()
    got = sorted(q.get(timeout=60) for _ in range(5))
    for p in ps:
        p.join()
    show("queue", got)
    try:
        q.get(timeout=0.1)
    except queue.Empty:
        show("empty queue", "Empty")
    q = ctx.Queue(maxsize=2)
    q.put(1)
    q.put(2)
    try:
        q.put(3, timeout=0.1)
    except queue.Full:
        show("full queue", "Full")
    show("qsize", (q.qsize(), q.empty(), q.full()))
    q = ctx.SimpleQueue()
    q.put({"a": [1, 2, 3]})
    show("SimpleQueue", q.get())
    q, r = ctx.Queue(), ctx.Queue()
    pr = ctx.Process(target=lambda a, b: b.put(len(a.get()) + 1), args=(q, r))
    pr.start()
    q.put(list(range(100000)))
    show("a large message through the queue", r.get(timeout=60))
    pr.join()
    parent, child = ctx.Pipe()
    p = ctx.Process(target=echo_conn, args=(child,))
    p.start()
    child.close()                                               # so that the end of the file is seen when the child leaves
    for item in (1, "two", [3, 3, 3], {"four": 4}):
        parent.send(item)
        show("pipe", parent.recv())
    parent.send(None)
    p.join()
    show("EOF on the pipe", (parent.poll(0.1), p.exitcode))
    try:
        parent.recv()
    except EOFError:
        show("recv after the peer left", "EOFError")
    a, b = ctx.Pipe(duplex=False)
    b.send_bytes(b"raw bytes")
    show("send_bytes", (a.poll(5), a.recv_bytes()))

    print("--- pools")
    with ctx.Pool(3) as pool:
        show("map", pool.map(square, range(12)))
        show("imap", list(pool.imap(square, range(6))))
        show("imap_unordered", sorted(pool.imap_unordered(square, range(6))))
        show("starmap", pool.starmap(pow, [(2, 3), (3, 2), (10, 3)]))
        r = pool.apply_async(square, (9,))
        show("apply_async", (r.get(timeout=60), r.successful()))
        r = pool.apply_async(fail)
        try:
            r.get(timeout=60)
        except RuntimeError as e:
            show("exception from a worker", e)
        show("chunks", pool.map(square, range(100), chunksize=7)[-3:])
        show("many tasks", sum(pool.map(square, range(1000))))
    show("pool closed", "ok")

    print("--- shared memory, locks, synchronization")
    counter = ctx.Value("i", 0)
    lock = ctx.Lock()
    ps = [ctx.Process(target=add_to, args=((lock, counter), 500)) for _ in range(4)]
    for p in ps:
        p.start()
    for p in ps:
        p.join()
    show("Value with a lock", counter.value)
    arr = ctx.Array("d", [0.0] * 8)

    def fill(a, start):
        for i in range(start, 8, 2):
            a[i] = i * 1.5

    ps = [ctx.Process(target=fill, args=(arr, s)) for s in (0, 1)]
    for p in ps:
        p.start()
    for p in ps:
        p.join()
    show("Array", list(arr))
    raw = ctx.RawValue("q", 5)
    p = ctx.Process(target=lambda v: setattr(v, "value", v.value * 2), args=(raw,))
    p.start()
    p.join()
    show("RawValue", raw.value)
    txt = ctx.Array("c", 10)
    p = ctx.Process(target=lambda a: setattr(a, "value", b"shared"), args=(txt,))
    p.start()
    p.join()
    show("char array", txt.value)
    ev = ctx.Event()
    show("Event", (ev.is_set(), ev.wait(0.05)))
    p = ctx.Process(target=lambda e: e.set(), args=(ev,))
    p.start()
    p.join()
    show("set by a child", ev.is_set())
    sem = ctx.Semaphore(2)
    show("Semaphore", (sem.acquire(False), sem.acquire(False), sem.acquire(False), sem.get_value()))
    sem.release()
    show("released", sem.get_value())
    bs = ctx.BoundedSemaphore(1)
    bs.acquire()
    bs.release()
    try:
        bs.release()
    except ValueError as e:
        show("BoundedSemaphore", e)
    lk = ctx.Lock()
    show("Lock", (lk.acquire(), lk.acquire(False), lk.acquire(timeout=0.05)))
    lk.release()
    rl = ctx.RLock()
    show("RLock", (rl.acquire(), rl.acquire(), rl.release(), rl.release()))
    barrier = ctx.Barrier(3)
    results = ctx.Queue()
    ps = [ctx.Process(target=wait_at, args=(barrier, results, n)) for n in range(3)]
    for p in ps:
        p.start()
    show("Barrier", sorted(results.get(timeout=60) for _ in range(3)))
    for p in ps:
        p.join()
    cond = ctx.Condition()
    flag = ctx.Value("i", 0)

    def cond_child(c, f):
        with c:
            f.value = 1
            c.notify_all()

    p = ctx.Process(target=cond_child, args=(cond, flag))
    with cond:
        p.start()
        show("Condition", cond.wait_for(lambda: flag.value == 1, timeout=60))
    p.join()
    shm = multiprocessing.shared_memory.SharedMemory(create=True, size=64)
    shm.buf[:5] = b"hello"

    def shm_child(name):
        s = multiprocessing.shared_memory.SharedMemory(name=name)
        s.buf[5:11] = b" there"
        s.close()

    p = ctx.Process(target=shm_child, args=(shm.name,))
    p.start()
    p.join()
    show("shared_memory", (bytes(shm.buf[:11]), shm.size >= 64))
    shm.close()
    shm.unlink()

    print("--- the other start methods")
    for method in ("spawn", "forkserver"):                           # a fresh interpreter per process (spawn) or one forked from a server process (forkserver)
        sctx = mp.get_context(method)
        q = sctx.Queue()
        p = sctx.Process(target=report, args=(q, method), name=f"{method}-child")
        p.start()
        show(f"{method} queue", q.get(timeout=120))
        p.join(120)
        show(f"{method} exit code", p.exitcode)
        with sctx.Pool(2) as pool:
            show(f"{method} pool", pool.map(square, range(5)))

    print("--- a manager")
    with ctx.Manager() as m:
        d = m.dict()
        l = m.list()

        def manager_child(dd, ll, n):
            dd[n] = n * n
            ll.append(n)

        ps = [ctx.Process(target=manager_child, args=(d, l, n)) for n in range(4)]
        for p in ps:
            p.start()
        for p in ps:
            p.join()
        show("managed dict", dict(sorted(d.items())))
        show("managed list", sorted(l))
        ns = m.Namespace()
        ns.value = 42
        show("namespace", ns.value)
        mq = m.Queue()
        mq.put("through the manager")
        show("managed queue", mq.get())
    print("--- done")
