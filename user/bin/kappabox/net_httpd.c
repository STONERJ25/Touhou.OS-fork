/* httpd: a tiny static file server (GET and HEAD, directory listings, no CGI), one process per connection. Handy for demos. */
#include "netutil.h"

static const struct { const char *ext, *type; } g_mime[] = {
    { ".html", "text/html" }, { ".htm", "text/html" }, { ".txt", "text/plain" }, { ".md", "text/plain" }, { ".c", "text/plain" }, { ".h", "text/plain" },
    { ".css", "text/css" }, { ".js", "application/javascript" }, { ".json", "application/json" }, { ".xml", "application/xml" },
    { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".svg", "image/svg+xml" },
    { ".ico", "image/x-icon" }, { ".pdf", "application/pdf" }, { ".gz", "application/gzip" }, { ".zip", "application/zip" },
    { ".tar", "application/x-tar" }, { ".wav", "audio/wav" }, { ".mp3", "audio/mpeg" },
};

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot) for (unsigned i = 0; i < sizeof g_mime / sizeof g_mime[0]; i++) if (!strcasecmp(dot, g_mime[i].ext)) return g_mime[i].type;
    return "application/octet-stream";
}

static void send_all(int fd, const void *p, size_t n) {
    const char *s = p;
    while (n) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, int head_only, int code, const char *reason, const char *type, const char *body, size_t len, const char *extra) {
    char h[512];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nServer: Hakurei-httpd/1.0\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n", code, reason,
                     type, len, extra ? extra : "");
    send_all(fd, h, (size_t)n);
    if (!head_only && len) send_all(fd, body, len);
}

static void error_page(int fd, int head, int code, const char *reason) {
    char body[200];
    int n = snprintf(body, sizeof body, "<html><body><h1>%d %s</h1></body></html>\n", code, reason);
    respond(fd, head, code, reason, "text/html", body, (size_t)n, NULL);
}

static int url_decode(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && *in != '?' && *in != '#'; in++) {
        if (o + 1 >= cap) return -1;
        if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = { in[1], in[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            in += 2;
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
    return 0;
}

static int handle(int fd, const char *peer, int quiet) {
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[8192];
    size_t have = 0;
    while (have < sizeof req - 1) {
        ssize_t n = recv(fd, req + have, sizeof req - 1 - have, 0);
        if (n <= 0) return 1;
        have += (size_t)n;
        req[have] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    char method[16], target[1024], version[16] = "";
    if (sscanf(req, "%15s %1023s %15s", method, target, version) < 2) { error_page(fd, 0, 400, "Bad Request"); return 1; }
    int head = !strcmp(method, "HEAD");
    int code = 200;
    char path[1100];
    if (strcmp(method, "GET") && !head) { error_page(fd, 0, 405, "Method Not Allowed"); code = 405; goto done; }
    if (target[0] != '/' || url_decode(target, path, sizeof path) < 0) { error_page(fd, head, 400, "Bad Request"); code = 400; goto done; }
    for (const char *p = path; *p; p++)                                  /* no way out of the served directory */
        if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') && (p[2] == '/' || p[2] == 0)) { error_page(fd, head, 400, "Bad Request"); code = 400; goto done; }
    {
        const char *rel = path + 1;
        char fs[1200];
        snprintf(fs, sizeof fs, "%s", *rel ? rel : ".");
        struct stat st;
        if (stat(fs, &st) < 0) { error_page(fd, head, errno == EACCES ? 403 : 404, errno == EACCES ? "Forbidden" : "Not Found"); code = errno == EACCES ? 403 : 404; goto done; }
        if (S_ISDIR(st.st_mode)) {
            size_t pl = strlen(path);
            if (path[pl - 1] != '/') {
                char loc[1200];
                snprintf(loc, sizeof loc, "Location: %s/\r\n", path);
                respond(fd, head, 301, "Moved Permanently", "text/html", "", 0, loc);
                code = 301;
                goto done;
            }
            char idx[1300];
            snprintf(idx, sizeof idx, "%s/index.html", fs);
            if (stat(idx, &st) == 0 && S_ISREG(st.st_mode)) {
                strlcpy(fs, idx, sizeof fs);
            } else {
                DIR *d = opendir(fs);
                if (!d) { error_page(fd, head, 403, "Forbidden"); code = 403; goto done; }
                char *page = malloc(65536);
                if (!page) { closedir(d); error_page(fd, head, 500, "Internal Server Error"); code = 500; goto done; }
                size_t n = (size_t)snprintf(page, 65536, "<html><head><title>Index of %s</title></head><body><h1>Index of %s</h1><ul>\n", path, path);
                struct dirent *e;
                while ((e = readdir(d)) && n + 600 < 65536)
                    if (e->d_name[0] != '.') n += (size_t)snprintf(page + n, 65536 - n, "<li><a href=\"%s%s\">%s%s</a></li>\n", e->d_name, e->d_type == DT_DIR ? "/" : "", e->d_name, e->d_type == DT_DIR ? "/" : "");
                closedir(d);
                n += (size_t)snprintf(page + n, 65536 - n, "</ul></body></html>\n");
                respond(fd, head, 200, "OK", "text/html", page, n, NULL);
                free(page);
                goto done;
            }
        }
        int f = open(fs, O_RDONLY);
        if (f < 0) { error_page(fd, head, errno == EACCES ? 403 : 404, errno == EACCES ? "Forbidden" : "Not Found"); code = errno == EACCES ? 403 : 404; goto done; }
        fstat(f, &st);
        char h[512];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nServer: Hakurei-httpd/1.0\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", mime_for(fs), (long long)st.st_size);
        send_all(fd, h, (size_t)hn);
        if (!head) {
            char buf[8192];
            ssize_t n;
            while ((n = read(f, buf, sizeof buf)) > 0) send_all(fd, buf, (size_t)n);
        }
        close(f);
    }
done:
    if (!quiet) fprintf(stderr, "%s \"%s %s\" %d\n", peer, method, target, code);
    return 0;
}

APPLET(httpd) {
    unsigned port = 8080;
    const char *dir = ".", *bind_addr = NULL;
    int once = 0, quiet = 0, opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "p:d:b:1q")) != -1) {
        switch (opt) {
        case 'p': port = (unsigned)atoi(optarg); break;
        case 'd': dir = optarg; break;
        case 'b': bind_addr = optarg; break;
        case '1': once = 1; break;
        case 'q': quiet = 1; break;
        default: kb_error("usage: httpd [-p PORT] [-d DIR] [-b ADDRESS] [-1] [-q]"); return 2;
        }
    }
    if (!port || port > 65535) { kb_error("bad port"); return 2; }
    if (chdir(dir) < 0) { kb_error("%s: %s", dir, strerror(errno)); return 1; }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { kb_error("socket: %s", strerror(errno)); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    uint32_t ip = 0;
    if (bind_addr && !nu_resolve(bind_addr, &ip)) return 1;
    sa.sin_addr.s_addr = htonl(ip);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0) { kb_error("cannot bind port %u: %s", port, strerror(errno)); return 1; }
    if (listen(ls, 16) < 0) { kb_error("listen: %s", strerror(errno)); return 1; }
    signal(SIGPIPE, SIG_IGN);
    if (!quiet) fprintf(stderr, "httpd: serving %s on port %u\n", dir, port);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept(ls, (struct sockaddr *)&peer, &pl);
        if (c < 0) { if (errno == EINTR) continue; kb_error("accept: %s", strerror(errno)); return 1; }
        char pname[24];
        snprintf(pname, sizeof pname, "%s", nu_ip(ntohl(peer.sin_addr.s_addr)));
        if (once) { handle(c, pname, quiet); close(c); return 0; }
        pid_t pid = fork();
        if (pid == 0) {
            close(ls);
            handle(c, pname, quiet);
            close(c);
            _exit(0);
        }
        close(c);
        while (waitpid(-1, NULL, WNOHANG) > 0) { }
    }
}
