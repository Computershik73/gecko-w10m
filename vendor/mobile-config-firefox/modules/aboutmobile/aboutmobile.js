/* Copyright 2025 Danny Colin
 * SPDX-License-Identifier: MPL-2.0 */

const {classes: Cc, interfaces: Ci, utils: Cu} = Components;

class PreferencesObserver {
    constructor() {
        this.init();
    }
    init() {
        const toggles = document.getElementsByTagName("moz-toggle");

        for (let toggle of toggles) {
            let isPrefEnabled = Services.prefs.getBoolPref(toggle.dataset.preference);

            if (isPrefEnabled) {
                toggle.setAttribute("pressed", "true");
            }
        }
    }
    observe(subject, topic, data) {
        const toggles = document.getElementsByTagName("moz-toggle");

        switch (topic) {
            case "nsPref:changed":
                for (let toggle of toggles) {
                    if (toggle.dataset.preference === data) {
                        let isPrefEnabled = Services.prefs.getBoolPref(
                            toggle.dataset.preference
                        );

                        (isPrefEnabled) ?
                            toggle.setAttribute("pressed", "true") :
                            toggle.removeAttribute("pressed");
                    }
                }
                break;
        }
        window.addEventListener("unload", () => {
            Services.prefs.removeObserver("", aboutMobilePrefObserver);
        }, { once: true });
    }
    // TODO:
    // - handle other form elements (e.g. input)
    handleClick(event) {
        const target = event.target;
        switch (target.dataset.preferenceType) {
            case "bool":
                Services.prefs.setBoolPref(
                    target.dataset.preference, target.pressed);
                break;
            case "number":
                Services.prefs.setIntPref(
                    target.dataset.preference, target.pressed);
                break;
            case "string":
                Services.prefs.setStringPref(
                    target.dataset.preference, target.pressed);
                break;
        }
    }
}

/* Entrypoint */
window.onload = (event) => {
    const preferencesObserver = new PreferencesObserver();
    Services.prefs.addObserver("", preferencesObserver.observe);
    document.addEventListener("click", preferencesObserver.handleClick);
};
