/**
 * Summaverick workspace helpers — mode pill, citations, source rail,
 * visual-viewport composer, and quiet motion that is not the research loop.
 */
(function () {
  'use strict';

  var reduced = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  function qs(sel, root) {
    return (root || document).querySelector(sel);
  }

  function syncModePill() {
    var group = qs('#depthModes');
    var indicator = qs('.mode-indicator', group);
    if (!group || !indicator) return;
    var active = group.querySelector('.mode-pill.active') || group.querySelector('[aria-checked="true"]');
    if (!active) return;
    var groupBox = group.getBoundingClientRect();
    var pillBox = active.getBoundingClientRect();
    group.style.setProperty('--mode-x', (pillBox.left - groupBox.left - 3) + 'px');
    group.style.setProperty('--mode-w', pillBox.width + 'px');
  }

  function bindModePills() {
    var group = qs('#depthModes');
    if (!group) return;
    if (!qs('.mode-indicator', group)) {
      var mark = document.createElement('span');
      mark.className = 'mode-indicator';
      mark.setAttribute('aria-hidden', 'true');
      group.insertBefore(mark, group.firstChild);
    }
    group.setAttribute('role', 'radiogroup');
    group.setAttribute('aria-label', 'Research depth');
    Array.prototype.forEach.call(group.querySelectorAll('.mode-pill'), function (pill) {
      pill.setAttribute('role', 'radio');
      pill.setAttribute('aria-checked', pill.classList.contains('active') ? 'true' : 'false');
    });
    group.addEventListener('click', function () {
      window.requestAnimationFrame(function () {
        Array.prototype.forEach.call(group.querySelectorAll('.mode-pill'), function (pill) {
          pill.setAttribute('aria-checked', pill.classList.contains('active') ? 'true' : 'false');
        });
        syncModePill();
      });
    });
    window.addEventListener('resize', syncModePill, { passive: true });
    if (window.visualViewport) {
      window.visualViewport.addEventListener('resize', syncModePill, { passive: true });
    }
    syncModePill();
  }

  function bindSendPress() {
    var send = qs('#sendBtn');
    var composer = qs('#composer');
    if (!send || !composer) return;
    function press() {
      if (reduced) return;
      composer.classList.remove('is-sending');
      void composer.offsetWidth;
      composer.classList.add('is-sending');
      window.setTimeout(function () {
        composer.classList.remove('is-sending');
      }, 180);
    }
    send.addEventListener('click', press);
    var input = qs('#queryInput');
    if (input) {
      input.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' && !e.shiftKey) press();
      });
    }
  }

  function isCoarse() {
    return window.matchMedia && window.matchMedia('(hover: none), (pointer: coarse)').matches;
  }

  function closeCite() {
    var preview = qs('#citePreview');
    var sheet = qs('#citeSheet');
    var overlay = qs('#citeSheetOverlay');
    if (preview) {
      preview.classList.remove('is-on');
      preview.hidden = true;
    }
    if (sheet) {
      sheet.classList.remove('is-on');
      sheet.hidden = true;
    }
    if (overlay) {
      overlay.classList.remove('is-on');
      overlay.hidden = true;
    }
    var opener = document._citeOpener;
    if (opener && typeof opener.focus === 'function') opener.focus();
    document._citeOpener = null;
  }

  function fillCite(node, data) {
    if (!node || !data) return;
    var source = node.querySelector('[data-cite-source]');
    var title = node.querySelector('[data-cite-title]');
    var excerpt = node.querySelector('[data-cite-excerpt]');
    var open = node.querySelector('[data-cite-open]');
    if (source) source.textContent = data.source || hostFrom(data.url) || 'Source';
    if (title) title.textContent = data.title || data.source || 'Source';
    if (excerpt) {
      excerpt.textContent = data.excerpt || '';
      excerpt.hidden = !data.excerpt;
    }
    if (open) {
      if (data.url) {
        open.href = data.url;
        open.hidden = false;
      } else {
        open.hidden = true;
      }
    }
  }

  function hostFrom(url) {
    try { return new URL(url).hostname.replace(/^www\./, ''); } catch (e) { return ''; }
  }

  function openCite(anchor, data) {
    document._citeOpener = anchor;
    if (isCoarse()) {
      var sheet = qs('#citeSheet');
      var overlay = qs('#citeSheetOverlay');
      fillCite(sheet, data);
      if (sheet) {
        sheet.hidden = false;
        sheet.classList.add('is-on');
      }
      if (overlay) {
        overlay.hidden = false;
        overlay.classList.add('is-on');
      }
      var closeBtn = qs('.cite-sheet-close', sheet);
      if (closeBtn) closeBtn.focus();
      return;
    }
    var preview = qs('#citePreview');
    if (!preview) return;
    fillCite(preview, data);
    preview.hidden = false;
    preview.classList.add('is-on');
    var rect = anchor.getBoundingClientRect();
    var top = rect.bottom + window.scrollY + 8;
    var left = Math.min(
      Math.max(12, rect.left + window.scrollX - 40),
      window.scrollX + window.innerWidth - 340
    );
    preview.style.top = top + 'px';
    preview.style.left = left + 'px';
  }

  function citeDataFrom(el) {
    return {
      url: el.getAttribute('data-url') || '',
      title: el.getAttribute('data-title') || el.getAttribute('title') || '',
      source: el.getAttribute('data-source') || '',
      excerpt: el.getAttribute('data-excerpt') || ''
    };
  }

  function bindCitations() {
    document.addEventListener('click', function (e) {
      var cite = e.target.closest && e.target.closest('.cite');
      if (!cite) {
        if (e.target.closest && (e.target.closest('.cite-preview') || e.target.closest('.cite-sheet'))) return;
        closeCite();
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      openCite(cite, citeDataFrom(cite));
    }, true);

    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') closeCite();
    });

    var overlay = qs('#citeSheetOverlay');
    var closeBtn = qs('.cite-sheet-close');
    if (overlay) overlay.addEventListener('click', closeCite);
    if (closeBtn) closeBtn.addEventListener('click', closeCite);
  }

  function bindHamburger() {}

  function bindViewport() {
    var root = document.documentElement;
    function apply() {
      var vv = window.visualViewport;
      if (!vv) return;
      root.style.setProperty('--vv-height', Math.round(vv.height) + 'px');
    }
    apply();
    if (window.visualViewport) {
      window.visualViewport.addEventListener('resize', apply, { passive: true });
      window.visualViewport.addEventListener('scroll', apply, { passive: true });
    }
    window.addEventListener('orientationchange', function () {
      window.setTimeout(apply, 120);
    });
  }

  function expose() {
    window.SVWorkspace = {
      syncModePill: syncModePill,
      closeCite: closeCite,
      openCite: openCite,
      hostFrom: hostFrom
    };
  }

  function init() {
    bindModePills();
    bindSendPress();
    bindCitations();
    bindHamburger();
    bindViewport();
    expose();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
