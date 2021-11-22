# Moke
Mouse from Keyboard

Copyright (C) 20201 Nathan Sidwell, nathan@acm.org

I purchased an XPS 13 9310, which is a laptop whose touchpad has no
separate physical mouse buttons.  Clicking the touchpad is how you get
LeftButton.  That's problematic because:

* It needs quite a forceful click, which is tiring

* To get MiddleButton & RightButton one has to enable multitouch,
  which is quite problematic.

However, the keyboard has a 'windows' key, which I never use (it's
treated as Left-Meta in Linux, but still not used by me).  That's
conveniently situated just above the touchpad, so why not use that as
Left-Mouse?  I can't just use the similarly convenient Left- and
Right-Alt for the other two mouse buttons, as Alt itself is needed.
But Windows+LeftAlt and Windows+RightAlt seem a useful combination.

Remapping those keys using the keymap wouldn't quite work -- UIs would
still think the Alt pressed on the combinations, and as it ended up I
found RightCtrl useful as a key too.

Hence Moke.  This reads from the keyboard device and creates a user
input device that is found by the X server.

# Usage


# Default

# SetUid

