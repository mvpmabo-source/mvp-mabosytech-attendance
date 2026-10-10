# V2.9.45 — Dashboard transport root-cause fix

The V2.9.44 browser failure came from the `/__asset` handler passing PROGMEM
pointers to the ordinary RAM response path. The transport shell is removed.
Small first-boot/login/logout pages are sent directly as String responses, while
large dashboard and JPEG assets use ESPAsyncWebServer's flash-aware
`AsyncProgmemResponse` with explicit byte lengths.

The dashboard UI files themselves are unchanged.