A simple implementation of the Linux kernel's [RCU](https://docs.kernel.org/RCU/whatisRCU.html) synchronization primitive in userspace, using the `membarrier` system call to keep readers very lightweight.

This is a **toy** implementation and shouldn't be used in production: it is only very lightly tested, doesn't check for errors everywhere it probably should, and scales poorly with many updaters. See [liburcu](https://liburcu.org/) if you need a real implementation.
