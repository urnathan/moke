[//]: grip
# Moke

Mouse/Keyboard: Emulate Mouse Buttons Using Keyboard

Copyright (C) 2021 Nathan Sidwell, nathan@acm.org

Do you wish your buttonless touch pad had buttons? Do you use Linux?
This is the widget for you.

## Backstory

I purchased an XPS 13 9310<a href="#0"><sup>0</sup></a>, which is a
laptop whose touchpad has no separate physical mouse buttons. Clicking
the touchpad is how you get LeftButton. That's problematic because:

* It needs quite a forceful click, which is tiring and significantly
  different from the keyboard force.

* The pressure needed increases towards the edge of the touchpad, or
  even forces a button release. There's no visible clues on the
  trackpad that its useable area is less than its physical size.

* It's very hard to release the button without also altering the
  location &mdash; it's infuriating to carefully size a window, and then
  have it change when releasing the button ☹

* During a drag, if you hit the edge of the touchpad, it's
  _impossible_ to resite your finger and continue dragging.

* To get MiddleButton & RightButton one has to enable multitouch,
  which is quite confusing and difficult to get exactly right.

All in all, not good. A clear indicator that I would not get on with
macbooks.

However, the keyboard has a 'windows' key, which is conveniently
situated just above the touchpad. Plus I never use it as the LeftMeta
that it is treated as in Linux. So why not use that as LeftButton?  I
can't just use the similarly convenient Left- and RightAlt keys for
the other two mouse buttons, as Alt itself is needed.  But chords of
Windows+LeftAlt and Windows+RightAlt seem useful combinations.

Remapping those keys using the keymap wouldn't quite work.  I need to
distinguish the different Alt keys, and UIs would still think the Alt
was pressed when using them in a chord (fortunately that's
functionality I very rarely, if ever, need).  As it happens I ended up
using RightCtrl as the first key of some combinations, which is
orthogonal to its use as a keypress modifier key.

Hence Moke. This reads the keyboard device and generates mouse button
events on a created user input device, which is dynamically found by
the X server.  It actually grabs the keyboard and proxys its key
events too, after filtering out those that activate the emulated mouse
buttons.

## Example

In my `.zlogin` the following appears in the laptop-specific fragment:

```shell
moke &
```

Because Moke's defaults work for me (funny heh?), that finds the
laptop's keyboard and creates a Moke device, which is subsequently
picked up when the X server starts.

`xinput` shows the keyboard and Moke device:

```shell
> xinput
⎡ Virtual core pointer                    	id=2	[master pointer  (3)]
...
⎜   ↳ Moke proxying AT Translated Set 2 keyboard	id=17	[slave  pointer  (2)]
⎣ Virtual core keyboard                   	id=3	[master keyboard (2)]
    ...
    ↳ AT Translated Set 2 keyboard            	id=15	[slave  keyboard (3)]
    ↳ Moke proxying AT Translated Set 2 keyboard   	id=18	[slave  keyboard (3)]

```

`evtest` can also find it:
```shell
> sudo evtest
No device specified, trying to scan all of /dev/input/event*
Available devices:
...
/dev/input/event2:	AT Translated Set 2 keyboard
...
/dev/input/event6:	Moke proxying AT Translated Set 2 keyboard
```

## Usage

```shell
moke [OPTIONS] [KEYBOARD]
```

KEYBOARD is either a pathname or a partial string match of the
keyboard device name:

* Non-absolute pathnames are relative to `/dev/input`.

* When name matching, the `/dev/input` directory is scanned looking
for exactly one EVIO keyboard device that matches the partial
name. Devices that do not report keys [A-Z] are not considered
keyboards. The partial name can be anchored to the start of a device
name with `^`, and anchored to the end with `$` &mdash; but it is
_not_ a regexp.  Use both `^` and `$` to force an exact match.

* A heuristic is used to distinguish pathnames from partial names. If
KEYBOARD begins with `/` or `./`, it is only considered a
pathname. Otherwise, if it contains a space, it is only considered a
partial string.  Otherwise an attempt is made as a pathname, and if
that fails it is treated as a partial name.

The OPTIONS are:

* `-h` Help text.

* `-l` Keys for LeftButton.

* `-m` Keys for MiddleButton.

* `-r` Keys for RightButton.

* `-v` Be verbose.  Provides helpful diagnostics about device names
  and mouse button emulation.

A key combination is either one or two key names, separated by a `+`.
If a the first key of a two-key chord is also the single key for
another mouse button, the chord will take priority.  There is also
hysteresis for chords so the second key does not need to remain
pressed for the duration of the emulated mouse button press.  In
addition to emulating mouse buttons, the activating keys are emulated
as released. This allows an Alt key to participate in a mouse button
chord, but not cause applications to consider Button+Alt as being
pressed.  If you need Button+Alt functionality you will need to also
press the other Alt button.

Different key combinations can be mapped to the same mouse button.

Moke only knows the names and aliases of a few keys, there's no point
allowing full generality here: Windows, LeftAlt, RightAlt, LeftCtrl,
RightCtrl, LeftMeta, Alt_L, Ctrl_L, Super_L, Alt_R, Ctrl_R.

## Defaults

If no KEYBOARD argument is provided, a default of ` keyboard$` is
used. As explained above this is anchored to the end of the device's
name.<a href="#1"><sup>1</sup></a> To provide an empty partial name,
and avoid the default, use an empty string for the argument (`''`). As
Moke also requires the keyboard to emit alphabetic keys, that can be
sufficient.

If no `-l`, `-m` or `-r` options are given, a default mapping is
provided:

* `-l Windows` (LeftButton)
* `-m Windows+LeftAlt` (MiddleButton)
* `-m RightCtrl+RightAlt` (MiddleButton)
* `-r RightCtrl` (RightButton)

These mappings mean the RightCtrl key is not otherwise useable.<a
href="#2"><sup>2</sup></a> Notice that the chords needed for
MiddleButton can be done with one hand &mdash; leaving the other to
operate the touchpad itself.

## Errors

Moke's error messages should be clear enough.  Here are some of the checks:

* It does not permit more than one instance of the Moke device.

* If it cannot find a device and is not running as root, it give you a
  reminder.

* It checks that the keyboard device is capable of generating the key
  presses needed for mouse emulation.

* It makes sure that exactly one device matches the partial name.  As
  the iteration ordering is unspecified you cannot rely on first-find
  behavior.

* It does not permit a chord's second key to be the first key of
  another combination.

## Setuid

Both the `/dev/uinput` device and `/dev/input` directory are not
useable by non-root users. Thus `moke` is expected to be installed
setuid (`chmod u+s moke`). For this to be effective, either install as
root, or do a bit of manual `chown`/`chmod` alteration after
installing.

When Moke detects it is running with setuid priviledges, it drops them
as soon as possible.

## Implementation

Moke is syntactically C++, but dynamically C. In other words it only
needs the C library and does not link against the C++ runtime. From
CMake's PoV it _is_ C and I rely on compiler options to tell it to
compile as unexceptional C++17.

Moke was slightly more complicated than I'd originally hoped. We have
to proxy all the keys from the keyboard, so that we can filter out key
press events and insert key release events for the modifier keys that
are used to activate mouse buttons.  The input core maintains
per-device state for pressed keys, and uses that to remove (say)
release events on a not-pressed key.  So we can't just inject releases
on a new input device.  That's also why we grab the keyboard &mdash; we
don't want its key events making it to other downstream consumers.

---

<a name="0">0</a>: In case you're wondering, I found the following
BIOS settings necessary to allow suspend to work (s2idle only, deep
sleep is not supported). Disable 'Signs of Life' and 'Wake on Lan'.  I
also set the SSD to AHCI (from RAID), which has also been reported as
necessary.

<a name="1">1</a>: All my laptops report their keyboard as `AT
Translated Set 2 keyboard`, and the final word is sufficiently unique.

<a name="2">2</a>: I use a remapped CapsLock key for the Ctrl
modifier, so no loss there.