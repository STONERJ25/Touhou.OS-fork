"""Kyouko's browser (`browser`, docs/BROWSER.md) in the guest: pages served by the host over QEMU's user networking (the guest sees the
host as 10.0.2.2), the window painted and looked at in regions, a link clicked, back, the keyboard scrolling, an address typed, find in
page, a form submitted, a local file, pictures, and HTTPS with a certificate the guest is given (and refused without one).

The program is started with --trace: it prints "browser: url ..." and "browser: loaded ..." lines, which the tests wait for. Geometry
assumed: the first normal window opens with its content at (44, 56); inside it the menu bar is 24 px high, the toolbar 34 and the page
starts below them (the find bar, when open, takes 30 more). The test pages place everything with absolute positions so that clicks and
pixel checks do not depend on how text is laid out.

The pieces that need no guest (fonts, painter, the whole core against the same kind of servers) are tested on the host:
tests/host/test_browser.py.
"""
import re
import ssl
import sys
import time
from contextlib import contextmanager
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))

from harness import test, Qemu, BUILD, scaled  # noqa: E402
from guitools import Gui  # noqa: E402
import test_browser as tb  # noqa: E402  (the servers and test pictures are shared with the host tests)

VIEW = (44, 56 + 24 + 34)                  # the screen position of the page's top-left corner
RED = (0xC8, 0x10, 0x2E)
GREEN = (0x00, 0xAA, 0x00)
BLUE = (0x00, 0x00, 0xCC)
CREAM = (0xFF, 0xF8, 0xE0)
WHITE = (0xFF, 0xFF, 0xFF)

Q1 = """<!doctype html><html><head><meta charset="utf-8"><title>Q one</title></head>
<body style="margin:0;background:#fff">
<a id="l" href="/q2.html" style="position:absolute;left:20px;top:20px;width:200px;height:30px;font-size:24px;background:#ffe0e0">go on</a>
<img src="/red.png" style="position:absolute;left:20px;top:80px">
<div style="position:absolute;left:300px;top:20px;width:100px;height:40px;background:#00aa00"></div>
<form action="/search" method="get" style="position:absolute;left:20px;top:160px"><input id="q" name="q" style="width:200px;height:24px;font-size:16px"> <input type="submit" value="Go"></form>
<p style="position:absolute;left:20px;top:240px;margin:0;font-size:20px">needle in a haystack with another needle</p>
<div style="position:absolute;left:0;top:1200px;width:300px;height:50px;background:#0000cc"></div>
</body></html>"""
Q2 = """<!doctype html><html><head><meta charset="utf-8"><title>Q two</title></head>
<body style="margin:0;background:#fff8e0"><div style="position:absolute;left:50px;top:50px;width:100px;height:100px;background:#c8102e"></div></body></html>"""
SEARCH = """<!doctype html><html><head><meta charset="utf-8"><title>Search done</title></head><body style="margin:0;background:#fff8e0"></body></html>"""


def routes():
    r = tb.web_routes()
    r["/q1.html"] = lambda h: h.reply(200, Q1.encode())
    r["/q2.html"] = lambda h: h.reply(200, Q2.encode())
    r["/search"] = lambda h: h.reply(200, SEARCH.encode())
    return r


def px(g, x, y):
    return g.pixel(VIEW[0] + x, VIEW[1] + y)


def near(g, x, y, rgb, tol=20):
    return g.pixel_near(VIEW[0] + x, VIEW[1] + y, rgb, tol)


def diff_pixels(a, b, rect):
    """How many pixels of rect = (x, y, w, h) differ between two screenshots (width, height, rgb bytes)."""
    w, _, ra = a
    _, _, rb = b
    x0, y0, rw, rh = rect
    n = 0
    for y in range(y0, y0 + rh):
        for x in range(x0, x0 + rw):
            o = (y * w + x) * 3
            if ra[o:o + 3] != rb[o:o + 3]:
                n += 1
    return n


def click_until(g, x, y, check, tries=3, wait=25):
    """Clicks at (x, y); if check() does not turn true within `wait` seconds the pointer is homed and the click aimed again (a PS/2 mouse packet can be
    lost on a busy PC, which leaves the pointer somewhere else)."""
    for _ in range(tries):
        g.home()
        g.click(x, y)
        deadline = time.time() + scaled(wait)
        while time.time() < deadline:
            if check():
                return True
            time.sleep(0.5)
    return False


def logged(g, pattern):
    return lambda: re.search(pattern, g.sh("cat /tmp/browser.log", timeout=60)) is not None


def wait_log(g, pattern, timeout=90, what=""):
    """Polls the program's trace file until a line matches; returns the whole log."""
    deadline = time.time() + scaled(timeout)
    log = ""
    while time.time() < deadline:
        log = g.sh("cat /tmp/browser.log", timeout=60)
        if re.search(pattern, log):
            return log
        time.sleep(1.0)
    raise AssertionError(f"the browser never wrote {pattern!r} {what}; its log:\n{log[-1500:]}")


def wait_pixel(g, x, y, rgb, tol=20, timeout=30):
    return g.wait_pixel(VIEW[0] + x, VIEW[1] + y, rgb, tol, timeout)


@contextmanager
def guest(name, **kw):
    with Qemu(cmdline="text", name=name, mem="768M", net=True, **kw) as vm:
        g = Gui(vm, prefix=name)
        g.start_desktop()
        deadline = time.time() + scaled(60)
        while "lease 10.0.2.15" not in vm.text() and time.time() < deadline:      # the network card has an address
            time.sleep(0.5)
        yield vm, g


def start_browser(g, args, env=""):
    g.sh(f"{env} browser --trace {args} > /tmp/browser.log 2>&1 &".strip())


@test
def browser_http_pages():
    """a page over http with a picture, a link, scrolling by keys, back, an address typed, find in page, and a form that is submitted"""
    srv = tb.Server(routes())
    try:
        with guest("t-browser-http") as (vm, g):
            start_browser(g, f"http://10.0.2.2:{srv.port}/q1.html")
            wait_log(g, r'browser: loaded .*"Q one"', what="(the page did not load)")
            time.sleep(1.5)
            g.shot("page")
            assert wait_pixel(g, 52, 104, RED), "the red picture is not where it should be"
            assert near(g, 350, 40, GREEN), "the green block is missing"
            assert near(g, 30, 25, (0xFF, 0xE0, 0xE0), 6), "the link's background is missing"
            assert near(g, 600, 400, WHITE, 4), "the page ground is not white"
            assert "/red.png" in srv.requests, srv.requests

            click_until(g, VIEW[0] + 100, VIEW[1] + 35, logged(g, r'browser: loaded .*"Q two"'))      # the link
            log = wait_log(g, r'browser: loaded .*"Q two"', what="(the link did not lead anywhere)")
            assert f"browser: url http://10.0.2.2:{srv.port}/q2.html" in log, log[-600:]
            assert wait_pixel(g, 100, 100, RED) and near(g, 400, 300, CREAM, 6), "the second page is not painted"
            g.shot("second")

            g.key("left", alt=True)                                                    # back
            time.sleep(0.5)
            deadline = time.time() + scaled(60)
            while not near(g, 52, 104, RED) and time.time() < deadline:
                time.sleep(1)
            assert near(g, 52, 104, RED), "back did not bring the first page again"

            for _ in range(3):                                                         # a page is the view less 40 pixels; the page ends at 1250
                g.key("pgdn")
            time.sleep(1.5)
            assert wait_pixel(g, 50, 500, BLUE), "Page Down did not scroll to the blue block at the bottom"
            g.shot("scrolled")
            g.key("home")
            assert wait_pixel(g, 52, 104, RED), "Home did not scroll back to the top"

            g.key("l", ctrl=True)                                                      # the address field
            g.type(f"10.0.2.2:{srv.port}/q2.html")
            g.key("ret")
            wait_log(g, rf'browser: url http://10.0.2.2:{srv.port}/q2.html', what="(the typed address was not followed)")
            assert wait_pixel(g, 100, 100, RED), "the typed address did not show the second page"
            g.key("left", alt=True)
            assert wait_pixel(g, 52, 104, RED)

            g.key("f", ctrl=True)                                                      # find: both words are highlighted one after the other
            g.type("needle")
            time.sleep(1.5)
            g.shot("find")
            shot = g.screen()
            found = g.count_pixels((VIEW[0], VIEW[1] + 30 + 235, 500, 40), (0x99, 0xC8, 0xFF), 40, shot, 2)
            assert found > 40, f"find did not highlight the match ({found} pixels of the selection colour)"
            g.key("esc")
            time.sleep(0.5)

            g.click(VIEW[0] + 80, VIEW[1] + 172)                                       # the form's text field
            g.type("reimu")
            g.key("ret")
            wait_log(g, r'browser: loaded .*"Search done"', what="(the form was not submitted)")
            assert any(r.startswith("/search?q=reimu") for r in srv.requests), srv.requests
    finally:
        srv.close()


@test
def browser_files_text_and_pictures():
    """a local file, a text file, a picture, an index of a folder, a missing file and the home page"""
    files = tb.Server({"/red.png": lambda h: h.reply(200, tb.RED_PNG, "image/png")})
    try:
        _files_test(files)
    finally:
        files.close()


def _files_test(files):
    with guest("t-browser-files") as (vm, g):
        g.sh("mkdir -p /tmp/bt && cd /tmp/bt && echo '<!doctype html><title>Local page</title><body style=\"margin:0;background:#fff8e0\"><div style=\"position:absolute;left:50px;top:50px;width:100px;height:100px;background:#c8102e\"></div>' > page.html")
        g.sh("cd /tmp/bt && echo 'plain words & <angle brackets>' > note.txt")
        g.sh(f"fetch -q -O /tmp/bt/red.png http://10.0.2.2:{files.port}/red.png", timeout=60)
        start_browser(g, "/tmp/bt/page.html")
        wait_log(g, r'browser: loaded file:///tmp/bt/page.html "Local page"', what="(the file did not load)")
        assert wait_pixel(g, 100, 100, RED) and near(g, 400, 300, CREAM, 6)
        g.key("l", ctrl=True)
        g.type("/tmp/bt/note.txt")
        g.key("ret")
        wait_log(g, r'browser: loaded file:///tmp/bt/note.txt', what="(the text file did not load)")
        time.sleep(1)
        shot = g.screen()
        dark = g.count_pixels((VIEW[0] + 8, VIEW[1] + 8, 260, 20), (0, 0, 0), 90, shot, 1)
        assert dark > 20, "the text of the plain text page is not drawn"
        g.key("l", ctrl=True)
        g.type("/tmp/bt/red.png")
        g.key("ret")
        wait_log(g, r'browser: loaded file:///tmp/bt/red.png', what="(the picture did not load)")
        assert wait_pixel(g, 440, 40, RED, 24), "the picture is not shown"
        g.key("l", ctrl=True)
        g.type("/tmp/bt")
        g.key("ret")
        wait_log(g, r'browser: loaded file:///tmp/bt "Index of /tmp/bt"', what="(the folder index did not load)")
        g.key("l", ctrl=True)
        g.type("/tmp/bt/missing.html")
        g.key("ret")
        wait_log(g, r'"Nothing here but the wind"', what="(a missing file did not give the error page)")
        g.shot("missing")


@test
def browser_start_page_and_menus():
    """with nothing to open it shows the start page; the Help menu opens (over the page, which is intact again afterwards) and leads to about:browser"""
    with guest("t-browser-menus") as (vm, g):
        start_browser(g, "")
        wait_log(g, r'browser: loaded about:home "Kyouko\'s browser"', what="(the start page did not load)")
        time.sleep(1.5)
        g.shot("home")
        g.move_to(700, 500)
        time.sleep(0.5)
        before = g.screen()
        assert g.pixel_near(VIEW[0] + 650, VIEW[1] + 300, (0x1D, 0x2A, 0x5C), 12), "the start page's card is not painted"
        menu = (190, 82, 220, 48)                                                       # where the Help menu opens: over the toolbar (both fonts)
        opened = False
        for attempt in range(3):                                                       # Help, then Keys and Limits; aimed again when a mouse packet was lost
            g.home()
            g.click(236, 68)                                                           # inside the title "Help" in the bitmap font and in Gensokyo Sans
            g.move_to(700, 500)
            time.sleep(1.5)
            if diff_pixels(before, g.screen(), menu) <= 500:
                continue
            if attempt == 0:
                g.shot("help-open")
            g.click(270, 94)
            deadline = time.time() + scaled(25)
            while time.time() < deadline and not opened:
                opened = logged(g, r'''browser: loaded about:browser "About Kyouko's browser"''')()
                time.sleep(0.5)
            if opened:
                break
        assert opened, "the Help menu did not open and lead to about:browser (three tries)"
        time.sleep(1.5)
        g.key("l", ctrl=True)
        g.type("about:home")
        g.key("ret")
        time.sleep(3)
        g.move_to(700, 500)
        time.sleep(0.5)
        assert g.pixel_near(VIEW[0] + 650, VIEW[1] + 300, (0x1D, 0x2A, 0x5C), 12), "the start page did not come back"
        assert diff_pixels(before, g.screen(), menu) < 30, "the menu left pixels behind on the toolbar"


@test
def browser_window_resize_and_paint_speed():
    """the window maximised repaints the page at the new size; scrolling an article page back and forth is timed from the program's own trace"""
    routes_ = routes()
    routes_["/article.html"] = lambda h: h.reply(200, (tb.PAGES / "article.html").read_bytes())
    srv = tb.Server(routes_)
    try:
        with guest("t-browser-speed") as (vm, g):
            start_browser(g, f"http://10.0.2.2:{srv.port}/article.html")
            wait_log(g, r'browser: loaded .*"Bunbunmaru Shimbun', what="(the article did not load)")
            before = g.pixel(960, 300)                                                 # the wallpaper, to the right of the window
            after = before
            for _ in range(4):                                                         # a PS/2 mouse packet can be lost on a busy PC: aim again
                g.home()
                g.click(887, 45)                                                       # the maximise button of the title bar
                time.sleep(3)
                after = g.pixel(960, 300)
                if after != before:
                    break
            g.shot("maximised")
            assert before != after, "the window did not grow over the wallpaper, or the page was not repainted for it"
            for _ in range(6):
                g.key("pgdn")
                time.sleep(0.4)
                g.key("pgup")
                time.sleep(0.4)
            time.sleep(2.5)
            log = wait_log(g, r"browser: paint \d+ frames", what="(no paint statistics)")
            stats = re.findall(r"browser: paint (\d+) frames, average (\d+) ms, worst (\d+) ms", log)
            print("paint statistics (frames, average ms, worst ms) in QEMU without acceleration:", stats)
            assert stats and all(int(a) < 3000 for _, a, _ in stats), stats
    finally:
        srv.close()


@test
def browser_hidpi_paints_natively():
    """at 2560x1440 (ui scale 2) the page is drawn at the screen's resolution, not enlarged: the text has edges at every screen pixel"""
    srv = tb.Server(routes())
    try:
        with Qemu(cmdline="text", name="t-browser-hidpi", mem="1G", net=True, video="2560x1440") as vm:
            g = Gui(vm, prefix="t-browser-hidpi")
            g.wait_prompt()
            vm.send("fairyd &\n")
            m = vm.expect(r"fairyd: ui scale (\d+)", timeout=60)
            assert m.group(1) == "2", m.group(0)
            vm.send("hakurei-shell &\n")
            time.sleep(scaled(8))
            deadline = time.time() + scaled(60)
            while "lease 10.0.2.15" not in vm.text() and time.time() < deadline:
                time.sleep(0.5)
            start_browser(g, f"http://10.0.2.2:{srv.port}/q1.html")
            wait_log(g, r'browser: loaded .*"Q one"', what="(the page did not load at scale 2)")
            time.sleep(3)
            g.shot("page")
            ox, oy = 2 * 44, 2 * (56 + 24 + 34)                                        # the page's corner in screen pixels
            assert g.pixel_near(ox + 2 * 52, oy + 2 * 104, RED, 20), "the picture is not where it should be at scale 2"
            assert g.pixel_near(ox + 2 * 350, oy + 2 * 40, GREEN, 20), "the green block is missing at scale 2"
            shot = g.screen()
            w, h, rgb = shot
            differing = total = 0
            for y in range(oy + 2 * 240, oy + 2 * 270):                                # the paragraph's text, row by row
                for x in range(ox + 2 * 20, ox + 2 * 400, 2):
                    a = rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]
                    b = rgb[(y * w + x + 1) * 3:(y * w + x + 1) * 3 + 3]
                    if a != WHITE_BYTES or b != WHITE_BYTES:
                        total += 1
                        differing += a != b
            assert total > 200 and differing > total * 0.15, f"the text looks enlarged, not drawn at the screen's resolution ({differing} of {total} pixel pairs differ)"
    finally:
        srv.close()


WHITE_BYTES = bytes([255, 255, 255])


@test
def browser_internet():
    """(optional: skipped without internet or the `certifi` package) real pages over https against Mozilla's roots: example.com loads, a page with an expired certificate is refused"""
    import socket
    try:
        import certifi
        socket.create_connection(("example.com", 443), timeout=8).close()
    except Exception as e:                                                             # noqa: BLE001  (no certifi, or no way out)
        print("skipped: needs the certifi package and a connection to the internet:", e)
        return
    roots = Path(certifi.where()).read_bytes()
    plain = tb.Server({"/roots.pem": lambda h: h.reply(200, roots, "application/x-pem-file")})
    try:
        with guest("t-browser-internet") as (vm, g):
            g.sh(f"fetch -q -O /tmp/roots.pem http://10.0.2.2:{plain.port}/roots.pem", timeout=90)
            start_browser(g, "https://example.com/", env="SSL_CERT_FILE=/tmp/roots.pem")
            wait_log(g, r'browser: loaded https://example.com/ "Example Domain"', timeout=240, what="(example.com did not load)")
            time.sleep(1.5)
            g.shot("example")
            g.key("l", ctrl=True)
            g.type("https://expired.badssl.com/")
            g.key("ret")
            wait_log(g, r'"That voice is not who it claims to be"', timeout=240, what="(the expired certificate was not refused)")
            g.shot("expired")
    finally:
        plain.close()


@test
def browser_https_and_the_certificate_check():
    """https with a certificate the guest trusts, and the refusal (an error page, nothing of the server's page) when it does not"""
    try:
        import cryptography  # noqa: F401
    except ImportError:
        print("skipped: the Python cryptography package is needed to make test certificates")
        return
    paths = tb.make_certs(BUILD / "browser-tls-guest", ["10.0.2.2"])
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(paths[1], paths[2])
    secure = tb.Server(routes(), ctx)
    plain = tb.Server({"/ca.pem": lambda h: h.reply(200, paths[0].read_bytes(), "application/x-pem-file")})
    try:
        with guest("t-browser-tls") as (vm, g):
            g.sh(f"fetch -q -O /tmp/ca.pem http://10.0.2.2:{plain.port}/ca.pem", timeout=60)
            start_browser(g, f"https://10.0.2.2:{secure.port}/q1.html", env="SSL_CERT_FILE=/tmp/ca.pem")
            wait_log(g, r'browser: loaded .*"Q one"', timeout=150, what="(the https page did not load)")
            assert wait_pixel(g, 52, 104, RED), "the https page is not painted"
            g.shot("secure")
            g.key("q", ctrl=True)
            time.sleep(2)
            start_browser(g, f"https://10.0.2.2:{secure.port}/q1.html")               # no root store: refused
            wait_log(g, r'"The shrine keeps no list of trusted voices"', timeout=150, what="(the page without a root store was not refused)")
            assert not near(g, 52, 104, RED, 10)
            g.shot("refused")
    finally:
        secure.close()
        plain.close()
