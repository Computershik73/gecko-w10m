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
