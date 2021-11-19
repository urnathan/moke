// Wakame - Windows+Alt Keys As Mouse Emulation
// Copyright (C) 2021 Nathan Sidwell, nathan@acm.org
// License: Affero GPL v3.0

// Although this is C++, we're only using it for syntax and staying in
// the C subset of runtime.

#include "wakamecfg.h"
// C
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
// OS
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/types.h>

namespace 
{
#if __CHAR_BIT__
unsigned const charBits = __CHAR_BIT__;
#else
unsigned const charBits = 8; // 'cos it is, isn't it
#endif  

using ul_t = unsigned long;
auto const ulBits = sizeof (ul_t) * charBits;

auto const &uinputDev = "/dev/uinput";
auto const &inputDevDir = "/dev/input";
auto const &keyboardName = " keyboard$";

// FIXME: think about how to make this more flexible
unsigned const mouseKey = KEY_LEFTMETA;
unsigned const mouseMiddleAlt = KEY_LEFTALT;
unsigned const mouseRightAlt = KEY_RIGHTALT;
}

namespace
{
char const *progName = "wakame";
bool flagVerbose = false;
}

namespace 
{
void Inform (char const *fmt, ...) {
  va_list args;
  fprintf (stderr, "%s:", progName);
  va_start (args, fmt);
  vfprintf (stderr, fmt, args);
  va_end (args);
  fprintf (stderr, "\n");
}

#define Verbose(fmt, ...)						\
  (flagVerbose ? Inform (fmt __VA_OPT__(, __VA_ARGS__)) : void(0))

template<typename T>
constexpr bool TestBit (T const *bits, unsigned n)
{
  return (bits[n / sizeof (T) * charBits] >> (n & (sizeof (T) * charBits - 1)))
    & 1;
}

// See if FD is the keyboard we want.  Must match wanted and accept
// key events.
bool IsKeyboard (int fd, char const *fName, char const *wantedName = nullptr)
{
  int version;
  char devName[256];
  int sDevLen;
  unsigned long typebits;

  if (ioctl (fd, EVIOCGVERSION, &version) < 0
      || (sDevLen = ioctl (fd, EVIOCGNAME (sizeof (devName)), devName)) < 0
      || ioctl (fd, EVIOCGBIT (0, EV_CNT), &typebits) < 0)
    {
      Verbose ("rejecting `%s': not an EVIO device", fName);
      return false;
    }

  // The name length includes the trailing NUL, check it
  unsigned devLen = unsigned (sDevLen);
  if (devLen > sizeof (devName) || !devLen || devName[devLen-1])
    {
      Verbose ("rejecting `%s': name badly formed", fName);
      return false;
    }
  devLen--; // Make it the usual len we care about

  if (!(typebits & (1 << EV_KEY)))
    {
      Verbose ("rejecting `%s' (%s): not a keyboard", fName, devName);
      return false;
    }

  if (wantedName && wantedName[0])
    {
      auto wantedLen = strlen (wantedName);
      bool anchorStart = wantedName[0] == '^';
      bool anchorEnd = wantedName[wantedLen - 1] == '$';
      bool matched = false;
      unsigned matchLen = wantedLen - anchorStart - anchorEnd;

      if (matchLen > devLen)
	; // name too long
      else if (anchorStart || anchorEnd)
	{
	  if (anchorStart && anchorEnd && matchLen != devLen)
	    ; // want exact match and lengths differ
	  else
	    matched = !memcmp (&devName[anchorStart ? 0 : devLen - matchLen],
			       &wantedName[anchorStart], matchLen);
	}
      else
	matched = strstr (devName, wantedName);

      if (!matched)
	{
	  Verbose ("rejecting `%s' (%s): does not match `%s'",
		   fName, devName, wantedName);
	  return false;
	}
    }

  Verbose ("found keyboard `%s' (%s)", fName, devName);

  return true;
}

// Find and open the keyboard, return an fd or -1 on failure.
// @parm(wanted) either filename in input dir, or name fragment.
// Fragment can be anchored at start with ^ or end with $, but it is
// not a regexp.
int FindKeyboard (char const *wanted = nullptr)
{
  int fd = -1;
  int dirfd = open (inputDevDir, O_RDONLY | O_DIRECTORY);

  if (dirfd < 0)
    Inform ("cannot open %s: %m", inputDevDir);
  else
    {
      // Try a direct open of something that might be file-like --
      // begins with '/' or './' or doesn't contain ' '
      if (wanted &&
	  (wanted[wanted[0] == '.'] == '/' || !strchr (wanted, ' ')))
	{
	  fd = openat (dirfd, wanted, O_RDONLY | O_NONBLOCK, 0);
	  if (fd >= 0)
	    {
	      if (IsKeyboard (fd, wanted, nullptr))
		{
		  close (dirfd);
		  return fd;
		}
	      close (fd);
	      fd = -1;
	    }
	}

      if (DIR *dir = fdopendir (dirfd))
	{
	  while (struct dirent const *ent = readdir (dir))
	    if (ent->d_type == DT_CHR)
	      {
		fd = openat (dirfd, ent->d_name, O_RDONLY | O_NONBLOCK, 0);
		if (fd >= 0)
		  {
		    if (IsKeyboard (fd, ent->d_name, wanted))
		      break;
		    close (fd);
		    fd = -1;
		  }
	      }
	  closedir (dir);
	}
      else
	close (dirfd);
    }

  return fd;
}

void Usage (FILE *stream = stderr)
{
  fprintf (stream, R"(Keyboard Emulation of Mouse Buttons
  Usage: %s [options] [inputkeyboard] [outputkeyboard]

If you have a buttonless trackpad, but really want mouse buttons, use
  Win - mouse 1
  Win + AltL - mouse 2
  Win + AltR - mouse 3

Options:
  -h	Help
  -v	Be verbose

Usually requires root privilege, as we muck about in /dev.

)", progName);
  fprintf (stream, "Version %s.\n", PROJECT_NAME " " PROJECT_VERSION);
  if (PROJECT_URL[0])
    fprintf (stream, "See %s for more information.\n", PROJECT_URL);
}
}

int main (int argc, char *argv[])
{
  if (auto const *pName = argv[0])
    {
      // set progName
      if (auto *slash = strrchr (pName, '/'))
	pName = slash + 1;
      progName = pName;
    }

  int argno = 1;
  for (; argno < argc; argno++)
    {
      auto const *arg = argv[argno];
      if (arg[0] != '-')
	break;
      if (!strcmp (arg, "-v"))
	flagVerbose = true;
      else if (!strcmp (arg, "-h"))
	{
	  Usage (stdout);
	  return 0;
	}
      else
	{
	  Inform ("unknown flag `%s'", arg);
	  Usage ();
	  return 1;
	}
    }

  char const *keyboard = keyboardName;
  if (argno < argc)
    keyboard = argv[argno++];

  if (argno != argc)
    {
      Inform ("unknown argument `%s'", argv[argno]);
      Usage ();
      return 1;
    }

  int keyFd = FindKeyboard (keyboard);
  if (keyFd < 0)
    {
      bool usingDefault = keyboard == keyboardName;
      Inform (usingDefault ? "cannot find keyboard%s%s"
	      : "cannot find keyboard `%s'%s",
	      usingDefault ? "" : keyboard,
	      geteuid () ? " (not root, sudo?)" : "");
      return 1;
    }
  
  close (keyFd);

  return 0;
}
