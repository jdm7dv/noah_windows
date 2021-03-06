#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cstdlib>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#endif

#include "common.h"
#include "cross_platform.h"
#include "syscall.h"
#include "noah.h"
#include "mm.h"

static FILE *strace_sink;
pthread_mutex_t strace_sync = PTHREAD_MUTEX_INITIALIZER;

void
init_meta_strace(const char *path)
{
  init_sink(path, &strace_sink, "strace");
}

typedef void meta_strace_hook(int syscall_num, int argc, char *argnames[6], char *typenames[6], uint64_t vals[6], uint64_t ret);

namespace {
  extern meta_strace_hook *strace_pre_hooks[NR_SYSCALLS];
  extern meta_strace_hook *strace_post_hooks[NR_SYSCALLS];
}

void
print_gstr(gstr_t str, int maxlen)
{
  fprintf(strace_sink, "\"");
  for (int i = 0; i < maxlen; i++) {
    char c = *((char*)guest_to_host(str) + i);
    if (c == '\0') {
      break;
    } else if (c == '\n') {
      fprintf(strace_sink, "\\n");
    } else if (!isprint(c)) {
      fprintf(strace_sink, "\\x%02x", c);
    } else {
      fprintf(strace_sink, "%c", c);
    }
  }
  fprintf(strace_sink, "\"");
}

void
print_arg(int syscall_num, int arg_idx, const char *arg_name, const char *type_name, uint64_t val)
{
    fprintf(strace_sink, "%s: ", arg_name);

    if (strcmp(type_name, "gstr_t") == 0) {
      print_gstr(val, 50);

    } else if (strcmp(type_name, "gaddr_t") == 0) {
      fprintf(strace_sink, "0x%016llx [host: 0x%016llx]", val, (uint64_t)guest_to_host(val));

    } else if (strcmp(type_name, "int") == 0) {
      fprintf(strace_sink, "%lld", val);

    } else {
      fprintf(strace_sink, "0x%llx", val);
    }
}

void
print_args(int syscall_num, int argc, char *argnames[6], char *typenames[6], uint64_t vals[6], uint64_t ret)
{
  for (int i = 0; i < argc; i++) {
    if (i > 0) {
      fprintf(strace_sink, ", ");
    }
    print_arg(syscall_num, i, argnames[i], typenames[i], vals[i]);
  }
}

void
print_ret(int syscall_num, int argc, char *argnames[6], char *typenames[6], uint64_t vals[6], uint64_t ret)
{
  fprintf(strace_sink, "): ret = 0x%llx", ret);
  if ((int64_t)ret < 0) {
    fprintf(strace_sink, "[%s]", linux_errno_str(-(int64_t)ret));
  }
  fprintf(strace_sink, "\n");
}

void
do_meta_strace(int syscall_num, char *syscall_name, meta_strace_hook def, meta_strace_hook **hooks, uint64_t ret,  va_list ap)
{
  int argc = 0;
  char *argnames[6];
  char *typenames[6];
  uint64_t vals[6];
  for (int i = 0; i < 6; i++) {
    typenames[i] = va_arg(ap, char*);
    argnames[i] = va_arg(ap, char*);
    vals[i] = va_arg(ap, uint64_t);

    if (typenames[i][0] == '0') {
      break;
    }
    argc++;
  }

  if (strcmp(syscall_name, "unimplemented") == 0) {
    fprintf(strace_sink, "<unimplemented systemcall>");
    def(-1, argc, argnames, typenames, vals, ret);
    return;
  }

  if (hooks[syscall_num]) {
    hooks[syscall_num](syscall_num, argc, argnames, typenames, vals, ret);
  } else {
    def(syscall_num, argc, argnames, typenames, vals, ret);
  }
}

void
meta_strace_info(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *mes;

  // TODO: vasprintf(&mes, fmt, ap);

  fprintf(strace_sink, "INFO: %s", mes);

  free(mes);
  va_end(ap);
}

/*
 * Called before systemcall.
 * Most systemcalls are traced here,
 * but result values stored in argument pointers cannot be traced. (such as read)
 */
void
meta_strace_pre(int syscall_num, char *syscall_name, ...)
{
  va_list ap;
  va_start(ap, syscall_name);

  if (!strace_sink) {
    va_end(ap);
    return;
  }

  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);

  pthread_mutex_lock(&strace_sync);
  fprintf(strace_sink, "[%d:%lld] %s(", getpid(), tid, syscall_name);

  do_meta_strace(syscall_num, syscall_name, print_args, strace_pre_hooks, 0, ap);

  fflush(strace_sink);
  pthread_mutex_unlock(&strace_sync);

  va_end(ap);
}

// Called after systemcall.
void
meta_strace_post(int syscall_num, char *syscall_name, uint64_t ret, ...)
{
  va_list ap;
  va_start(ap, ret);

  if (!strace_sink) {
    va_end(ap);
    return;
  }

  pthread_mutex_lock(&strace_sync);

  do_meta_strace(syscall_num, syscall_name, print_ret, strace_post_hooks, ret, ap);

  fflush(strace_sink);
  pthread_mutex_unlock(&strace_sync);

  va_end(ap);
}


namespace {
  meta_strace_hook *strace_pre_hooks[NR_SYSCALLS] = {
    //  [LSYS_read] = trace_read_pre,
    //  [LSYS_recvfrom] = trace_recvfrom_pre,
    //  [LSYS_write] = trace_write_pre,
    //  [LSYS_sendto] = trace_sendto_pre,
    //  [LSYS_execve] = trace_execve_pre,
    //  [LSYS_rt_sigprocmask] = trace_rt_sigprocmask_post,
      NULL
  };
  meta_strace_hook *strace_post_hooks[NR_SYSCALLS] = {
    //  [LSYS_read] = trace_read_post,
    //  [LSYS_recvfrom] = trace_recvfrom_post,
    //  [LSYS_rt_sigprocmask] = trace_rt_sigprocmask_pre,
      NULL
  };
}
