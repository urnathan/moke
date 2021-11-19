// Wakame - Windows+Alt Keys As Mouse Emulation
// Copyright (C) 2021 Nathan Sidwell, nathan@acm.org
// License: Affero GPL v3.0

// Although this is C++, we're only using it for syntax and staying in
// the C subset of semantics.

#include "wakamecfg.h"
// C
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
auto const &UinputDev = "/dev/uinput";
auto const &InputDevDir = "/dev/input";
auto const &KeyboardName = "AT Translated Set 2 keyboard";

// FIXME: think about how to make this more flexible
unsigned const mouseKey = KEY_LEFTMETA;
unsigned const mouseMiddleAlt = KEY_LEFTALT;
unsigned const mouseRightAlt = KEY_RIGHTALT;
}

namespace
{
bool verbose = false;
}

namespace 
{
bool TestBit (unsigned long const *bits, unsigned n)
{
  return (bits[n / (sizeof (unsigned long) * 8)]
	  >> (n & ((sizeof (unsigned long) * 8) - 1))) & 1;
}

int FindKeyboard ()
{
  int fd = -1;
  int dirfd = open (InputDevDir, O_RDONLY | O_DIRECTORY);
  if (DIR *dir = fdopendir (dirfd))
    {
      while (struct dirent const *ent = readdir (dir))
	if (ent->d_type == DT_CHR)
	  {
	    fd = openat (dirfd, ent->d_name, O_RDONLY | O_NONBLOCK, 0);
	    if (fd >= 0)
	      {
		// Interrogate
		printf ("file:%s", ent->d_name);
		char name[256];
		ioctl (fd, EVIOCGNAME (sizeof (name)), name);
		printf (" name:%s", name);
		printf ("\n  ");

		int version;
		if (ioctl (fd, EVIOCGVERSION, &version) < 0)
		  printf ("unknown");
		else
		  {
		    printf (" version:%u.%u.%u", version >> 16,
			    (version >> 8) & 0xff, version & 0xff);

		    unsigned short id[4];
		    ioctl (fd, EVIOCGID, id);
		    printf (" ID: bus=0x%x, vendor=0x%x, product=0x%x,"
			    " version=0x%x", id[ID_BUS], id[ID_VENDOR],
			    id[ID_PRODUCT], id[ID_VERSION]);
		    printf ("\n  ");

		    ioctl (fd, EVIOCGPHYS (sizeof (name)), name);
		    printf (" phys:%s", name);
		    printf ("\n  ");

		    printf ("props: ");
		    unsigned long propbits;
		    ioctl (fd, EVIOCGPROP (INPUT_PROP_CNT), &propbits);
		    for (unsigned ix = 0; ix != INPUT_PROP_CNT; ix++)
		      {
			if (!(ix & 7) && ix)
			  printf (",");
			printf ("%d", unsigned (propbits >> ix) & 1);
		      }
		    printf ("\n  ");

		    printf ("types: ");
		    unsigned long typebits;
		    ioctl (fd, EVIOCGBIT (0, EV_CNT), &typebits);
		    for (unsigned type = 0; type != EV_CNT; type++)
		      {
			if (!(type & 7) && type)
			  printf (",");
			printf ("%d", unsigned (typebits >> type) & 1);
		      }
		    if (typebits & (1 << EV_KEY))
		      {
			printf ("\n ");
			unsigned long keybits[KEY_CNT
					      / sizeof (unsigned long) / 8];
			ioctl (fd, EVIOCGBIT (EV_KEY, KEY_CNT), keybits);
			if (TestBit (keybits, mouseKey))
			  printf (" mouseKey");
			if (TestBit (keybits, mouseMiddleAlt))
			  printf (" mouseMiddleAlt");
			if (TestBit (keybits, mouseMiddleAlt))
			  printf (" mouseMiddleAlt");
		      }
		  }
		printf ("\n");

		close (fd);
		fd = -1;
	      }
	  }
      closedir (dir);
    }
  else if (dirfd >= 0)
    close (dirfd);
  return fd;
}

}


int main (int, char *[])
{
  int keyFd = FindKeyboard ();
  if (keyFd < 0)
    return 1;
  
  close (keyFd);

  return 0;
}
