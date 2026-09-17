// Copyright 2026 Peter Mack
// SPDX-License-Identifier: MPL-2.0

export class TabCounter {
    init() {
        const windowObserver = {
            observe: (subject, topic) => {
                if (topic === 'domwindowopened') {
                    const window = subject;
                    window.addEventListener('load', () => {
                        if (window.location.href !== 'chrome://browser/content/browser.xhtml') return;
                        this._setupForWindow(window);
                    }, { once: true });
                }
            }
        };

        Services.obs.addObserver(windowObserver, 'domwindowopened');
    }

    _setupForWindow(window) {
        const updateCount = (adjustment = 0) => {
            let count = window.gBrowser.tabs.length + adjustment;
            const displayValue = count > 99 ? '∞' : count;
            window.document.documentElement.style.setProperty('--tab-count', `"${displayValue}"`);
        };

        updateCount();

        const container = window.gBrowser.tabContainer;
        container.addEventListener('TabOpen', () => updateCount(0));
        container.addEventListener('TabClose', () => updateCount(-1));
    }
}
