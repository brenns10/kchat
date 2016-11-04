kchat
=====

This is a mildly terrifying *local* "chat" system that shirks networking
protocols in favor of sending all messages directly through the kernel using its
own kernel module. The idea is that user processes may open a file, and anything
they read from this file will be messages from the chat room, and anything they
write to this file will be broadcast to the entire chat room.

Interestingly enough, this turns out to be pretty darn close to a
producer/consumer IPC mechanism, and since it has no real "chat features" (it's
literally a byte pipe), this could conceivably even be used as a strange form of
IPC.

Try It!
-------

You'll need root on a Linux machine. I'm running 4.8.4, and it's entirely
possible that this won't compile correctly on older versions.

```bash
# Build kchat_mod.ko, the kernel module
$ make

# Build the c client (although you can use cat if you just want to read)
$ gcc -o kchat_client kchat_client.c

# Load the kernel module
$ sudo insmod kchat_mod.ko

# Check for the device number from the module's output
$ dmesg | tail
# Use that number in the next command

# Create a "chat server" (i.e. a device file)
$ sudo mknod chat c 242 0

# Now run your clients (each in a separate window)
$ sudo kchat_client chat

# At this point all your terminals are "chatting" with each other!
```

What's Cool About It?
---------------------

No really, I promise it's cool! If you don't believe me, check out the code for
the [client](kchat_client.c). It is really just reading, writing, and selecting
from a couple files. If you looked at it, you wouldn't believe that it was a
chat system!

References
----------

My original inspiration came from the Linux Kernel Module Programming Guide,
especially the chapter on character devices. Check that
out [here](http://www.tldp.org/LDP/lkmpg/2.6/html/lkmpg.html#AEN567).

I was able to competently use data structures and locking primitives within the
kernel thanks to *Linux Kernel Development, Third Edition* by Robert Love. If
you're interested at all in kernel development, buy it from Amazon and stick it
under your pillow at night.

I managed to hack together a good enough understanding of polling and wait
queues to make the files work with both blocking non-blocking I/O, thanks to the
information from the *Linux Device Drivers, 3rd Edition*, which I accessed
from [here](http://www.makelinux.net/ldd3/). Especially see Sections 6.2 and
6.3!

License
-------

As I normally do, this is released under the BSD 3 Clause license.
Check [LICENSE.txt]() for more info.
