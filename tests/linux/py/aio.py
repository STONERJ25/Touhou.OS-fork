"""The asyncio module on the epoll event loop: tasks and timers, queues and locks, an echo server and client over TCP, streams, subprocesses with pipes, signal handlers, threads
and executors, time-outs and cancellation, and a UNIX-domain server."""
import asyncio
import os
import selectors
import signal
import socket
import sys
import time


def show(label, value=""):
    print(f"{label}: {value}")


async def tasks_and_timers():
    print("--- tasks and timers")
    order = []

    async def worker(n, delay):
        await asyncio.sleep(delay)
        order.append(n)
        return n * n

    results = await asyncio.gather(worker(1, 0.15), worker(2, 0.05), worker(3, 0.10))
    show("gather keeps the order of the arguments", results)
    show("the order of completion", order)
    t = asyncio.create_task(worker(4, 0.01), name="named")
    show("task", (t.get_name(), t.done()))
    show("result", await t)
    loop = asyncio.get_running_loop()
    fired = []
    loop.call_later(0.05, fired.append, "later")
    loop.call_soon(fired.append, "soon")
    await asyncio.sleep(0.2)
    show("call_soon before call_later", fired)
    h = loop.call_later(30, fired.append, "never")
    h.cancel()
    show("cancelled handle", (h.cancelled(), fired))
    t0 = loop.time()
    await asyncio.sleep(0.2)
    show("loop.time advances", loop.time() - t0 >= 0.19)
    try:
        await asyncio.wait_for(asyncio.sleep(30), timeout=0.1)
    except asyncio.TimeoutError:
        show("wait_for", "TimeoutError")
    t = asyncio.create_task(asyncio.sleep(30))
    await asyncio.sleep(0.01)
    t.cancel()
    try:
        await t
    except asyncio.CancelledError:
        show("cancel", (t.cancelled(), t.done()))

    async def failing():
        raise ValueError("task failed")

    res = await asyncio.gather(failing(), worker(5, 0.01), return_exceptions=True)
    show("return_exceptions", [type(r).__name__ if isinstance(r, Exception) else r for r in res])
    try:
        async with asyncio.timeout(0.1):
            await asyncio.sleep(30)
    except TimeoutError:
        show("asyncio.timeout", "TimeoutError")
    async with asyncio.TaskGroup() as tg:
        t1 = tg.create_task(worker(6, 0.02))
        t2 = tg.create_task(worker(7, 0.01))
    show("TaskGroup", (t1.result(), t2.result()))
    done, pending = await asyncio.wait([asyncio.create_task(worker(8, 0.01)), asyncio.create_task(worker(9, 30))], return_when=asyncio.FIRST_COMPLETED)
    show("wait FIRST_COMPLETED", (len(done), len(pending)))
    for p in pending:
        p.cancel()
    show("as_completed", sorted([await f for f in asyncio.as_completed([worker(n, 0.01 * (5 - n)) for n in range(1, 5)])]))


async def sync_primitives():
    print("--- queues, locks, events")
    q = asyncio.Queue(maxsize=2)
    produced, consumed = [], []

    async def producer():
        for n in range(6):
            await q.put(n)
            produced.append(n)
        await q.put(None)

    async def consumer():
        while (item := await q.get()) is not None:
            consumed.append(item)

    await asyncio.gather(producer(), consumer())
    show("bounded queue", (produced, consumed))
    lock = asyncio.Lock()
    log = []

    async def locked(n):
        async with lock:
            log.append(f"in {n}")
            await asyncio.sleep(0.01)
            log.append(f"out {n}")

    await asyncio.gather(*(locked(n) for n in range(3)))
    show("lock", log)
    ev = asyncio.Event()
    seen = []

    async def waiter():
        await ev.wait()
        seen.append("released")

    ws = [asyncio.create_task(waiter()) for _ in range(3)]
    await asyncio.sleep(0.01)
    ev.set()
    await asyncio.gather(*ws)
    show("event", seen)
    sem = asyncio.Semaphore(2)
    running, peak = 0, 0

    async def limited():
        nonlocal running, peak
        async with sem:
            running += 1
            peak = max(peak, running)
            await asyncio.sleep(0.02)
            running -= 1

    await asyncio.gather(*(limited() for _ in range(6)))
    show("semaphore limits concurrency", peak)
    cond = asyncio.Condition()
    ready = []

    async def cond_waiter():
        async with cond:
            await cond.wait_for(lambda: bool(ready))
            return "condition met"

    t = asyncio.create_task(cond_waiter())
    await asyncio.sleep(0.01)
    async with cond:
        ready.append(1)
        cond.notify_all()
    show("condition", await t)


async def network():
    print("--- TCP streams")
    handled = []

    async def echo(reader, writer):
        while line := await reader.readline():
            writer.write(line.upper())
            await writer.drain()
        handled.append("closed")
        writer.close()
        await writer.wait_closed()

    server = await asyncio.start_server(echo, "127.0.0.1", 0, limit=2 ** 20)
    port = server.sockets[0].getsockname()[1]
    show("server sockets", [s.getsockname()[0] for s in server.sockets])
    reader, writer = await asyncio.open_connection("127.0.0.1", port, limit=2 ** 20)
    writer.write(b"hello\n")
    show("echo", await reader.readline())
    writer.write(b"one\ntwo\nthree\n")
    show("three lines", [await reader.readline() for _ in range(3)])
    writer.write(b"x" * 100000 + b"\n")
    show("100 kB line", len(await reader.readline()))
    writer.write(b"partial")
    writer.write_eof()
    show("after write_eof", (await reader.read(100), reader.at_eof()))
    writer.close()
    await writer.wait_closed()
    await asyncio.sleep(0.1)
    show("server saw the close", handled)

    async def talk(n):
        r, w = await asyncio.open_connection("127.0.0.1", port)
        w.write(f"client {n}\n".encode())
        await w.drain()
        line = await r.readline()
        w.close()
        await w.wait_closed()
        return line

    show("ten clients", await asyncio.gather(*(talk(n) for n in range(10))))
    server.close()
    await server.wait_closed()
    try:
        await asyncio.open_connection("127.0.0.1", port)
    except ConnectionRefusedError as e:
        show("refused", e.errno)
    print("--- UNIX-domain server")
    path = "aio.sock"
    srv = await asyncio.start_unix_server(echo, path)
    r, w = await asyncio.open_unix_connection(path)
    w.write(b"over a path\n")
    show("unix echo", await r.readline())
    w.close()
    await w.wait_closed()
    srv.close()
    await srv.wait_closed()
    os.unlink(path)
    print("--- protocols and sockets")
    loop = asyncio.get_running_loop()
    got = []
    done = loop.create_future()

    class Proto(asyncio.Protocol):
        def connection_made(self, transport):
            self.t = transport
            transport.write(b"hello from the protocol")

        def data_received(self, data):                              # a stream has no message boundaries: the pieces may arrive together or apart
            got.append(data)

        def connection_lost(self, exc):                             # the server closes after its reply: the end of the stream
            done.set_result(exc)

    class Server(asyncio.Protocol):
        def connection_made(self, transport):
            transport.write(b"greeting")
            self.t = transport

        def data_received(self, data):
            self.t.write(data[::-1])
            self.t.close()

    srv = await loop.create_server(Server, "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    await loop.create_connection(Proto, "127.0.0.1", port)
    show("protocol exit", await done)
    show("protocol data", b"".join(got))
    srv.close()
    await srv.wait_closed()
    a, b = socket.socketpair()
    a.setblocking(False)
    b.setblocking(False)
    await loop.sock_sendall(a, b"raw sockets")
    show("sock_recv", await loop.sock_recv(b, 100))
    a.close()
    b.close()
    udp_got = []
    ufut = loop.create_future()

    class UDP(asyncio.DatagramProtocol):
        def datagram_received(self, data, addr):
            udp_got.append(data)
            ufut.set_result(None)

    ut, _ = await loop.create_datagram_endpoint(UDP, local_addr=("127.0.0.1", 0))
    uaddr = ut.get_extra_info("sockname")
    ct, _ = await loop.create_datagram_endpoint(asyncio.DatagramProtocol, remote_addr=uaddr)
    ct.sendto(b"udp datagram")
    await asyncio.wait_for(ufut, 30)
    show("datagram", udp_got)
    ct.close()
    ut.close()


async def processes():
    print("--- subprocesses")
    p = await asyncio.create_subprocess_exec("/bin/echo", "hello", "from", "echo", stdout=asyncio.subprocess.PIPE)
    out, _ = await p.communicate()
    show("echo", (out, p.returncode))
    p = await asyncio.create_subprocess_shell("echo to-out; echo to-err >&2; exit 3", stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    out, err = await p.communicate()
    show("shell", (out, err, p.returncode))
    p = await asyncio.create_subprocess_exec("cat", stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
    out, _ = await p.communicate(b"through cat\n" * 3)
    show("communicate with input", out)
    p = await asyncio.create_subprocess_exec("sh", "-c", "while read l; do echo got $l; done", stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
    p.stdin.write(b"one\n")
    await p.stdin.drain()
    show("conversation", await p.stdout.readline())
    p.stdin.write(b"two\n")
    show("conversation, second", await p.stdout.readline())
    p.stdin.close()
    show("exit after EOF", await p.wait())
    p = await asyncio.create_subprocess_exec("sleep", "30")
    await asyncio.sleep(0.1)
    p.terminate()
    show("terminate", await p.wait())
    p = await asyncio.create_subprocess_exec("sleep", "30")
    p.kill()
    show("kill", await p.wait())
    procs = [await asyncio.create_subprocess_exec("sh", "-c", f"echo {n}; exit {n}", stdout=asyncio.subprocess.PIPE) for n in range(4)]
    res = await asyncio.gather(*(p.communicate() for p in procs))
    show("four at once", sorted((r[0], p.returncode) for r, p in zip(res, procs)))
    try:
        await asyncio.create_subprocess_exec("/nonexistent/program")
    except FileNotFoundError as e:
        show("missing program", e.errno)
    p = await asyncio.create_subprocess_exec(sys.executable, "-c", "import sys; print('child python'); sys.exit(2)", stdout=asyncio.subprocess.PIPE)
    out, _ = await p.communicate()
    show("python child", (out, p.returncode))


async def signals_and_threads():
    print("--- signals, threads, executors")
    loop = asyncio.get_running_loop()
    hit = asyncio.Event()
    loop.add_signal_handler(signal.SIGUSR1, hit.set)
    os.kill(os.getpid(), signal.SIGUSR1)
    await asyncio.wait_for(hit.wait(), 30)
    show("add_signal_handler", hit.is_set())
    loop.remove_signal_handler(signal.SIGUSR1)
    stop = asyncio.Event()
    loop.add_signal_handler(signal.SIGTERM, stop.set)
    loop.call_later(0.05, os.kill, os.getpid(), signal.SIGTERM)
    await asyncio.wait_for(stop.wait(), 30)
    show("SIGTERM handled", stop.is_set())
    loop.remove_signal_handler(signal.SIGTERM)
    show("to_thread", await asyncio.to_thread(lambda: sum(range(1000))))
    show("run_in_executor", await loop.run_in_executor(None, pow, 2, 100))
    results = await asyncio.gather(*(asyncio.to_thread(time.sleep, 0.05) for _ in range(8)))
    show("eight blocking calls", len(results))
    fut = loop.create_future()
    import threading
    threading.Timer(0.05, loop.call_soon_threadsafe, args=(fut.set_result, "from another thread")).start()
    show("call_soon_threadsafe", await asyncio.wait_for(fut, 30))
    r, w = os.pipe()
    got = loop.create_future()
    loop.add_reader(r, lambda: got.set_result(os.read(r, 100)))
    loop.call_later(0.05, os.write, w, b"through add_reader")
    show("add_reader", await asyncio.wait_for(got, 30))
    loop.remove_reader(r)
    os.close(r)
    os.close(w)


async def main():
    show("loop class", type(asyncio.get_running_loop()).__name__)
    show("selector", selectors.DefaultSelector.__name__)
    await tasks_and_timers()
    await sync_primitives()
    await network()
    await processes()
    await signals_and_threads()
    print("--- done")


asyncio.run(main())
