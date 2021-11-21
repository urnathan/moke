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
char const *progName = "wakame";
bool flagVerbose = false;

enum KeyFlags : unsigned char 
{
  KF_Used = 1 << 0, // we are interested in this key
  KF_Down = 1 << 1, // this key is pressed
  KF_Changed  = 1 << 2, // this key changed state
};

unsigned char keyState[KEY_CNT];

struct Map
{
  unsigned mouse; // the mouse BTN to emit
  unsigned key;  // the keyboard KEY we want
  unsigned mod;  // keyboard modifier, if any
  char override; // overrides a non-modified button

  bool down; // whether we consider this pressed
};

auto const buttonHWM = 3;
unsigned numButtons = 0;
Map mapping[buttonHWM]
= {
  {BTN_LEFT, KEY_LEFTMETA, 0, 0, false},
  {BTN_MIDDLE, KEY_LEFTMETA, KEY_LEFTALT, 0, false},
  {BTN_RIGHT, KEY_LEFTMETA, KEY_RIGHTALT, 0, false},
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
  for (unsigned ix = numButtons; ix--;)
    if (button == mapping[ix].mouse)
      {
	Inform ("button %d (%s) is already specified",
		button, ButtonName (button));
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
      keyState[mapping[ix].key] = KF_Used;
      if (mapping[ix].mod)
	{
	  keyState[mapping[ix].mod] = KF_Used;
	  for (unsigned jx = numButtons; jx--;)
	    if (!mapping[jx].mod && mapping[jx].key == mapping[ix].key)
	      mapping[ix].override = jx + 1;
	}
    }

  return true;
}

// See if FD is the keyboard we want.  Must match wanted and accept
// key events.
int IsKeyboard (int fd, char const *fName, char const *wantedName = nullptr)
{
  int version;
  char devName[MAXNAMLEN];
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
		fd = openat (dirfd, ent->d_name, O_RDONLY/* | O_NONBLOCK*/, 0);
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

int InitUser (char const *name)
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
      snprintf (udev.name, UINPUT_MAX_NAME_SIZE,
		"Keyboard Virtual Mouse Buttons");
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
  unsigned changed[buttonHWM * 2];
  unsigned numChanged = 0;

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
	      if (ev->code < KEY_CNT && keyState[code] & KF_Used)
		{
		  if (!(keyState[code] & KF_Changed))
		    changed[numChanged++] = code;
		  keyState[code] = ((ev->value ? KF_Down : 0)
				    | KF_Used | KF_Changed);
		}
	    }
	  else if (ev->type == EV_SYN)
	    {
	      if (ev->code == SYN_DROPPED)
		Inform ("dropped packets");
	      else if (ev->code == SYN_REPORT && numChanged)
		{
		  constexpr unsigned maxOutEv = buttonHWM * 3 + 1;
		  input_event eventsOut[maxOutEv];

		  auto *out = eventsOut;
		  unsigned downMask = 0;
		  unsigned overrideMask = 0;
		  for (unsigned ix = 0; ix != numButtons; ix++)
		    {
		      // Add hystersis for buttons with modifiers.
		      bool down = (keyState[mapping[ix].key] & KF_Down)
			&& (!mapping[ix].mod
			    || mapping[ix].down
			    || (keyState[mapping[ix].mod] & KF_Down));

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

		  // Reset the changed flags on the changed keys
		  while (numChanged--)
		    keyState[changed[numChanged]] &= ~KF_Changed;
		  numChanged = 0;
		}
	    }
	}      
    }

  return true;
}

void Usage (FILE *stream = stderr)
{
  fprintf (stream, R"(Keyboard Emulation of Mouse Buttons
  Usage: %s [OPTIONS] [INPUTKEYBOARD] [OUTPUTDEVICE]

Emulate mouse buttons to OUTPUTDEVICE using INPUTDEVICE.

MetaLeft (the window key) is the mouse button, with AltLeft and
AltRight modifying it to buttons 2 & 3.  FIXME: Parameterize this.

INPUTKEYBOARD defaults to a device in %s that reports key events and
whose reported name ends with ` keyboard'.  You may provide either a
pathname (absolute or relative to %s), or a string to partially match
the reported device name.  Use `^' and `$' to anchor the string at the
beginning and/or end of the reported name.

OUTPUTDEVICE defaults to %s.

Options:
  -h	  Help
  -l LEFT  Keys for left
  -m MIDDLE Keys for middle
  -r RIGHT Keys for right
  -v	  Be verbose

Usually requires root privilege, as we muck about in /dev.

)", progName, inputDevDir, inputDevDir, uinputDev);
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

  if (!InitMapping ())
    return 1;

  char const *keyboard = keyboardName;
  if (argno < argc)
    keyboard = argv[argno++];
  char const *user = uinputDev;
  if (argno < argc)
    user = argv[argno++];

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

  int userFd = -1;

  if (InitKeyboard (keyFd))
    userFd = InitUser (user);

  if (userFd >= 0)
    {
      Loop (keyFd, userFd);

      close (userFd);
    }
  close (keyFd);

  return 0;
}
