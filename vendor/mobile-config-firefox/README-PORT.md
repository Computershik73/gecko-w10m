# mobile-config-firefox, vendored

Source: https://gitlab.postmarketos.org/postmarketOS/mobile-config-firefox
(mirror https://github.com/hamed7ir is not involved; this is the upstream
postmarketOS project), commit f344755, MPL-2.0 (see LICENSE).

What is used: `modules/`, `themes/`, `policies.json` and
`mobile-config-autoconfig.js` (with one change: the chrome.manifest is found
under the engine's directory in the package instead of /usr/lib). The prefs
from `mobile-config-prefs.js` are folded into tools/build-appx.sh, which also
stages these files: the autoconfig script at the package root, the modules and
themes under `mobile-config-firefox/`, and policies.json under
`browser/distribution/`.

## The mobile breakpoint

Every `@media (max-width: 700px)` in `themes/` is `1200px` here. Upstream runs
on postmarketOS phones whose browser window is a few hundred CSS pixels wide;
this one draws at 2.666667 device pixels per CSS pixel, so its window is 540
CSS pixels in portrait and **839 in landscape**. Past 700, all of the mobile
chrome switched itself off the moment the phone was turned on its side --
the address bar went back to the top, the tab menu came back, the whole
desktop layout. 1200 is above anything this display can produce either way up.

## Popup notifications

The theme makes every panel the width of the screen and, for doorhangers
(`.popup-notification-panel`: microphone and camera prompts, add-on installs
and the note after them), pulls them up by `-100vh` and stretches them to the
screen's height.
On postmarketOS the Wayland compositor then puts the popup where it fits.
Here Gecko places popups itself, against the screen, and the result was a
prompt cut down to the room right of its icon in the address bar, running a
few pixels past the right edge (where the Allow button is), pinned to the top
of the screen, with a transparent area over the page that swallowed taps.

Two port changes undo that:

- `themes/shared/chrome/popups.css`, at the end of the mobile block:
  doorhangers get no `-100vh` offset, no minimum height, paint containment
  only and no input-region margin, so Gecko flips them to sit right above the
  bar, as tall as their content.
- In the engine (`nsMenuPopupFrame.cpp`, `FlipFromAttribute`, under
  GECKO_W10M): an arrow panel flipped "both" -- panel.js's default -- slides
  along the screen instead of being shrunk to the space beside its anchor.
  It is done there rather than by setting `flip="slide"` from the autoconfig
  script because some panels (form validation, date and colour pickers) are
  put in and opened in one go, before any script could change them.
