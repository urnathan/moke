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
auto const &deviceName = "Moke proxying ";

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

struct DeviceInfo
{
  char name[UINPUT_MAX_NAME_SIZE];
  ul_t keyMask[(KEY_CNT + ulBits - 1) / ulBits];
};

// -1: wanted, not pressed
// +1: wanted, pressed
// 0: not wanted
signed char keyState[KEY_CNT];

struct Map
{
  unsigned short mouse; // the mouse BTN to emit
  unsigned short key;  // the keyboard KEY we want
  unsigned short mod;  // keyboard modifier, if any

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
  (flagVerbose ? Inform (fmt __VA_OPT__ (,) __VA_ARGS__) : void (0))

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

  if (!code)
    {
      Inform ("unknown key `%s'", opt);
      return false;
    }

  numButtons++;
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
enum IKC {IK_Not, IK_Moke, IK_OK, IK_Bad};

IKC IsKeyboard (DeviceInfo *info, int fd, char const *dir, char const *fName,
		char const *wantedName = nullptr)
{
  int version;
  if (ioctl (fd, EVIOCGVERSION, &version) < 0)
    {
    not_evio:
      if (!dir || wantedName)
	Verbose ("rejecting `%s': not an EVIO device", fName);
      return IK_Not;
    }

  char devName[UINPUT_MAX_NAME_SIZE];
  int sDevLen = ioctl (fd, EVIOCGNAME (sizeof (devName)), devName);
  if (sDevLen < 0)
    goto not_evio;

  ul_t typeMask = 0;
  if (ioctl (fd, EVIOCGBIT (0, EV_CNT), &typeMask) < 0)
    goto not_evio;

  // The name length includes the trailing NUL, check it
  unsigned devLen = unsigned (sDevLen);
  if (devLen > sizeof (devName) || !devLen || devName[devLen-1])
    {
      if (!dir || wantedName)
	Inform ("rejecting `%s': name badly formed", fName);
      return IK_Not;
    }
  devLen--; // Make it the usual len we care about

  if (dir && !strncmp (devName, deviceName, sizeof (deviceName) - 1))
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

  // Must generate EV_KEY and not generate non-keyboard-like events
  char const *whyNot = nullptr;
  if (!(typeMask & (1u << EV_KEY)))
    {
      whyNot = "does not generate Key events";
    not_keyboard:
      if (!dir || wantedName)
	Inform ("rejecting `%s' (%s): not a keyboard, %s", fName, devName,
		whyNot);
      return IK_Not;
    }
  if (typeMask & ~((1u << EV_KEY) | (1u << EV_SYN) | (1u << EV_MSC)
		   | (1u << EV_REP) | (1u << EV_LED)))
    {
      whyNot = "generates non-keyboard events";
      goto not_keyboard;
    }

  ul_t keyMask[(KEY_CNT + ulBits - 1) / ulBits];
  memset (keyMask, 0, sizeof (keyMask));
  if (ioctl (fd, EVIOCGBIT (EV_KEY, KEY_CNT), keyMask) < 0)
    goto not_evio;

  // Check some usual keyboard keys are generated
  static unsigned char const someKeys[]
    = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    0};
  for (unsigned char const *keyPtr = someKeys; *keyPtr; keyPtr++)
    if (!TestBit (keyMask, *keyPtr))
      {
	whyNot = "does not generate letter keys";
	goto not_keyboard;
      }

  if (!dir || wantedName)
    Verbose ("found keyboard `%s' (%s)", fName, devName);

  for (unsigned ix = numButtons; ix--;)
    for (unsigned jx = 0; jx != 2; jx++)
      if (auto key = (&mapping[ix].key)[jx])
	if (!TestBit (keyMask, key))
	  {
	    Inform ("keyboard `%s' (%s) does not generate %s (code %d)",
		     fName, devName, KeyName (key), key);
	    return IK_Bad;
	  }

  memcpy (info->name, devName, devLen + 1);
  memcpy (info->keyMask, keyMask, sizeof (info->keyMask));

  return IK_OK;
}

// Find and open the keyboard, return an fd or -1 on failure.
// @parm(wanted) either filename in input dir, or name fragment.
// Fragment can be anchored at start with ^ or end with $, but it is
// not a regexp.
int FindKeyboard (DeviceInfo *info, char const *wanted)
{
  int fd = -1;
  int dirfd = open (inputDevDir, O_RDONLY | O_DIRECTORY);
  bool ok = true;

  bool isPathname = wanted[wanted[0] == '.'] == '/';
  if (isPathname || (wanted[0] && !strchr (wanted, ' ')))
    {
      fd = openat (dirfd, wanted, O_RDONLY, 0);
      if (fd < 0)
	{
	  if (isPathname || flagVerbose)
	    Inform ("cannot open `%s': %m", wanted);
	}
      else
	{
	  auto is = IsKeyboard (info, fd, nullptr, wanted, nullptr);
	  if (is == IK_Not)
	    {
	      close (fd);
	      fd = -1;
	    }
	  else
	    {
	      if (is == IK_Bad)
		ok = false;
	      isPathname = true;
	    }
	}
    }

  if (DIR *dir = fdopendir (dirfd))
    {
      // Scan the directory looking for a keyboard and checking we're
      // not already installed.

      while (struct dirent const *ent = readdir (dir))
	if (ent->d_type == DT_CHR)
	  {
	    // We do want to block reading this!
	    int probe = openat (dirfd, ent->d_name, O_RDONLY, 0);
	    if (probe >= 0)
	      {
		auto is = IsKeyboard (info, probe, inputDevDir, ent->d_name,
				      isPathname ? nullptr : wanted);
		if (is == IK_Moke)
		  // We're already running
		  ok = false;
		else if (is != IK_Not && !isPathname)
		  {
		    if (is == IK_Bad)
		      ok = false;
		    if (fd >= 0)
		      {
			Inform ("multiple devices found"
				" (use a more specific name?)");
			ok = false;
		      }
		    else
		      fd = probe;
		  }

		if (probe != fd)
		  close (probe);
	      }
	  }
      closedir (dir);
    }
  else
    Inform ("cannot open %s: %m", inputDevDir);

  if (!ok)
    {
      close (fd);
      fd = -2;
    }

  return fd;
}

int InitDevice (int keyFd, DeviceInfo const *info, char const *name)
{
  int fd = open (name, O_WRONLY);
  if (fd < 0)
    {
    fail:
      Inform ("cannot %s output `%s': %m", fd < 0 ? "open" : "initialize", name);
      close (fd);
      return -1;
    }

  if (ioctl (fd, UI_SET_EVBIT, EV_KEY) < 0)
    goto fail;
  for (unsigned ix = numButtons; ix--;)
    if (ioctl (fd, UI_SET_KEYBIT, mapping[ix].mouse) < 0)
      goto fail;
  for (unsigned ix = KEY_CNT; ix--;)
    if (TestBit (info->keyMask, ix) && ioctl (fd, UI_SET_KEYBIT, ix) < 0)
      goto fail;

  uinput_user_dev udev;
  memset (&udev, 0, sizeof (udev));
#if __GNUC__ && !__clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
  // Yes, we know there might be overrun, that's why we're using snprintf
  snprintf (udev.name, sizeof (udev.name), "%s%s", deviceName, info->name);
#if __GCC__ && !__clang__
#pragma GCC diagnostic pop
#endif
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

  // We need to grab, as we're filtering keypresses.  Fortunately
  // we'll automatically ungrab when we terminate, by whatever
  // mechanism.
  int rc = ioctl (keyFd, EVIOCGRAB, reinterpret_cast<void *> (1));
  if (rc < 0)
    {
      Inform ("keyboard is grabbed by another process");
      close (fd);
      return -1;
    }

  return fd;
}

void Loop (int keyFd, int userFd)
{
  enum PKF {PK_None, PK_Changed, PK_Resync};
  auto flags = PK_None;

  constexpr unsigned maxInEv = 8;
  for (;;)
    {
      input_event events[maxInEv + buttonHWM];
      int bytes = read (keyFd, events, sizeof (events));
      if (bytes < 0)
	{
	  Inform ("error reading device: %m");
	  break;
	}

      auto *base = events, *ev = base, *ptr = base;

      for (auto *next = ev; bytes > 0; ev = next, bytes -= sizeof (*ev))
	{
	  next = ev + 1;
	  switch (ev->type)
	    {
	    default:
	      // Drop
	    elide:
	      if (ev == base)
		base = ptr = next;
	      break;

	    case EV_KEY:
	      {
		unsigned code = ev->code;

		if (ev->code < KEY_CNT && keyState[code] && flags != PK_Resync)
		  {
		    if (ev->value == 2)
		      goto elide;
		    else if (bool (ev->value) != (keyState[code] >= 0))
		      {
			flags = PK_Changed;
			keyState[code] = -keyState[code];
		      }
		  }

		if (ptr != ev)
		  *ptr = *ev;
		ptr++;
	      }
	      break;

	    case EV_SYN:
	      {
		unsigned changedMask = 0;
		if (ev->code == SYN_DROPPED)
		  {
		    flags = PK_Resync;
		    Inform ("dropped packets");
		    for (unsigned ix = KEY_CNT; ix--;)
		      if (keyState[ix])
			keyState[ix] = -1;
		  }
		else if (ev->code == SYN_REPORT && flags != PK_None)
		  {
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

		    changedMask = downMask;
		    for (unsigned ix = 0; ix != numButtons; ix++)
		      changedMask ^= unsigned (mapping[ix].down) << ix;
		    flags = PK_None;
		  }

		if (changedMask)
		  {
		    // A mouse button changed, figure out what to report
		    input_event bEvents[buttonHWM + 1];
		    unsigned numBE = 0;

		    for (unsigned ix = 0; ix != numButtons; ix++)
		      if (changedMask & (1 << ix))
			{
			  // This button has changed state.
			  bool down = !mapping[ix].down;
			  Verbose ("%s is %s", ButtonName (mapping[ix].mouse),
				   down ? "pressed" : "released");
			  mapping[ix].down = down;
			  bEvents[numBE] = *ev;
			  bEvents[numBE].type = EV_KEY;
			  bEvents[numBE].code = mapping[ix].mouse;
			  bEvents[numBE].value = down;
			  numBE++;

			  if (down)
			    // Unpress the activating keys
			    for (auto *probe = base; probe != ptr; probe++)
			      {
				unsigned key = mapping[ix].key;
				if (probe->code == key && probe->value)
				  {
				    probe->value = 0;
				    if (mapping[ix].mod)
				      probe->code = mapping[ix].mod;
				    break;
				  }
			      }
			}

		    // Write in one or two blocks
		    bEvents[numBE++] = *ev;
		    if (unsigned count = (reinterpret_cast<char *> (ptr)
					   - reinterpret_cast<char *> (base)))
		      {
			if (!bytes)
			  {
			    count += numBE * sizeof (bEvents[0]);
			    memcpy (ptr, bEvents, numBE * sizeof (bEvents[0]));
			    numBE = 0;
			  }
			write (userFd, base, count);
		      }

		    if (numBE)
		      write (userFd, bEvents, numBE * sizeof (bEvents[0]));
		  }
		else
		  {
		    if (ev != ptr)
		      *ptr = *ev;
		    ptr++;
		    unsigned count = (reinterpret_cast<char *> (ptr)
				      - reinterpret_cast<char *> (base));
		    write (userFd, base, count);
		  }
		base = ptr = next;
	      }
	    }
	}

      if (unsigned bytes = (reinterpret_cast<char *> (ptr)
			    - reinterpret_cast<char *> (base)))
	write (userFd, base, bytes);

      if (bytes < 0)
	Inform ("unexpected byte count reading keyboard");
    }

  return;
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

  int keyFd, devFd;
  {
    DeviceInfo info;
    keyFd = FindKeyboard (&info, keyboard);
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

    devFd = InitDevice (keyFd, &info, device);
  }
  
  if (issetuid)
    // and drop them again
    seteuid (uid);

  if (devFd >= 0)
    {
      Loop (keyFd, devFd);

      close (devFd);
    }

  ioctl (keyFd, EVIOCGRAB, reinterpret_cast<void *> (0));
  close (keyFd);

  return 0;
}
