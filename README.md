# Moke

Mouse/Keyboard: Emulate Mouse Buttons Using Keyboard

Copyright (C) 2021 Nathan Sidwell, nathan@acm.org

Do you wish your buttonless touch pad had buttons? This is the widget
for you.

# Backstory

I purchased an XPS 13 9310, which is a laptop whose touchpad has no
separate physical mouse buttons. Clicking the touchpad is how you get
LeftButton. That's problematic because:

* It needs quite a forceful click, which is tiring and ...

* The pressure needed increases towards the edge of the touchpad, or
  isn't even possible. There's no visible clues on the trackpad that
  the useable area is less than its physical size.

* It's very hard to release the button without altering location --
  the number of times sized a window as I wanted it, and then have it
  change when releasing :frowningface:☹

* During a drag, if you hit the edge of the touchpad, it's
  _impossible_ to resite your finger and continue dragging.

* To get MiddleButton & RightButton one has to enable multitouch,
  which is quite problematic.

All in all, not good. A clear indicator that I would not get on with
macbooks.

However, the keyboard has a 'windows' key, which I never use (it's
treated as LeftMeta in Linux, but still not used by me). That's
conveniently situated just above the touchpad, so why not use that as
LeftButton?  I can't just use the similarly convenient Left- and
RightAlt for the other two mouse buttons, as Alt itself is needed.
But Windows+LeftAlt and Windows+RightAlt seem a useful combination.

Remapping those keys using the keymap wouldn't quite work.  I needed
to separate different Alt and Ctrl keys, and UIs would still think the
Alt or Ctrl was pressed when using them in a chord.  Plus I ended up
using RightCtrl as a non-modifier mouse key, which is orthogonal to
its use as a regular modifier key.

Hence Moke. This reads from the keyboard device and generates mouse
button events on a created user input device that is found by the X
server.

# Example

I have the following in my .zlogin:
```shell
moke &
```

Because Moke's defaults work for me (funny heh?), that finds the
laptop's keyboard and creates a Moke device, which is subsequently
picked up when the X server starts.

`xinput` shows the keyboard and moke device:

```shell
> xinput   
...
⎣ Virtual core keyboard                   	id=3	[master keyboard (2)]
    ...
    ↳ AT Translated Set 2 keyboard            	id=15	[slave  keyboard (3)]
    ↳ Moke Key to Button Mapper               	id=18	[slave  keyboard (3)]

```

`evtest` can also find it:
```shell
>sudo evtest
No device specified, trying to scan all of /dev/input/event*
Available devices:
...
/dev/input/event2:	AT Translated Set 2 keyboard
...
/dev/input/event17:	Moke Key to Button Mapper
```

# Usage
```shell
moke [OPTIONS] [KEYBOAD]
```

KEYBOARD is either a pathname or a partial string match of the
keyboard device name. As this can be ambiguous as to which is which,
and the following heuristic is used. If KEYBOARD begins with `/` or
`./`, it is a pathname. If it contains a space, it is a partial
string.  Otherwise an attempt is made to open it and if that fails it
is treated as a partial name.

File opening is relative to `/dev/input`.

For find-by name, the `/dev/input` directory is scanned looking for
exactly one EVIO keyboard device that matches the partial name.  The
partial name can be anchored to the start of a device name with `^`,
and anchored to the end with `$` -- but it is _not_ a regexp.  Use `^`
and `$` to force an exact match.

The OPTIONS are:

* `-h` Help text.

* `-l` Keys for LeftButton.

* `-m` Keys for MiddleButton.

* `-r` Keys for RightButton.

* `-v` Be verbose.  Provides helpful diagnostics about device names
  and mouse button emulation.

A key combination is either a one or two key names, separated by a
`+`.  If a the first key of a two-key chord is also the single key for
another mouse button, the chord will take priority.  There is also
hysteresis for chords where the second key does not need to remain
pressed for the duration of the emulated mouse button press.  In
addition to emulating mouse buttons, the keys for that button are
emulated as released. This allows an Alt key to participate in a mouse
button chord, but not cause applications to consider you as pressing
Mouse+Alt.  If you need Mouse+Alt functionality you will need to also
press the other Alt button.

# Defaults

The default keyboard match used if no KEYBOARD argument is provided.
It is ` keyboard$`, which as explained above anchors the partial name
to the end of the device's name.  All my laptops report a keyboard
named `AT Translated Set 2 keyboard`, and the final word is
sufficiently unique. To provide an empty partial name, and avoid the
default, use an empty string for the argument (`''`).

If no `-l`, `-m` or `-r` options are given, a default mapping is
provided. It is:

* Windows -> LeftButton
* Windows+LeftAlt -> MiddleButton
* RightCtrl+RightAlt -> MiddleButton
* RightCtrl -> RightMouse

These mappings mean the RightCtrl key is not otherwise useable -- but
I remap the CapsLock key to Ctrl functionality.<a
href="#1"><sup>1</sup></a> The chords needed for MiddleButton can be
done with one hand -- leaving the other to operate the touchpad
itself.

# Errors

Moke's error messages should be clear enough.  It does not permit more
than one instance of the Moke device.  It reminds you if it is not
running as root, and cannot find a device.

# Setuid

Both the `/dev/uinput` device and `/dev/input` directory are not
useable by non-root users. Thus moke is expected to be installed
setuid (`chmod u+s moke`). For this to be effective, either install as
root, or do a bit of manual `chown`/`chmod` alteration after
installing.

When moke detects it is running with setuid priviledges, it drops them
as soon as possible.

--

<a name="1">1</a>: _Where it always should be._ Why do we still have
CapsLock anyway?
