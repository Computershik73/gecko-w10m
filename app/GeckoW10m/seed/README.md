# Startup cache seed

Untracked. Drop here, from a device that ran the package this engine build
went into: `scriptCache.bin`, `urlCache.bin`, `startupCache.4.little` (from
`LocalState\profile\startupCache\`) and the profile's `compatibility.ini`.
tools/build-appx.sh ships them only when the build id in compatibility.ini
matches the staged engine; the shell seeds a brand-new profile from them.
