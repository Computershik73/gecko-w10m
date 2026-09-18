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
