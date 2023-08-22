** A small Pic32 NVM library
Heavily based on microchip code (no idea from which library or example) with some additions. Supports page caching, but requires reads from that memory to be done with the internal NVM_memcpy4 function.

Writes are flushed only when another page is written to or NVM_flush is called. Keep this in mind when writing settings etc, you will loose at least the last write if you don't do this manually.

Documentation is also lacking...

