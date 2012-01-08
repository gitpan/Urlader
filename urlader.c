#include "urlib.h"
#include "urlib.c"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include "liblzf/lzf_d.c"

/* some simple? dynamic memory management for paths might save ltos of ram */
static u_handle pack_handle;
static u_handle lock_handle;
static char tmppath[MAX_PATH];

static int exe_argc;
static u_dynbuf exe_argv;
static u_dynbuf exe_args; /* actual arguments strings copied here */

static void
tmpdir (const char *dir)
{
  static int cnt;

  for (;;)
    {
//      #ifdef _WIN32
//        sprintf (tmppath, "%s/%x_%x.tmp", dir, (unsigned int)GetCurrentProcessId (), ++cnt);
//      #else
        sprintf (tmppath, "%s/t-%x_%x.tmp", dir, (unsigned int)getpid ()             , ++cnt);
//      #endif

      if (!u_mkdir (tmppath))
        return;
    }
}

static void
systemv (const char *const argv[])
{
#ifdef _WIN32
  _spawnv (P_WAIT, argv [0], argv);
#else
  pid_t pid = fork ();

  if (pid < 0)
    u_fatal ("fork failure");

  if (!pid)
    {
      execv (argv [0], (void *)argv);
      _exit (124);
    }

  int status;
  waitpid (pid, &status, 0);
#endif
}

static void
deltree (const char *path)
{
  #ifdef _WIN32
    char buf[MAX_PATH * 2 + 64];
    const char *argv[] = { getenv ("COMSPEC"), "/c", "rd", "/s", "/q", buf, 0 };
    sprintf (buf, "\"%s\"", path);
  #else
    const char *argv[] = { "/bin/rm", "-rf", path, 0 };
  #endif
  systemv (argv);
}

static char *pack_base, *pack_end;
static struct u_pack_tail *pack_tail;
static struct u_pack_hdr *pack_cur;
static char *scratch;
static unsigned int scratch_size;

#define PACK_NAME ((char *)(pack_cur + 1))
#define PACK_DATA (PACK_NAME + u_16 (pack_cur->namelen) + 1)
#define PACK_VALID pack_cur->type

static void
pack_next (void)
{
  unsigned int d = u_32 (pack_cur->datalen);

  pack_cur = (struct u_pack_hdr *)(PACK_DATA + d);
}

static void
pack_unmap (void)
{
  if (pack_base)
    {
      u_munmap (pack_base, pack_end - pack_base);
      pack_base = 0;
    }

  if (scratch)
    {
      u_free (scratch, scratch_size);
      scratch = 0;
    }
}

static int
pack_map (void)
{
  char *addr;
  unsigned int size;

#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION fi;

  if (!GetFileInformationByHandle (pack_handle, &fi))
    return 0;

  size = fi.nFileSizeLow;
#else
  size = lseek (pack_handle, 0, SEEK_END);
#endif

  addr = u_mmap (pack_handle, size);
  if (!addr)
    return 0;

  /*pack_unmap ();*/

  pack_base = addr;
  pack_end  = pack_base + size;

  pack_tail = (void *)(pack_end - sizeof (*pack_tail));

  if (memcmp (pack_tail->magic, TAIL_MAGIC, sizeof (TAIL_MAGIC) - 1))
    return 0;

  pack_cur = (struct u_pack_hdr *)(pack_end - u_32 (pack_tail->size));

  if (pack_cur->type != T_META)
    return 0;

  scratch = u_malloc (scratch_size = u_32 (pack_tail->max_uncompressed));
  if (!scratch)
    return 0;

  return 1;
}

static void
add_arg (char *arg, unsigned int len)
{
  char *addr = u_dynbuf_append (&exe_args,  arg, len);
  u_dynbuf_append (&exe_argv, &addr, sizeof (addr));
}

static void
load (void)
{
  strcpy (exe_id , PACK_NAME);
  strcpy (exe_ver, PACK_DATA);
  u_set_exe_info ();

  if (u_chdir (exe_dir))
    u_fatal ("unable to change to application data directory");

  u_handle h = u_open ("override");
  if (u_valid (h))
    {
      u_handle oh = pack_handle;

      pack_unmap ();
      pack_handle = h;

      if (pack_map ()
          && strcmp (exe_id , PACK_NAME) == 0
          && strcmp (exe_ver, PACK_DATA) <= 0)
        u_setenv ("URLADER_OVERRIDE", "override");
      else
        {
          pack_unmap ();
          pack_handle = oh;
          oh = h;
          pack_map ();
        }

      u_close (oh);
    }

  strcpy (exe_ver, PACK_DATA);
  u_set_exe_info ();
  pack_next ();

  for (;;)
    {
      if (pack_cur->type == T_ENV)
        u_setenv (PACK_NAME, PACK_DATA);
      else if (pack_cur->type == T_ARG)
        add_arg (PACK_NAME, u_16 (pack_cur->namelen) + 1);
      else
        break;

      pack_next ();
    }
 
done_env_arg:

  strcat (strcpy (tmppath, execdir), ".lck");
  lock_handle = u_lock (tmppath, 0, 1);
  if (!lock_handle)
    u_fatal ("unable to lock application instance");

  if (access (execdir, F_OK))
    {
      // does not exist yet, so unpack and move
      tmpdir (exe_dir);

      if (u_chdir (tmppath))
        u_fatal ("unable to change to new instance directory");

      for (;;)
        {
          switch (pack_cur->type)
            {
              case T_DIR:
                u_mkdir (PACK_NAME);
                break;

              case T_FILE:
                {
                  u_handle h = u_creat (PACK_NAME, pack_cur->flags & F_EXEC);
                  unsigned int dlen, len = u_32 (pack_cur->datalen);
                  char *data = PACK_DATA;

                  if (pack_cur->flags & F_LZF)
                    if (dlen = lzf_decompress (data, len, scratch, scratch_size))
                      {
                        data = scratch;
                        len  = dlen;
                      }
                    else
                      u_fatal ("unable to uncompress file data - pack corrupted?");

                  if (!u_valid (h))
                    u_fatal ("unable to unpack file from packfile - disk full?");

                  if (u_write (h, data, len) != len)
                    u_fatal ("unable to unpack file from packfile - disk full?");

                  u_fsync (h);
                  u_close (h);
                }
                break;

              case T_NULL:
                goto done;
            }

          pack_next ();
        }

done:
      if (u_chdir (datadir))
        u_fatal ("unable to change to data directory");

      u_sync ();

      if (u_rename (tmppath, execdir))
        deltree (tmppath); // if move fails, delete new, assume other process created it independently
    }

  pack_unmap ();
  u_close (pack_handle);

  if (u_chdir (execdir))
    u_fatal ("unable to change to application instance directory");

  u_setenv ("URLADER_VERSION", URLADER_VERSION);

#if 0
  // yes, this is overkill
  u_setenv ("SHLIB_PATH"        , execdir); // hpux
  u_setenv ("LIBPATH"           , execdir); // aix
  u_setenv ("LD_LIBRARY_PATH"   , execdir); // most elf systems
  u_setenv ("LD_LIBRARY_PATH_32", execdir); // solaris
  u_setenv ("LD_LIBRARY_PATH_64", execdir); // solaris
  u_setenv ("LD_LIBRARYN32_PATH", execdir); // irix
  u_setenv ("LD_LIBRARY64_PATH" , execdir); // irix
  u_setenv ("DYLD_LIBRARY_PATH" , execdir); // os sucks from apple
#endif
}

static void
execute (void)
{
  char *null = 0;
  u_dynbuf_append (&exe_argv, &null, sizeof (null));
  systemv ((const char *const *)exe_argv.addr);
}

// this argc/argv is without argv [0]
static void
doit (int argc, char *argv[])
{
  int i;

  u_setenv ("URLADER_CURRDIR", currdir);

  u_set_datadir ();
  u_mkdir (datadir);

  if (!pack_map ())
    u_fatal ("unable to map pack file - executable corrupted?");

  load ();

  while (argc--)
    {
      add_arg (*argv, strlen (*argv) + 1);
      ++argv;
    }

  execute ();
}

#ifdef _WIN32

int APIENTRY
WinMain (HINSTANCE hI, HINSTANCE hP, LPSTR argv, int command_show)
{
  if (!GetModuleFileName (hI, tmppath, sizeof (tmppath)))
    u_fatal ("unable to find executable pack");

  u_setenv ("URLADER_EXEPATH", tmppath);

  pack_handle = u_open (tmppath);
  if (!u_valid (pack_handle))
    u_fatal ("unable to open executable pack");

  if (!GetCurrentDirectory (sizeof (currdir), currdir))
    strcpy (currdir, ".");

  doit (1, &argv);

  return 0;
}

#else

int
main (int argc, char *argv[])
{
  u_setenv ("URLADER_EXEPATH", argv [0]);

  pack_handle = u_open (argv [0]);
  if (!u_valid (pack_handle))
    u_fatal ("unable to open executable pack");

  if (!getcwd (currdir, sizeof (currdir)))
    strcpy (currdir, ".");

#if 0
  /* intersperse hostname, for whatever reason */

  if (gethostname (tmppath, sizeof (tmppath)))
    strcpy (tmppath, "default");

  u_append (datadir, tmppath);
#endif

  doit (argc - 1, argv + 1);

  return 0;
}

#endif

