/* EINA - EFL data type library
 * Copyright (C) 2015 Carsten Haitzler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library;
 * if not, see <http://www.gnu.org/licenses/>.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
# endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include "eina_debug.h"
#include "eina_types.h"
#include "eina_list.h"
#include "eina_mempool.h"
#include "eina_util.h"
#include "eina_evlog.h"
#include "eina_hash.h"
#include "eina_stringshare.h"
#include "eina_debug_private.h"

#ifdef __CYGWIN__
# define LIBEXT ".dll"
#else
# define LIBEXT ".so"
#endif

#define SIG SIGPROF

// yes - a global debug spinlock. i expect contention to be low for now, and
// when needed we can split this up into mroe locks to reduce contention when
// and if that day comes
Eina_Spinlock _eina_debug_lock;

// only init once
static Eina_Bool _inited = EINA_FALSE;
static char *_my_app_name = NULL;

extern Eina_Bool eina_module_init(void);
extern Eina_Bool eina_mempool_init(void);
extern Eina_Bool eina_list_init(void);

extern Eina_Spinlock _eina_debug_thread_lock;

static Eina_Bool _debug_disabled = EINA_FALSE;

/* Local session */
/* __thread here to allow debuggers to be master and slave by using two different threads */
static __thread Eina_Debug_Session *_session = NULL;
static Eina_Debug_Session *_last_local_session = NULL;

/* Opcode used to load a module
 * needed by the daemon to notify loading success */
static int _module_init_opcode = EINA_DEBUG_OPCODE_INVALID;
static int _module_shutdown_opcode = EINA_DEBUG_OPCODE_INVALID;
static Eina_Hash *_modules_hash = NULL;

static int _bridge_keep_alive_opcode = EINA_DEBUG_OPCODE_INVALID;

static Eina_Semaphore _thread_cmd_ready_sem;

typedef void *(*Eina_Debug_Encode_Cb)(const void *buffer, int size, int *ret_size);
typedef void *(*Eina_Debug_Decode_Cb)(const void *buffer, int size, int *ret_size);

typedef struct
{
   int magic; /* Used to certify the validity of the struct */
   const Eina_Debug_Opcode *ops;
   Eina_Debug_Opcode_Status_Cb status_cb;
} _opcode_reply_info;

struct _Eina_Debug_Session
{
   Eina_List **cbs; /* Table of callbacks lists indexed by opcode id */
   Eina_List *opcode_reply_infos;
   Eina_Debug_Dispatch_Cb dispatch_cb; /* Session dispatcher */
   Eina_Debug_Encode_Cb encode_cb; /* Packet encoder */
   Eina_Debug_Decode_Cb decode_cb; /* Packet decoder */
   double encoding_ratio; /* Encoding ratio */
   /* List of shell commands to send before the communication
    * with the daemon. Only used when a shell remote connection is requested.
    */
   Eina_List *cmds;
   int cbs_length; /* cbs table size */
   int fd_in; /* File descriptor to read */
   int fd_out; /* File descriptor to write */
   /* Indicator to wait for input before continuing sending commands.
    * Only used in shell remote connections */
   Eina_Bool wait_for_input : 1;
};

static void _opcodes_register_all(Eina_Debug_Session *session);
static void _thread_start(Eina_Debug_Session *session);

EAPI int
eina_debug_session_send_to_thread(Eina_Debug_Session *session, int dest_id, int thread_id, int op, void *data, int size)
{
   Eina_Debug_Packet_Header hdr;

   if (!session) return -1;
   if (op == EINA_DEBUG_OPCODE_INVALID) return -1;
   /* Preparation of the packet header */
   hdr.size = size + sizeof(Eina_Debug_Packet_Header);
   hdr.opcode = op;
   hdr.cid = dest_id;
   hdr.thread_id = thread_id;
   if (!session->encode_cb)
     {
        e_debug("socket: %d / opcode %X / bytes to send: %d",
              session->fd_out, op, hdr.size);
#ifndef _WIN32
        eina_spinlock_take(&_eina_debug_lock);
        /* Sending header */
        write(session->fd_out, &hdr, sizeof(hdr));
        /* Sending payload */
        if (size) write(session->fd_out, data, size);
        eina_spinlock_release(&_eina_debug_lock);
#endif
     }
   else
     {
        unsigned char *total_buf = NULL;
        void *new_buf;
        int total_size = size + sizeof(hdr), new_size = 0;
        total_buf = alloca(total_size);
        memcpy(total_buf, &hdr, sizeof(hdr));
        if (size > 0) memcpy(total_buf + sizeof(hdr), data, size);

        new_buf = session->encode_cb(total_buf, total_size, &new_size);
        e_debug("socket: %d / opcode %X / packet size %d / bytes to send: %d",
              session->fd_out, op, total_size, new_size);
#ifndef _WIN32
        eina_spinlock_take(&_eina_debug_lock);
        write(session->fd_out, new_buf, new_size);
        eina_spinlock_release(&_eina_debug_lock);
#endif
        free(new_buf);
     }

   return hdr.size;
}

EAPI int
eina_debug_session_send(Eina_Debug_Session *session, int dest, int op, void *data, int size)
{
   return eina_debug_session_send_to_thread(session, dest, 0, op, data, size);
}

static void
_daemon_greet(Eina_Debug_Session *session)
{
   /* say hello to our debug daemon - tell them our PID and protocol
      version we speak */
   /* Version + Pid + App name */
   int size = 8 + (_my_app_name ? strlen(_my_app_name) : 0) + 1;
   unsigned char *buf = alloca(size);
   int version = 1; // version of protocol we speak
   int pid = getpid();
   memcpy(buf + 0, &version, 4);
   memcpy(buf + 4, &pid, 4);
   if (_my_app_name)
      memcpy(buf + 8, _my_app_name, strlen(_my_app_name) + 1);
   else
      buf[8] = '\0';
   eina_debug_session_send(session, 0, EINA_DEBUG_OPCODE_HELLO, buf, size);
}

static void
_cmd_consume(Eina_Debug_Session *session)
{
   const char *line = NULL;
   do {
        line = eina_list_data_get(session->cmds);
        session->cmds = eina_list_remove_list(session->cmds, session->cmds);
        if (line)
          {
             if (!strncmp(line, "WAIT", 4))
               {
                  e_debug("Wait for input");
                  session->wait_for_input = EINA_TRUE;
                  return;
               }
             else if (!strncmp(line, "SLEEP_1", 7))
               {
                  e_debug("Sleep 1s");
                  sleep(1);
               }
             else
               {
                  e_debug("Apply cmd line: %s", line);
                  write(session->fd_out, line, strlen(line));
                  write(session->fd_out, "\n", 1);
               }
          }
   }
   while (line);
   /* When all the cmd has been applied, we can begin to send debug packets */
   _daemon_greet(session);
   _opcodes_register_all(session);
}

static Eina_List *
_parse_cmds(const char *cmds)
{
   Eina_List *lines = NULL;
   while (cmds && *cmds)
     {
        char *tmp = strchr(cmds, '\n');
        Eina_Stringshare *line;
        if (tmp)
          {
             line = eina_stringshare_add_length(cmds, tmp - cmds);
             cmds = tmp + 1;
          }
        else
          {
             line = eina_stringshare_add(cmds);
             cmds = NULL;
          }
        lines = eina_list_append(lines, line);
     }
   return lines;
}

#ifndef _WIN32
static int
_packet_receive(unsigned char **buffer)
{
   unsigned char *packet_buf = NULL, *size_buf;
   int rret = -1, ratio, size_sz;

   if (!_session) goto end;

   if (_session->wait_for_input)
     {
        /* Wait for input */
        char c;
        int flags = fcntl(_session->fd_in, F_GETFL, 0);
        e_debug_begin("Characters received: ");
        fcntl(_session->fd_in, F_SETFL, flags | O_NONBLOCK);
        while (read(_session->fd_in, &c, 1) == 1) e_debug_continue("%c", c);
        fcntl(_session->fd_in, F_SETFL, flags);
        e_debug_end();
        _session->wait_for_input = EINA_FALSE;
        _cmd_consume(_session);
        return 0;
     }

   ratio = _session->decode_cb && _session->encoding_ratio ? _session->encoding_ratio : 1.0;
   size_sz = sizeof(int) * ratio;
   size_buf = alloca(size_sz);
   if ((rret = read(_session->fd_in, size_buf, size_sz)) == size_sz)
     {
        int size;
        if (_session->decode_cb)
          {
             /* Decode the size if needed */
             void *size_decoded_buf = _session->decode_cb(size_buf, size_sz, NULL);
             size = (*(int *)size_decoded_buf) * _session->encoding_ratio;
             free(size_decoded_buf);
          }
        else
          {
             size = *(int *)size_buf;
          }
        e_debug("Begin to receive a packet of %d bytes", size);
        // allocate a buffer for the next bytes to receive
        packet_buf = malloc(size);
        if (packet_buf)
          {
             int cur_packet_size = size_sz;
             memcpy(packet_buf, size_buf, size_sz);
             /* Receive all the remaining packet bytes */
             while (cur_packet_size < size)
               {
                  rret = read(_session->fd_in, packet_buf + cur_packet_size, size - cur_packet_size);
                  if (rret <= 0)
                    {
                       e_debug("Error on read: %d", rret);
                       perror("Read");
                       goto end;
                    }
                  cur_packet_size += rret;
               }
             if (_session->decode_cb)
               {
                  /* Decode the packet if needed */
                  void *decoded_buf = _session->decode_cb(packet_buf, size, &cur_packet_size);
                  free(packet_buf);
                  packet_buf = decoded_buf;
               }
             *buffer = packet_buf;
             rret = cur_packet_size;
             e_debug("Received a packet of %d bytes", cur_packet_size);
          }
        else
          {
             // we couldn't allocate memory for payloa buffer
             // internal memory limit error
             e_debug("Cannot allocate %u bytes for op", (unsigned int)size);
             goto end;
          }
     }
   else
     {
        e_debug("Invalid size read %i != %i", rret, size_sz);
        goto end;
     }
end:
   if (rret <= 0 && packet_buf) free(packet_buf);
   return rret;
}
#endif

EAPI void
eina_debug_disable()
{
   _debug_disabled = EINA_TRUE;
}

EAPI void
eina_debug_session_terminate(Eina_Debug_Session *session)
{
   /* Close fd here so the thread terminates its own session by itself */
   if (!session) return;
   close(session->fd_in);
}

EAPI void
eina_debug_session_dispatch_override(Eina_Debug_Session *session, Eina_Debug_Dispatch_Cb disp_cb)
{
   if (!session) return;
   if (!disp_cb) disp_cb = eina_debug_dispatch;
   session->dispatch_cb = disp_cb;
}

typedef struct {
     Eina_Module *handle;
     Eina_Bool (*init)(void);
     Eina_Bool (*shutdown)(void);
     int ref;
} _module_info;

#define _LOAD_SYMBOL(cls_struct, pkg, sym) \
   do \
     { \
        char func_name[1024]; \
        snprintf(func_name, sizeof(func_name), "%s_debug_" #sym, pkg); \
        (cls_struct)->sym = eina_module_symbol_get((cls_struct)->handle, func_name); \
        if (!(cls_struct)->sym) \
          { \
             e_debug("Failed loading symbol '%s' from the library.", func_name); \
             eina_module_free((cls_struct)->handle); \
             (cls_struct)->handle = NULL; \
             free((cls_struct)); \
             return EINA_FALSE; \
          } \
     } \
   while (0)

static Eina_Debug_Error
_module_init_cb(Eina_Debug_Session *session, int cid, void *buffer, int size)
{
   char module_path[1024];
   _module_info *minfo = NULL;
   const char *module_name = buffer;
   char *resp;
   if (size <= 0) return EINA_DEBUG_ERROR;
   if (!_modules_hash) _modules_hash = eina_hash_string_small_new(NULL);

   minfo = eina_hash_find(_modules_hash, module_name);
   if (minfo && minfo->ref)
     {
        minfo->ref++;
        goto end;
     }

   e_debug("Init module %s", module_name);
   if (!minfo)
     {
        snprintf(module_path, sizeof(module_path), PACKAGE_LIB_DIR "/lib%s_debug"LIBEXT, module_name);
        minfo = calloc(1, sizeof(*minfo));
        eina_hash_add(_modules_hash, module_name, minfo);
     }
   if (!minfo->handle) minfo->handle = eina_module_new(module_path);
   if (!minfo->handle || !eina_module_load(minfo->handle))
     {
        e_debug("Failed loading debug module %s.", module_name);
        if (minfo->handle) eina_module_free(minfo->handle);
        minfo->handle = NULL;
        goto end;
     }

   if (!minfo->init) _LOAD_SYMBOL(minfo, module_name, init);
   if (!minfo->shutdown) _LOAD_SYMBOL(minfo, module_name, shutdown);

   if (minfo->init()) minfo->ref = 1;

end:
   resp = alloca(size + 1);
   memcpy(resp, buffer, size);
   resp[size] = !!(minfo->ref);
   eina_debug_session_send(session, cid, _module_init_opcode, resp, size+1);
   return EINA_DEBUG_OK;
}

static Eina_Debug_Error
_module_shutdown_cb(Eina_Debug_Session *session, int cid, void *buffer, int size)
{
   _module_info *minfo = NULL;
   const char *module_name = buffer;
   char *resp;
   Eina_Bool ret = EINA_TRUE;
   if (size <= 0 || !_modules_hash) return EINA_DEBUG_ERROR;

   minfo = eina_hash_find(_modules_hash, module_name);
   if (minfo)
     {
        if (!--(minfo->ref))
          {
             eina_hash_del(_modules_hash, module_name, minfo);
             if (minfo->shutdown) ret = minfo->shutdown();
             if (minfo->handle) eina_module_free(minfo->handle);
             minfo->handle = NULL;
             free(minfo);
          }
     }
   resp = alloca(size + 1);
   memcpy(resp, buffer, size);
   resp[size] = !!ret;
   eina_debug_session_send(session, cid, _module_shutdown_opcode, resp, size+1);
   return EINA_DEBUG_OK;
}

static const Eina_Debug_Opcode _EINA_DEBUG_MONITOR_OPS[] = {
       {"module/init", &_module_init_opcode, &_module_init_cb},
       {"module/shutdown", &_module_shutdown_opcode, &_module_shutdown_cb},
       {NULL, NULL, NULL}
};

static const Eina_Debug_Opcode _EINA_DEBUG_BRIDGE_OPS[] = {
       {"Bridge/Keep-Alive", &_bridge_keep_alive_opcode, NULL},
       {NULL, NULL, NULL}
};

static void
_static_opcode_register(Eina_Debug_Session *session,
      int op_id, Eina_Debug_Cb cb)
{
   if(session->cbs_length < op_id + 1)
     {
        int i = session->cbs_length;
        session->cbs_length = op_id + 16;
        session->cbs = realloc(session->cbs, session->cbs_length * sizeof(Eina_List *));
        for(; i < session->cbs_length; i++) session->cbs[i] = NULL;
     }
   if (cb)
     {
        session->cbs[op_id] = eina_list_append(session->cbs[op_id], cb);
     }
}

/*
 * Response of the daemon containing the ids of the requested opcodes.
 * PTR64 + (opcode id)*
 */
static Eina_Debug_Error
_callbacks_register_cb(Eina_Debug_Session *session, int src_id EINA_UNUSED, void *buffer, int size)
{
   _opcode_reply_info *info = NULL, *info2;
   Eina_List *itr;
   int *os;
   unsigned int count, i;

   uint64_t info_64;
   memcpy(&info_64, buffer, sizeof(uint64_t));
   info = (_opcode_reply_info *)info_64;

   if (!info) return EINA_DEBUG_ERROR;
   EINA_LIST_FOREACH(session->opcode_reply_infos, itr, info2)
     {
        if (info2 == info)
          {
             os = (int *)((unsigned char *)buffer + sizeof(uint64_t));
             count = (size - sizeof(uint64_t)) / sizeof(int);

             for (i = 0; i < count; i++)
               {
                  if (info->ops[i].opcode_id) *(info->ops[i].opcode_id) = os[i];
                  _static_opcode_register(session, os[i], info->ops[i].cb);
                  e_debug("Opcode %s -> %d", info->ops[i].opcode_name, os[i]);
               }
             if (info->status_cb) info->status_cb(EINA_TRUE);
             return EINA_DEBUG_OK;
          }
     }

   return EINA_DEBUG_ERROR;
}

static void
_opcodes_registration_send(Eina_Debug_Session *session,
      _opcode_reply_info *info)
{
   unsigned char *buf;

   int count = 0;
   int size = sizeof(uint64_t);

   while(info->ops[count].opcode_name)
     {
        size += strlen(info->ops[count].opcode_name) + 1;
        count++;
     }

   buf = alloca(size);

   uint64_t info_64 = (uint64_t)info;
   memcpy(buf, &info_64, sizeof(uint64_t));
   int size_curr = sizeof(uint64_t);

   count = 0;
   while(info->ops[count].opcode_name)
     {
        int len = strlen(info->ops[count].opcode_name) + 1;
        memcpy(buf + size_curr, info->ops[count].opcode_name, len);
        size_curr += len;
        count++;
     }

   eina_debug_session_send(session, 0, EINA_DEBUG_OPCODE_REGISTER, buf, size);
}

static void
_opcodes_register_all(Eina_Debug_Session *session)
{
   Eina_List *l;
   _opcode_reply_info *info = NULL;

   _static_opcode_register(session,
         EINA_DEBUG_OPCODE_REGISTER, _callbacks_register_cb);
   EINA_LIST_FOREACH(session->opcode_reply_infos, l, info)
        _opcodes_registration_send(session, info);;
}

static void
_opcodes_unregister_all(Eina_Debug_Session *session)
{
   Eina_List *l;
   int i;
   _opcode_reply_info *info = NULL;

   if (!session) return;
   session->cbs_length = 0;
   for (i = 0; i < session->cbs_length; i++)
      eina_list_free(session->cbs[i]);
   free(session->cbs);
   session->cbs = NULL;

   EINA_LIST_FOREACH(session->opcode_reply_infos, l, info)
     {
        const Eina_Debug_Opcode *op = info->ops;
        while(!op->opcode_name)
          {
             if (op->opcode_id) *(op->opcode_id) = EINA_DEBUG_OPCODE_INVALID;
             op++;
          }
        if (info->status_cb) info->status_cb(EINA_FALSE);
     }
}

static const char *
_socket_home_get()
{
   // get possible debug daemon socket directory base
   const char *dir = getenv("XDG_RUNTIME_DIR");
   if (!dir) dir = eina_environment_home_get();
   if (!dir) dir = eina_environment_tmp_get();
   return dir;
}

#ifndef _WIN32
#define LENGTH_OF_SOCKADDR_UN(s) \
   (strlen((s)->sun_path) + (size_t)(((struct sockaddr_un *)NULL)->sun_path))
#endif

EAPI Eina_Debug_Session *
eina_debug_local_connect(Eina_Bool is_master)
{
#ifndef _WIN32
   char buf[4096];
   int fd, socket_unix_len, curstate = 0;
   struct sockaddr_un socket_unix;
#endif

   Eina_Debug_Session *session = calloc(1, sizeof(*session));
   session->dispatch_cb = eina_debug_dispatch;
   session->fd_out = session->fd_in = -1;
   // try this socket file - it will likely be:
   //   ~/.ecore/efl_debug/0
   // or maybe
   //   /var/run/UID/.ecore/efl_debug/0
   // either way a 4k buffer should be ebough ( if it's not we're on an
   // insane system)
#ifndef _WIN32
   snprintf(buf, sizeof(buf), "%s/%s/%s/%i", _socket_home_get(), SERVER_PATH, SERVER_NAME,
         is_master ? SERVER_MASTER_PORT : SERVER_SLAVE_PORT);
   // create the socket
   fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0) goto err;
   // set the socket to close when we exec things so they don't inherit it
   if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) goto err;
   // set up some socket options on addr re-use
   if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&curstate,
                  sizeof(curstate)) < 0)
     goto err;
   // sa that it's a unix socket and where the path is
   socket_unix.sun_family = AF_UNIX;
   strncpy(socket_unix.sun_path, buf, sizeof(socket_unix.sun_path) - 1);
   socket_unix_len = LENGTH_OF_SOCKADDR_UN(&socket_unix);
   // actually connect to efl_debugd service
   if (connect(fd, (struct sockaddr *)&socket_unix, socket_unix_len) < 0)
      goto err;
   // we succeeded
   session->fd_out = session->fd_in = fd;
   // start the monitor thread
   _thread_start(session);

   _daemon_greet(session);
   _opcodes_register_all(session);
   if (!is_master)
      eina_debug_opcodes_register(session, _EINA_DEBUG_MONITOR_OPS, NULL);

   _last_local_session = session;
   return session;
err:
   // some kind of connection failure here, so close a valid socket and
   // get out of here
   if (fd >= 0) close(fd);
   if (session) free(session);
#else
   (void) _session;
   (void) type;
#endif
   return NULL;
}

EAPI Eina_Debug_Session *
eina_debug_fds_attach(int fd_in, int fd_out)
{
   Eina_Debug_Session *session = calloc(1, sizeof(*session));
   session->dispatch_cb = eina_debug_dispatch;
   session->fd_out = fd_out;
   session->fd_in = fd_in;
   // start the monitor thread
   _thread_start(session);
   _opcodes_register_all(session);
   _last_local_session = session;
   return session;
}

static Eina_Bool
_bridge_keep_alive_send(void *data)
{
   Eina_Debug_Session *s = data;
   eina_debug_session_send(s, 0, _bridge_keep_alive_opcode, NULL, 0);
   return EINA_TRUE;
}

EAPI Eina_Debug_Session *
eina_debug_shell_remote_connect(const char *cmds_str)
{
#ifndef _WIN32
   Eina_List *cmds = _parse_cmds(cmds_str);
   char *cmd = eina_list_data_get(cmds);
   int pipeToShell[2], pipeFromShell[2];
   int pid = -1;

   cmds = eina_list_remove_list(cmds, cmds);

   pipe(pipeToShell);
   pipe(pipeFromShell);
   pid = fork();
   if (pid == -1) return EINA_FALSE;
   if (!pid)
     {
        int i = 0;
        const char *args[16] = { 0 };
        /* Child */
        close(STDIN_FILENO);
        dup2(pipeToShell[0], STDIN_FILENO);
        close(STDOUT_FILENO);
        dup2(pipeFromShell[1], STDOUT_FILENO);
        args[i++] = cmd;
        do
          {
             cmd = strchr(cmd, ' ');
             if (cmd)
               {
                  *cmd = '\0';
                  args[i++] = ++cmd;
               }
          }
        while (cmd);
        args[i++] = 0;
        execvpe(args[0], (char **)args, environ);
        perror("execvpe");
        exit(-1);
     }
   else
     {
        Eina_Debug_Session *session = calloc(1, sizeof(*session));
        /* Parent */
        session->dispatch_cb = eina_debug_dispatch;
        session->fd_in = pipeFromShell[0];
        session->fd_out = pipeToShell[1];

        int flags = fcntl(session->fd_in, F_GETFL, 0);
        flags &= ~O_NONBLOCK;
        if (fcntl(session->fd_in, F_SETFL, flags) == -1) perror(0);

        eina_debug_session_shell_codec_enable(session);
        session->cmds = cmds;
        _cmd_consume(session);
        eina_debug_opcodes_register(session, _EINA_DEBUG_BRIDGE_OPS, NULL);
        eina_debug_timer_add(10000, _bridge_keep_alive_send, session);
        // start the monitor thread
        _thread_start(session);
        return session;
     }
#else
   (void) cmd;
   return NULL;
#endif
}

// this is a DEDICATED debug thread to monitor the application so it works
// even if the mainloop is blocked or the app otherwise deadlocked in some
// way. this is an alternative to using external debuggers so we can get
// users or developers to get useful information about an app at all times
static void *
_monitor(void *_data)
{
#ifndef _WIN32
   _session = _data;

   // set a name for this thread for system debugging
#ifdef EINA_HAVE_PTHREAD_SETNAME
# ifndef __linux__
   pthread_set_name_np
# else
   pthread_setname_np
# endif
     (pthread_self(), "Edbg-mon");
#endif

   // sit forever processing commands or timeouts in the debug monitor
   // thread - this is separate to the rest of the app so it shouldn't
   // impact the application specifically
   for (;_session;)
     {
        int size;
        unsigned char *buffer;

        size = _packet_receive(&buffer);
        // if not negative - we have a real message
        if (size > 0)
          {
             if (EINA_DEBUG_OK != _session->dispatch_cb(_session, buffer))
               {
                  // something we don't understand
                  e_debug("EINA DEBUG ERROR: Unknown command");
               }
             /* Free the buffer only if the default dispatcher is used */
             if (_session->dispatch_cb == eina_debug_dispatch)
                free(buffer);
          }
#if 0
        else if (size == 0)
          {
             // May be due to a response from a script line
          }
#endif
        else
          {
             close(_session->fd_in);
             _opcodes_unregister_all(_session);
             free(_session);
             _session = NULL;
          }
     }
#endif
   return NULL;
}

// start up the debug monitor if we haven't already
static void
_thread_start(Eina_Debug_Session *session)
{
   pthread_t monitor_thread;
   int err;
   sigset_t oldset, newset;

   sigemptyset(&newset);
   sigaddset(&newset, SIGPIPE);
   sigaddset(&newset, SIGALRM);
   sigaddset(&newset, SIGCHLD);
   sigaddset(&newset, SIGUSR1);
   sigaddset(&newset, SIGUSR2);
   sigaddset(&newset, SIGHUP);
   sigaddset(&newset, SIGQUIT);
   sigaddset(&newset, SIGINT);
   sigaddset(&newset, SIGTERM);
#ifdef SIGPWR
   sigaddset(&newset, SIGPWR);
#endif
   sigprocmask(SIG_BLOCK, &newset, &oldset);

   err = pthread_create(&monitor_thread, NULL, _monitor, session);

   sigprocmask(SIG_SETMASK, &oldset, NULL);
   if (err != 0)
     {
        e_debug("EINA DEBUG ERROR: Can't create monitor debug thread!");
        abort();
     }
}

/*
 * Sends to daemon:
 * - Pointer to ops: returned in the response to determine which opcodes have been added
 * - List of opcode names seperated by \0
 */
EAPI void
eina_debug_opcodes_register(Eina_Debug_Session *session, const Eina_Debug_Opcode ops[],
      Eina_Debug_Opcode_Status_Cb status_cb)
{
   if (!session) session = _last_local_session;
   if (!session) return;

   _opcode_reply_info *info = malloc(sizeof(*info));
   info->ops = ops;
   info->status_cb = status_cb;

   session->opcode_reply_infos = eina_list_append(
         session->opcode_reply_infos, info);

   //send only if _session's fd connected, if not -  it will be sent when connected
   if(session && session->fd_in != -1 && !session->cmds)
      _opcodes_registration_send(session, info);
}

/*
 * Encoder for shell sessions
 * Each byte is encoded in two bytes.
 */
static void *
_shell_encode_cb(const void *data, int src_size, int *dest_size)
{
   const char *src = data;
   int new_size = src_size * 2;
   char *dest = malloc(new_size);
   int i;
   for (i = 0; i < src_size; i++)
     {
        dest[(i << 1) + 0] = ((src[i] & 0xF0) >> 4) + 0x40;
        dest[(i << 1) + 1] = ((src[i] & 0x0F) >> 0) + 0x40;
     }
   if (dest_size) *dest_size = new_size;
   return dest;
}

/*
 * Decoder for shell sessions
 * Each two bytes are merged into one byte.
 */
static void *
_shell_decode_cb(const void *data, int src_size, int *dest_size)
{
   const char *src = data;
   int i = 0, j;
   char *dest = malloc(src_size / 2);
   if (!dest) goto error;
   for (i = 0, j = 0; j < src_size; j++)
     {
        if ((src[j] & 0xF0) == 0x40 && (src[j + 1] & 0xF0) == 0x40)
          {
             dest[i++] = ((src[j] - 0x40) << 4) | ((src[j + 1] - 0x40));
             j++;
          }
     }
   goto end;
error:
   free(dest);
   dest = NULL;
end:
   if (dest_size) *dest_size = i;
   return dest;
}

EAPI void
eina_debug_session_shell_codec_enable(Eina_Debug_Session *session)
{
   if (!session) return;
   session->encode_cb = _shell_encode_cb;
   session->decode_cb = _shell_decode_cb;
   session->encoding_ratio = 2.0;
}

static Eina_Debug_Error
_self_dispatch(Eina_Debug_Session *session, void *buffer)
{
   Eina_Debug_Packet_Header *hdr =  buffer;
   Eina_List *itr;
   int opcode = hdr->opcode;
   Eina_Debug_Cb cb = NULL;

   if (opcode >= session->cbs_length)
     {
        e_debug("Invalid opcode %d", opcode);
        return EINA_DEBUG_ERROR;
     }

   EINA_LIST_FOREACH(session->cbs[opcode], itr, cb)
     {
        if (!cb) continue;
        Eina_Debug_Error ret = cb(session, hdr->cid,
              (unsigned char *)buffer + sizeof(*hdr),
              hdr->size - sizeof(*hdr));
        if (ret == EINA_DEBUG_ERROR) return ret;
     }
   return EINA_DEBUG_OK;
}

EAPI Eina_Debug_Error
eina_debug_dispatch(Eina_Debug_Session *session, void *buffer)
{
   Eina_Debug_Packet_Header *hdr = buffer;
   Eina_Debug_Error ret = EINA_DEBUG_OK;
   if (hdr->thread_id == 0) return _self_dispatch(session, buffer);
   else
     {
        int i, nb_calls = 0;
        eina_spinlock_take(&_eina_debug_thread_lock);
        for (i = 0; i < _eina_debug_thread_active_num; i++)
          {
             _eina_debug_thread_active[i].cmd_buffer = NULL;
             if (hdr->thread_id == (int)0xFFFFFFFF ||
                   hdr->thread_id == _eina_debug_thread_active[i].thread_id)
               {
                  _eina_debug_thread_active[i].cmd_session = session;
                  _eina_debug_thread_active[i].cmd_buffer = buffer;
                  _eina_debug_thread_active[i].cmd_result = EINA_DEBUG_OK;
                  pthread_kill(_eina_debug_thread_active[i].thread, SIG);
                  nb_calls++;
               }
          }
        eina_spinlock_release(&_eina_debug_thread_lock);
        while (nb_calls)
          {
             while (nb_calls)
               {
                  eina_semaphore_lock(&_thread_cmd_ready_sem);
                  nb_calls--;
               }
             eina_spinlock_take(&_eina_debug_thread_lock);
             for (i = 0; i < _eina_debug_thread_active_num; i++)
               {
                  if (_eina_debug_thread_active[i].cmd_buffer)
                    {
                       switch (_eina_debug_thread_active[i].cmd_result)
                         {
                          case EINA_DEBUG_OK:
                               {
                                  _eina_debug_thread_active[i].cmd_buffer = NULL;
                                  break;
                               }
                          case EINA_DEBUG_ERROR:
                               {
                                  _eina_debug_thread_active[i].cmd_buffer = NULL;
                                  ret = EINA_DEBUG_ERROR;
                                  break;
                               }
                          case EINA_DEBUG_AGAIN:
                               {
                                  pthread_kill(_eina_debug_thread_active[i].thread, SIG);
                                  nb_calls++;
                                  break;
                               }
                          default: break;
                         }
                    }
               }
             eina_spinlock_release(&_eina_debug_thread_lock);
          }
     }
   return ret;
}

static void
_signal_handler(int sig EINA_UNUSED,
      siginfo_t *si EINA_UNUSED, void *foo EINA_UNUSED)
{
   int i, slot = -1;
   pthread_t self = pthread_self();
   eina_spinlock_take(&_eina_debug_thread_lock);
   for (i = 0; i < _eina_debug_thread_active_num; i++)
     {
        if (self == _eina_debug_thread_active[i].thread)
          {
             slot = i;
             break;
          }
     }
   eina_spinlock_release(&_eina_debug_thread_lock);
   if (slot != -1)
     {
        _eina_debug_thread_active[slot].cmd_result =
           _self_dispatch(_eina_debug_thread_active[slot].cmd_session,
                 _eina_debug_thread_active[slot].cmd_buffer);
     }
   eina_semaphore_release(&_thread_cmd_ready_sem, 1);
}

#ifdef __linux__
   extern char *__progname;
#endif

static void
_signal_init(void)
{
   struct sigaction sa;

   // set up signal handler for our profiling signal - eevery thread should
   // obey this (this is the case on linux - other OSs may vary)
   sa.sa_sigaction = _signal_handler;
   sa.sa_flags = SA_RESTART | SA_SIGINFO;
   sigemptyset(&sa.sa_mask);
   if (sigaction(SIG, &sa, NULL) != 0)
     e_debug("EINA DEBUG ERROR: Can't set up sig %i handler!", SIG);

   sa.sa_sigaction = NULL;
   sa.sa_handler = SIG_IGN;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;
   if (sigaction(SIGPIPE, &sa, 0) == -1) perror(0);
}

Eina_Bool
eina_debug_init(void)
{
   pthread_t self;

   // if already inbitted simply release our lock that we may have locked on
   // shutdown if we are re-initted again in the same process
   if (_inited)
     {
        eina_spinlock_release(&_eina_debug_thread_lock);
        return EINA_TRUE;
     }
   // mark as initted
   _inited = EINA_TRUE;
   eina_module_init();
   eina_mempool_init();
   eina_list_init();
   // For Windows support GetModuleFileName can be used
   // set up thread things
   eina_spinlock_new(&_eina_debug_lock);
   eina_spinlock_new(&_eina_debug_thread_lock);
   self = pthread_self();
   _eina_debug_thread_mainloop_set(&self);
   _eina_debug_thread_add(&self);
#if defined(HAVE_GETUID) && defined(HAVE_GETEUID)
   // if we are setuid - don't debug!
   if (getuid() != geteuid()) return EINA_TRUE;
#endif
   // if someone uses the EFL_NODEBUG env var or disabled debug - do not do
   // debugging. handy for when this debug code is buggy itself

#ifdef __linux__
   _my_app_name = __progname;
#endif
   if (!getenv("EFL_NODEBUG") && !_debug_disabled)
     {
        eina_debug_local_connect(EINA_FALSE);
     }
   eina_semaphore_new(&_thread_cmd_ready_sem, 0);
   _signal_init();
   _eina_debug_cpu_init();
   _eina_debug_bt_init();
   _eina_debug_timer_init();
   return EINA_TRUE;
}

Eina_Bool
eina_debug_shutdown(void)
{
   _eina_debug_timer_shutdown();
   _eina_debug_bt_shutdown();
   _eina_debug_cpu_shutdown();
   eina_semaphore_free(&_thread_cmd_ready_sem);
   eina_spinlock_take(&_eina_debug_thread_lock);
   // yes - we never free on shutdown - this is because the monitor thread
   // never exits. this is not a leak - we intend to never free up any
   // resources here because they are allocated once only ever.
   return EINA_TRUE;
}
