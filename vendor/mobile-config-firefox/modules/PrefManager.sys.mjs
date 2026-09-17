// Copyright 2025 Danny Colin
// SPDX-License-Identifier: MPL-2.0

// TODO:
// - Use PoliciesUtils and Service.prefs.* instead of our own module
// https://searchfox.org/mozilla-central/source/browser/components/enterprisepolicies/Policies.sys.mjs#2808
export class PrefManager {
  static pref(prefName, value) {
    try {
      var prefBranch = getPrefBranch();

      if (typeof value == "string") {
        if (gIsUTF8) {
          prefBranch.setStringPref(prefName, value);
          return;
        }
        prefBranch.setCharPref(prefName, value);
      } else if (typeof value == "number") {
        prefBranch.setIntPref(prefName, value);
      } else if (typeof value == "boolean") {
        prefBranch.setBoolPref(prefName, value);
      }
    } catch (e) {
      console.error(e);
    }
  }

  static defaultPref(prefName, value) {
    try {
      const prefBranch = Services.prefs.getDefaultBranch(null);
      if (typeof value == "string") {
        const gIsUTF8 = true;
        if (gIsUTF8) {
          prefBranch.setStringPref(prefName, value);
          return;
        }
        prefBranch.setCharPref(prefName, value);
      } else if (typeof value == "number") {
        prefBranch.setIntPref(prefName, value);
      } else if (typeof value == "boolean") {
        prefBranch.setBoolPref(prefName, value);
      }
    } catch (e) {
      console.error(e);
    }
  }

  static getPref(prefName) {
    try {
      var prefBranch = getPrefBranch();

      switch (prefBranch.getPrefType(prefName)) {
        case prefBranch.PREF_STRING:
          if (gIsUTF8) {
            return prefBranch.getStringPref(prefName);
          }
          return prefBranch.getCharPref(prefName);

        case prefBranch.PREF_INT:
          return prefBranch.getIntPref(prefName);

        case prefBranch.PREF_BOOL:
          return prefBranch.getBoolPref(prefName);
        default:
          return null;
      }
    } catch (e) {
      console.error(e);
    }
    return undefined;
  }
};
