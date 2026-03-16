#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void sigchld_handler(int signo) {
    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static void install_sigchld_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) < 0) die("sigaction");
}

static int open_listen_socket(const char *port_str) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int gai = getaddrinfo(NULL, port_str, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        exit(EXIT_FAILURE);
    }

    int listen_fd = -1;
    int opt = 1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd < 0) continue;

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            close(listen_fd);
            listen_fd = -1;
            continue;
        }

        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) break;

        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(res);

    if (listen_fd < 0) die("bind/socket");
    if (listen(listen_fd, 64) < 0) die("listen");

    return listen_fd;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) break;
        p += w;
        left -= w;
    }
    return n - left;
}

static const char *reason_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Permission Denied";
        case 404: return "Not Found";
        case 500: return "Internal Error";
        case 501: return "Not Implemented";
        default:  return "Error";
    }
}

static int send_header(int client_fd, int code, size_t content_length) {
    char header[512];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.0 %d %s\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-Length: %zu\r\n"
                       "\r\n",
                       code, reason_phrase(code), content_length);
    if (len < 0 || (size_t)len >= sizeof(header)) return -1;
    return write_all(client_fd, header, len) < 0 ? -1 : 0;
}

static void send_error_response(int client_fd, const char *method, int code) {
    bool is_head = strcmp(method, "HEAD") == 0;
    char body[512];
    int blen = snprintf(body, sizeof(body),
                        "<html><body><h1>%d %s</h1></body></html>\n",
                        code, reason_phrase(code));
    if (blen < 0) blen = 0;
    size_t body_len = blen;

    if (send_header(client_fd, code, is_head ? 0 : body_len) < 0) return;
    if (!is_head) write_all(client_fd, body, body_len);
}

static void rstrip_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

static bool contains_dotdot(const char *path) {
    return strstr(path, "..") != NULL;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int stream_file_to_client(int client_fd, int file_fd) {
    char buf[8192];
    while (1) {
        ssize_t r = read(file_fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        if (write_all(client_fd, buf, r) < 0) return -1;
    }
    return 0;
}

static void handle_file_request(int client_fd, const char *method, const char *url_path) {
    if (!url_path || url_path[0] != '/') {
        send_error_response(client_fd, method, 400);
        return;
    }
    if (strcmp(url_path, "/") == 0) {
        send_error_response(client_fd, method, 400);
        return;
    }
    if (contains_dotdot(url_path)) {
        send_error_response(client_fd, method, 403);
        return;
    }
    if (strchr(url_path, '~')) {
        send_error_response(client_fd, method, 400);
        return;
    }

    const char *fs_path = url_path + 1;

    struct stat st;
    if (stat(fs_path, &st) != 0) {
        send_error_response(client_fd, method, 404);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        send_error_response(client_fd, method, 403);
        return;
    }
    if (access(fs_path, R_OK) != 0) {
        send_error_response(client_fd, method, 403);
        return;
    }

    bool is_head = strcmp(method, "HEAD") == 0;
    int fd = -1;

    if (!is_head) {
        fd = open(fs_path, O_RDONLY);
        if (fd < 0) {
            send_error_response(client_fd, method, errno == EACCES ? 403 : 404);
            return;
        }
    }

    if (send_header(client_fd, 200, st.st_size) < 0) {
        if (fd >= 0) close(fd);
        return;
    }

    if (!is_head) {
        stream_file_to_client(client_fd, fd);
        close(fd);
    }
}

static char **split_args_ampersand(const char *arg_str, int *out_argc) {
    *out_argc = 0;
    if (!arg_str || *arg_str == '\0') return NULL;

    int count = 1;
    for (const char *p = arg_str; *p; p++) if (*p == '&') count++;

    char **args = calloc(count, sizeof(char *));
    if (!args) return NULL;

    char *copy = strdup(arg_str);
    if (!copy) {
        free(args);
        return NULL;
    }

    int i = 0;
    char *save = NULL;
    char *tok = strtok_r(copy, "&", &save);
    while (tok && i < count) {
        args[i++] = strdup(tok);
        tok = strtok_r(NULL, "&", &save);
    }

    free(copy);
    *out_argc = i;
    return args;
}

static void free_args(char **args, int argc) {
    if (!args) return;
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
}

static void handle_cgi_request(int client_fd, const char *method, const char *url_path) {
    bool is_head = strcmp(method, "HEAD") == 0;

    if (!starts_with(url_path, "/cgi-like/") || contains_dotdot(url_path)) {
        send_error_response(client_fd, method, 400);
        return;
    }

    const char *qmark = strchr(url_path, '?');
    size_t prog_len = qmark ? (size_t)(qmark - url_path) : strlen(url_path);
    if (prog_len <= strlen("/cgi-like/")) {
        send_error_response(client_fd, method, 400);
        return;
    }

    char *prog_url = strndup(url_path, prog_len);
    if (!prog_url) {
        send_error_response(client_fd, method, 500);
        return;
    }

    char *prog_fs = strdup(prog_url + 1);
    free(prog_url);
    if (!prog_fs) {
        send_error_response(client_fd, method, 500);
        return;
    }

    struct stat st;
    if (stat(prog_fs, &st) != 0) {
        free(prog_fs);
        send_error_response(client_fd, method, 404);
        return;
    }
    if (access(prog_fs, X_OK) != 0 || S_ISDIR(st.st_mode)) {
        free(prog_fs);
        send_error_response(client_fd, method, 403);
        return;
    }

    const char *arg_str = qmark ? qmark + 1 : NULL;
    int argc = 0;
    char **args = split_args_ampersand(arg_str, &argc);

    char **exec_argv = calloc(argc + 2, sizeof(char *));
    if (!exec_argv) {
        free(prog_fs);
        free_args(args, argc);
        send_error_response(client_fd, method, 500);
        return;
    }

    exec_argv[0] = prog_fs;
    for (int i = 0; i < argc; i++) exec_argv[i + 1] = args[i];
    exec_argv[argc + 1] = NULL;

    pid_t cpid = fork();
    if (cpid < 0) {
        free(exec_argv);
        free(prog_fs);
        free_args(args, argc);
        send_error_response(client_fd, method, 500);
        return;
    }

    if (cpid == 0) {
        close(client_fd);
        char tmpname[128];
        snprintf(tmpname, sizeof(tmpname), "cgi-out.%ld.txt", (long)getpid());
        int outfd = open(tmpname, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (outfd < 0) _exit(127);
        dup2(outfd, STDOUT_FILENO);
        dup2(outfd, STDERR_FILENO);
        close(outfd);
        execv(exec_argv[0], exec_argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(cpid, &status, 0) < 0 && errno == EINTR);

    char tmpname[128];
    snprintf(tmpname, sizeof(tmpname), "cgi-out.%ld.txt", (long)cpid);

    struct stat outst;
    if (stat(tmpname, &outst) != 0) {
        unlink(tmpname);
        free(exec_argv);
        free(prog_fs);
        free_args(args, argc);
        send_error_response(client_fd, method, 500);
        return;
    }

    if (send_header(client_fd, 200, outst.st_size) < 0) {
        unlink(tmpname);
        free(exec_argv);
        free(prog_fs);
        free_args(args, argc);
        return;
    }

    if (!is_head) {
        int outfd = open(tmpname, O_RDONLY);
        if (outfd >= 0) {
            stream_file_to_client(client_fd, outfd);
            close(outfd);
        }
    }

    unlink(tmpname);
    free(exec_argv);
    free(prog_fs);
    free_args(args, argc);
}

static int parse_request_line(char *line, char **method_out, char **path_out) {
    *method_out = NULL;
    *path_out = NULL;

    char *save = NULL;
    char *method = strtok_r(line, " \t", &save);
    char *path = strtok_r(NULL, " \t", &save);
    char *vers = strtok_r(NULL, " \t", &save);

    if (!method || !path || !vers) return -1;

    *method_out = method;
    *path_out = path;
    return 0;
}

static void handle_one_request(int client_fd, const char *method, const char *url_path) {
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error_response(client_fd, method, 501);
        return;
    }

    if (starts_with(url_path, "/cgi-like/")) {
        handle_cgi_request(client_fd, method, url_path);
    } else {
        handle_file_request(client_fd, method, url_path);
    }
}

static void handle_connection(int client_fd) {
    FILE *in = fdopen(client_fd, "r");
    if (!in) {
        close(client_fd);
        return;
    }

    char *line = NULL;
    size_t cap = 0;

    while (1) {
        ssize_t n = getline(&line, &cap, in);
        if (n < 0) break;

        rstrip_crlf(line);
        if (line[0] == '\0') continue;

        char *work = strdup(line);
        if (!work) {
            send_error_response(client_fd, "GET", 500);
            continue;
        }

        char *method = NULL;
        char *path = NULL;
        if (parse_request_line(work, &method, &path) != 0) {
            free(work);
            send_error_response(client_fd, "GET", 400);
            continue;
        }

        handle_one_request(client_fd, method, path);
        free(work);

        while (1) {
            n = getline(&line, &cap, in);
            if (n < 0) break;
            rstrip_crlf(line);
            if (line[0] == '\0') break;
        }
   
    }

    free(line);
    fclose(in);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end = NULL;
    errno = 0;
    long port = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || port < 1024 || port > 65535) {
        fprintf(stderr, "Port must be between 1024 and 65535\n");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    install_sigchld_handler();

    int listen_fd = open_listen_socket(argv[1]);

    while (1) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int client_fd = accept(listen_fd, (struct sockaddr *)&ss, &slen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            close(listen_fd);
            handle_connection(client_fd);
            _exit(0);
        }

        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
