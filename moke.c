// Moke - Windows+Alt Keys As Mouse Emulation -*- mode:c++ -*-
// Copyright (C) 2021 Nathan Sidwell, nathan@acm.org
// License: Affero GPL v3.0

// Although this is C++, we're only using it for syntax and staying in
// the C subset of runtime.  From the build's PoV this is C and you'll
// notice we link as a C program.

#include "mokecfg.h"
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
#include <linux/uinput.h>
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
auto const &deviceName = "Moke Key to Button Mapper";

struct KeyName 
{
  unsigned key;
  char const *name;
};

// There are only a few keys that can be used
constexpr KeyName const keys[]
= {
  {KEY_LEFTMETA, "Windows"},
  {KEY_LEFTALT, "LeftAlt"},
  {KEY_RIGHTALT, "RightAlt"},
  {KEY_LEFTCTRL, "LeftCtrl"},
  {KEY_RIGHTCTRL, "RightCtrl"},

  {KEY_LEFTMETA, "LeftMeta"},
  {KEY_LEFTALT, "Alt_L"},
  {KEY_LEFTCTRL, "Ctrl_L"},
  {KEY_LEFTMETA, "Super_L"},
  {KEY_RIGHTALT, "Alt_R"},
  {KEY_RIGHTCTRL, "Ctrl_R"},

  {0, nullptr}};
}

constexpr KeyName const buttons[]
= {
  {BTN_LEFT, "LeftMouse"},
  {BTN_MIDDLE, "MiddleMouse"},
  {BTN_RIGHT, "RightMouse"},
  {0, nullptr},
};

namespace
{
char const *progName = "";
bool flagVerbose = false;

signed char keyState[KEY_CNT];

struct Map
{
  unsigned mouse; // the mouse BTN to emit
  unsigned key;  // the keyboard KEY we want
  unsigned mod;  // keyboard modifier, if any
  char override; // overrides a non-modified button

  bool down; // whether we consider this pressed
};

auto const buttonHWM = 6;
unsigned numButtons = 0;
Map mapping[buttonHWM]
= {
  {BTN_LEFT, KEY_LEFTMETA, 0, 0, false},
  {BTN_MIDDLE, KEY_LEFTMETA, KEY_LEFTALT, 0, false},
  {BTN_RIGHT, KEY_RIGHTCTRL, 0, 0, false},
  {BTN_MIDDLE, KEY_RIGHTCTRL, KEY_RIGHTALT, 0, false},
};

}

namespace 
{
template<typename T>
constexpr bool TestBit (T const *bits, unsigned n)
{
  return (bits[n / (sizeof (T) * charBits)]
	  >> (n & (sizeof (T) * charBits - 1))) & 1;
}

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

char const *KeyName (unsigned code, KeyName const *key = keys)
{
  for (; key->key; key++)
    if (key->key == code)
      return key->name;
  return nullptr;
}

char const *ButtonName (unsigned code)
{
  return KeyName (code, buttons);
}

unsigned KeyCode (char const *name)
{
  for (auto *key = keys; key->key; key++)
    if (!strcasecmp (name, key->name))
      return key->key;
  return 0;
}

bool ParseMapping (unsigned button, char *opt)
{
  if (numButtons == buttonHWM)
    {
      Inform ("too many buttons (limit is %d)", buttonHWM);
      return false;
    }

  char *plus = strchr (opt, '+');
  if (plus)
    *plus = 0;
  unsigned code = KeyCode (opt);
  if (code)
    {
      mapping[numButtons].mouse = button;
      mapping[numButtons].key = code;
      if (plus)
	{
	  *plus++ = '+';
	  opt = plus;
	  code = KeyCode (opt);
	  mapping[numButtons].mod = code;
	}
    }

  if (code)
    numButtons++;
  else
    {
      Inform ("unknown key `%s'", opt);
      return false;
    }

  return true;
}

bool InitMapping ()
{
  if (!numButtons)
    // Use the default buttons
    while (numButtons != buttonHWM && mapping[numButtons].mouse)
      numButtons++;

  // Figure out if modifier combos override any non-modifier button
  for (unsigned ix = numButtons; ix--;)
    {
      keyState[mapping[ix].key] = -1;
      if (mapping[ix].mod)
	{
	  keyState[mapping[ix].mod] = -1;
	  for (unsigned jx = numButtons; jx--;)
	    {
	      if (mapping[jx].key == mapping[ix].mod)
		{
		  Inform ("%s modifier for %s chord is key for %s",
			  KeyName (mapping[ix].mod),
			  ButtonName (mapping[ix].mouse),
			  ButtonName (mapping[jx].mouse));
		  return false;
		}
	      if (!mapping[jx].mod && mapping[jx].key == mapping[ix].key)
		mapping[ix].override = jx + 1;
	    }
	}
    }

  return true;
}

// See if FD is the keyboard we want.  Must match wanted and accept
// key events.
enum IKC {IK_Not, IK_Moke, IK_OK, IK_Matched};

IKC IsKeyboard (int fd, char const *dir, char const *fName,
		char const *wantedName = nullptr)
{
  int version;
  char devName[MAXNAMLEN];
  int sDevLen;
  unsigned long typebits;

  if (ioctl (fd, EVIOCGVERSION, &version) < 0
      || (sDevLen = ioctl (fd, EVIOCGNAME (sizeof (devName)), devName)) < 0
      || ioctl (fd, EVIOCGBIT (0, EV_CNT), &typebits) < 0)
    {
      if (!dir || wantedName)
	Verbose ("rejecting `%s': not an EVIO device", fName);
      return IK_Not;
    }

  // The name length includes the trailing NUL, check it
  unsigned devLen = unsigned (sDevLen);
  if (devLen > sizeof (devName) || !devLen || devName[devLen-1])
    {
      if (!dir || wantedName)
	Verbose ("rejecting `%s': name badly formed", fName);
      return IK_Not;
    }
  devLen--; // Make it the usual len we care about

  if (!(typebits & (1 << EV_KEY)))
    {
      if (!dir || wantedName)
	Verbose ("rejecting `%s' (%s): not a keyboard", fName, devName);
      return IK_Not;
    }

  if (dir && !strcmp (devName, deviceName))
    {
      Inform ("already present at `%s/%s'", dir, fName);
      return IK_Moke;
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
	  return IK_Not;
	}
    }

  if (!dir || wantedName)
    Verbose ("found keyboard `%s' (%s)", fName, devName);

  return wantedName ? IK_Matched : IK_OK;
}

// Find and open the keyboard, return an fd or -1 on failure.
// @parm(wanted) either filename in input dir, or name fragment.
// Fragment can be anchored at start with ^ or end with $, but it is
// not a regexp.
int FindKeyboard (char const *wanted)
{
  int fd = -1;
  int dirfd = open (inputDevDir, O_RDONLY | O_DIRECTORY);

  bool isPathname = wanted[wanted[0] == '.'] == '/';
  if (isPathname || !strchr (wanted, ' '))
    {
      fd = openat (dirfd, wanted, O_RDONLY, 0);
      if (fd < 0)
	{
	  if (isPathname)
	    Inform ("cannot open `%s': %m", wanted);
	}
      else if (IK_Not == IsKeyboard (fd, nullptr, wanted, nullptr))
	{
	  close (fd);
	  fd = -1;
	}
      isPathname = fd >= 0;
    }

  if (DIR *dir = fdopendir (dirfd))
    {
      // Scan the directory looking for a keyboard and checking we're
      // not already installed.
      bool ok = true;

      while (struct dirent const *ent = readdir (dir))
	if (ent->d_type == DT_CHR)
	  {
	    // We do want to block reading this!
	    int probe = openat (dirfd, ent->d_name, O_RDONLY, 0);
	    if (probe >= 0)
	      {
		switch (IsKeyboard (probe, inputDevDir, ent->d_name,
				    isPathname ? nullptr : wanted))
		  {
		  case IK_Not:
		    break;
		  case IK_Moke:
		    // We're already running
		    ok = false;
		    break;
		  case IK_OK:
		    break;
		  case IK_Matched:
		    if (fd >= 0)
		      {
			Inform ("multiple devices found"
				" (use a more specific name?)");
			ok = false;
		      }
		    else
		      fd = probe;
		    break;
		  }

		if (probe != fd)
		  close (probe);
	      }
	  }

      if (!ok)
	{
	  close (fd);
	  fd = -2;
	}

      closedir (dir);
    }
  else
    Inform ("cannot open %s: %m", inputDevDir);

  return fd;
}

// Verify keyboard has the keys we need.
bool InitKeyboard (int fd)
{
  // Try grabbing it to check no one else is
  int rc = ioctl (fd, EVIOCGRAB, reinterpret_cast<void *> (1));
  ioctl (fd, EVIOCGRAB, reinterpret_cast<void *> (0));
  if (rc < 0)
    {
      Inform ("keyboard is grabbed by another process");
      return false;
    }
  
  ul_t bits[(KEY_CNT + ulBits - 1) / ulBits];
  ioctl(fd, EVIOCGBIT(EV_KEY, KEY_CNT), bits);
  for (unsigned ix = numButtons; ix--;)
    for (unsigned jx = 0; jx != 2; jx++)
      if (auto key = (&mapping[ix].key)[jx])
	if (!TestBit (bits, key))
	  {
	    Inform ("keyboard does not generate key %d (%s)",
		    key, KeyName (key));
	    return false;
	  }

  // FIXME: can/should we disable the modifierness/repeatness of the key?
  return true;
}

int InitDevice (char const *name)
{
  int fd = open (name, O_WRONLY | O_NONBLOCK);
  if (fd < 0)
    Inform ("cannot open output `%s': %m", name);
  else if (ioctl (fd, UI_SET_EVBIT, EV_KEY) < 0)
    {
    fail:
      Inform ("cannot initialize `%s': %m", name);
      close (fd);
      fd = -1;
    }
  else
    {
      for (unsigned ix = numButtons; ix--;)
	if (ioctl(fd, UI_SET_KEYBIT, mapping[ix].mouse) < 0)
	  goto fail;
      for (unsigned ix = KEY_CNT; ix--;)
	if (keyState[ix] && ioctl(fd, UI_SET_KEYBIT, ix) < 0)
	  goto fail;

      uinput_user_dev udev;
      memset (&udev, 0, sizeof (udev));
      snprintf (udev.name, UINPUT_MAX_NAME_SIZE, deviceName);
      udev.id.bustype = BUS_VIRTUAL;
      udev.id.vendor  = 21324; // Julian Day 2021-11-20
      udev.id.product = 0x1;
      unsigned vmaj, vmin;
      sscanf (PROJECT_VERSION, "%u.%u", &vmaj, &vmin);
      udev.id.version = vmaj * 1000 + vmin;

      if (write (fd, &udev, sizeof (udev)) < 0)
	goto fail;

      if (ioctl (fd, UI_DEV_CREATE) < 0)
	goto fail;
    }

  return fd;
}

bool Loop (int keyfd, int userfd)
{
  bool anyChanged = false;

  constexpr unsigned maxInEv = 8;
  input_event eventsIn[maxInEv];

  for (;;)
    {
      int cnt = read (keyfd, eventsIn, sizeof (eventsIn));
      if (cnt < 0)
	{
	  Inform ("error reading keyboard: %m");
	  return false;
	}
      for (auto const *ev = eventsIn; cnt; ev++)
	{
	  cnt -= sizeof (*ev);
	  if (cnt < 0)
	    {
	      Inform ("unexpected byte count reading keyboard");
	      return false;
	    }

	  if (ev->type == EV_KEY)
	    {
	      unsigned code = ev->code;
	      if (ev->code < KEY_CNT && keyState[code])
		{
		  int state = (ev->value ? +1 : -1);
		  if (state != keyState[code])
		    {
		      anyChanged = true;
		      keyState[code] = state;
		    }
		}
	    }
	  else if (ev->type == EV_SYN)
	    {
	      if (ev->code == SYN_DROPPED)
		Inform ("dropped packets");
	      else if (ev->code == SYN_REPORT && anyChanged)
		{
		  constexpr unsigned maxOutEv = buttonHWM * 3 + 1;
		  input_event eventsOut[maxOutEv];

		  auto *out = eventsOut;
		  unsigned downMask = 0;
		  unsigned overrideMask = 0;
		  for (unsigned ix = 0; ix != numButtons; ix++)
		    {
		      // Add hystersis for buttons with modifiers.
		      bool down = (keyState[mapping[ix].key] >= 0)
			&& (!mapping[ix].mod
			    || mapping[ix].down
			    || (keyState[mapping[ix].mod] >= 0));

		      downMask |= unsigned (down) << ix;
		      if (mapping[ix].override && (down || mapping[ix].down))
			overrideMask |= 1 << mapping[ix].override;
		    }
		  downMask &= ~(overrideMask >> 1);

		  for (unsigned ix = 0; ix != numButtons; ix++)
		    {
		      bool down = (downMask >> ix) & 1;
		      if (down != mapping[ix].down)
			{
			  // This button has changed state.
			  Verbose ("mouse %d (%s) is %s", mapping[ix].mouse,
				   ButtonName (mapping[ix].mouse),
				   down ? "pressed" : "released");
			  mapping[ix].down = down;
			  *out = *ev;
			  out->type = EV_KEY;
			  out->code = mapping[ix].mouse;
			  out->value = down;
			  out++;
			  if (down)
			    {
			      // Hide the keys from downstream
			      // consumers.
			      *out = *ev;
			      out->type = EV_KEY;
			      out->code = mapping[ix].key;
			      out->value = 0;
			      out++;
			      if (mapping[ix].mod)
				{
				  *out = *ev;
				  out->type = EV_KEY;
				  out->code = mapping[ix].mod;
				  out->value = 0;
				  out++;
				}
			    }
			}
		    }

		  if (out != eventsOut)
		    {
		      *out++ = *ev;
		      int bytes = (out - eventsOut) * sizeof (*out);
		      int cout = write (userfd, eventsOut, bytes);
		      if (cout != bytes)
			Inform ("unexpected byte count writing");
		    }

		  anyChanged = false;
		}
	    }
	}      
    }

  return true;
}

void Usage (FILE *stream = stderr)
{
  fprintf (stream, R"(Moke: Mouse Buttons From Keyboard
  Usage: %s [OPTIONS] [KEYBOARD] [DEVICE]

Use the keyboard to emit mouse keys, for when your laptop has no
buttons on its trackpad.

KEYBOARD defaults to a device in `%s' that reports key events and
whose reported name ends with ` keyboard'. You may provide either a
pathname (absolute or relative to %s), or a string to partially match
the reported device name. Use `^' and `$' to anchor the string at the
beginning and/or end of the reported name. (evtest and xinput can be
used to locate keyboard names.

DEVICE defaults to `%s', which will usually create a pseudo device in
`%s', which is automatically found by the X server (even after the X
server has started).

Options:
  -h	   Help
  -l KEYS  Keys for left
  -m KEYS  Keys for middle
  -r KEYS  Keys for right
  -v	   Be verbose

KEYS names a main key and an optional modifier key (prefixed with
`+'). Only a small subset of keys are supported -- the 'windows' key
and left or right ctrl or alt keys.  When a mouse button is emulated,
the keyboard keys are supressed -- so the mouse button doesn't appear
to be ALT+Button itself, for instance.  A mouse button can be
generated from more than one key combination.  If no buttons are
specified, the default mapping is:

   -l Windows -m Windows+LeftAlt -m RightCtrl+RightAlt -r RightCtrl

Known keys are)", progName, inputDevDir, inputDevDir, uinputDev, inputDevDir);
  for (unsigned ix = 0; keys[ix].name; ix++)
    fprintf (stream, "%s %s", &","[!ix], keys[ix].name);

  fprintf (stream, R"(.

Usually requires root privilege, as we muck about in /dev.
)");
  fprintf (stream, "\nVersion %s.\n", PROJECT_NAME " " PROJECT_VERSION);
  if (PROJECT_URL[0])
    fprintf (stream, "See %s for more information.\n", PROJECT_URL);
}
}

int main (int argc, char *argv[])
{
  uid_t uid = getuid ();
  uid_t euid = geteuid ();
  bool issetuid = uid != euid;

  if (issetuid)
    seteuid (uid);
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
      auto *arg = argv[argno];
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
	  struct Opts 
	  {
	    unsigned short button;
	    char opt[2];
	  };
	  static Opts const opts[] =
	    {
	      {BTN_LEFT, {'-', 'l'}},
	      {BTN_MIDDLE, {'-', 'm'}},
	      {BTN_RIGHT, {'-', 'r'}},
	      {0, {0, 0}},
	    };
	  for (unsigned ix = 0; opts[ix].button; ix++)
	    if (!strncmp (arg, opts[ix].opt, 2))
	      {
		char *opt = arg + 2;
		if (!*opt)
		  {
		    if (argno + 1 == argc)
		      {
			Inform ("option `%s' requires an argument", arg);
			return 1;
		      }
		    opt = argv[++argno];
		  }
		if (!ParseMapping (opts[ix].button, opt))
		  return 1;
		goto found;
	      }

	  Inform ("unknown flag `%s'", arg);
	  Usage ();
	  return 1;
	found:;
	}
    }

  if (issetuid)
    Verbose ("operating as setuid %u", unsigned (euid));

  if (!InitMapping ())
    return 1;

  char const *keyboard = keyboardName;
  if (argno < argc)
    keyboard = argv[argno++];
  char const *device = uinputDev;
  if (argno < argc)
    device = argv[argno++];

  if (argno != argc)
    {
      Inform ("unknown argument `%s'", argv[argno]);
      Usage ();
      return 1;
    }

  if (issetuid)
    // get privileges back
    seteuid (euid);

  int keyFd = FindKeyboard (keyboard);
  if (keyFd < 0)
    {
      if (keyFd == -1)
	{
	  bool usingDefault = keyboard == keyboardName;
	  Inform (usingDefault ? "cannot find keyboard%s%s"
		  : "cannot find keyboard `%s'%s",
		  usingDefault ? "" : keyboard,
		  geteuid () ? " (not root, sudo?)" : "");
	}
      return 1;
    }

  int devFd = -1;

  if (InitKeyboard (keyFd))
    devFd = InitDevice (device);

  if (issetuid)
    // and drop them again
    seteuid (uid);

  if (devFd >= 0)
    {
      Loop (keyFd, devFd);

      close (devFd);
    }
  close (keyFd);

  return 0;
}
