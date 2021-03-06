#ifdef HAVE_LWS_CONFIG_H
#include "lws_config.h"
#endif

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>

#ifdef __APPLE__
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif

#include <libwebsockets.h>
#include <json.h>

#include "utils.h"

#ifdef LWS_FOP_FLAG_COMPR_ACCEPTABLE_GZIP
#define USE_NEW_FOPS 1
#define USE_ADOPT_FILE 1
#else
#define USE_NEW_FOPS 0
#define USE_ADOPT_FILE 0
#endif

extern volatile bool force_exit;
extern struct lws_context *context;
extern struct tty_server *server;

struct pty_data {
    char *data;
    int len;
    STAILQ_ENTRY(pty_data) list;
};

/** Data specific to a pty process. */
struct pty_client {
    struct pty_client *next_pty_client;
    int pid;
    int pty;
    pthread_t thread;
    int nrows, ncols;
    float pixh, pixw;
    int eof_seen;  // 1 means seen; 2 reported to client
    bool exit;
    int paused;
    struct lws *first_client_wsi;
    char *obuffer; // output from child process
    size_t olen; // used length of obuffer
    size_t osize; // allocated size of obuffer
    long sent_count;
    long confirmed_count;
    struct lws *pty_wsi;
};

/** Data specific to a client connection. */
struct tty_client {
    struct pty_client *pclient;
    bool initialized;
    //bool pty_started; = pclient!=NULL
    bool authenticated;
    char hostname[100];
    char address[50];
    char *version_info; // received from client
    size_t osent; // index in obuffer

    struct lws *wsi;
    // data received from client and not yet processed.
    // (Normally, this is only if an incomplete reportEvent message.)
    char *buffer;
    size_t len; // length of data in buffer
#if USE_ADOPT_FILE
    struct lws *next_client_wsi;
#endif

#if USE_ADOPT_FILE
#else
    STAILQ_HEAD(pty, pty_data) queue;
    pthread_mutex_t lock;

    LIST_ENTRY(tty_client) list;
#endif
};
#define MASK28 0xfffffff

struct tty_server {
    LIST_HEAD(client, tty_client) clients;    // client list
    int client_count;                         // client count
    char *prefs_json;                         // client preferences
    char *credential;                         // encoded basic auth credential
    int reconnect;                            // reconnect timeout
    char *index;                              // custom index.html
    char *command;                            // full command line
    char **argv;                              // command with arguments
    int sig_code;                             // close signal
    char *sig_name;                           // human readable signal string
    bool readonly;                            // whether not allow clients to write to the TTY
    bool check_origin;                        // whether allow websocket connection from different origin
    bool once;                                // whether accept only one client and exit on disconnection
    bool client_can_close;
    char *socket_path;                        // UNIX domain socket path
    pthread_mutex_t lock;
};

extern int
callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

extern void
initialize_resource_map(struct lws_context *, const char*);

extern int
callback_tty(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

extern int
callback_pty(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

extern char *get_resource_path();
extern int get_executable_directory_length();
extern char *get_bin_relative_path(const char* app_path);
extern char* get_executable_path();
extern char *get_bin_relative_path(const char* app_path);

#if COMPILED_IN_RESOURCES
struct resource {
  char *name;
  unsigned char *data;
  unsigned int length;
};
extern struct resource resources[];
#endif

#define FOREACH_WSCLIENT(VAR, PCLIENT)      \
  for (VAR = (PCLIENT)->first_client_wsi; VAR != NULL; \
       VAR = ((struct tty_client *) lws_wsi_user(VAR))->next_client_wsi)
