# V2.9.45 — Dashboard transport root-cause fix

- Removes the V2.9.44 `/__asset` transport-shell path.
- The V2.9.44 failure was caused by passing PROGMEM addresses into ordinary `beginResponse()` on `/__asset`.
- Create Account, Login, and Logout pages use direct String responses.
- Dashboard gzip and JPEG assets use `AsyncProgmemResponse` (flash-aware) with exact lengths.
- Dashboard HTML/CSS/JS/assets are preserved unchanged.
- Wi-Fi credentials/configuration are preserved from the V2.9.44 source; no replacement network is invented.