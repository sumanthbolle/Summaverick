/**
 * Page Guide — a fly-out help panel, self-contained.
 *
 * Adds a small tab docked to the right edge of the viewport. Opening it
 * slides in a right-hand panel that explains how to explore the current
 * page: what the key controls are and how they fit together.
 *
 * Follows the SBShared / SBSearch / SBSubscribe pattern already used on
 * this site: one shared script, page-specific content passed at init time,
 * styles + markup injected on first use, dark mode handled automatically
 * via `html[data-theme="dark"]` (same attribute nav-theme.js already sets).
 *
 * Usage:
 *   <script src="assets/js/shared/help-panel.js"></script>
 *   <script>
 *     SBHelpGuide.init({
 *       page: 'blog',
 *       title: 'How to explore the Blog',
 *       intro: 'Optional one-line summary.',
 *       steps: [
 *         { title: 'Step title', desc: 'What it does and where to find it.' }
 *       ]
 *     });
 *   </script>
 */
(function () {
  'use strict';

  var STYLE_ID = 'sbHelpGuideStyles';
  var SEEN_PREFIX = 'sbhg_seen_';

  var CSS =
    '.sbhg-tab{position:fixed;top:50%;right:0;transform:translateY(-50%);z-index:9400;' +
      'display:flex;align-items:center;gap:8px;padding:12px 14px 12px 12px;border:1px solid rgba(0,0,0,0.08);' +
      'border-right:none;border-radius:14px 0 0 14px;background:rgba(255,255,255,0.92);color:#1d1d1f;' +
      'box-shadow:-6px 4px 24px rgba(0,0,0,0.14);cursor:pointer;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text",system-ui,sans-serif;' +
      'font-size:13px;font-weight:600;backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);' +
      'transition:padding-right .25s ease,box-shadow .25s ease;}' +
    '.sbhg-tab:hover{padding-right:18px;box-shadow:-8px 6px 28px rgba(0,0,0,0.2);}' +
    '.sbhg-tab:focus-visible{outline:2px solid #0066CC;outline-offset:2px;}' +
    '.sbhg-tab svg{width:17px;height:17px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex-shrink:0;}' +
    '.sbhg-tab-label{white-space:nowrap;}' +
    '.sbhg-tab-dot{position:absolute;top:6px;right:8px;width:8px;height:8px;border-radius:50%;background:#ff3b30;' +
      'box-shadow:0 0 0 2px rgba(255,255,255,0.92);}' +
    '@media(max-width:640px){.sbhg-tab-label{display:none}.sbhg-tab{padding:12px 10px;top:auto;bottom:100px;transform:none;}' +
      '.sbhg-tab:hover{padding-right:10px}}' +
    '.sbhg-scrim{position:fixed;inset:0;background:rgba(0,0,0,0.42);backdrop-filter:blur(2px);-webkit-backdrop-filter:blur(2px);' +
      'z-index:10040;opacity:0;pointer-events:none;transition:opacity .3s ease;}' +
    '.sbhg-scrim.show{opacity:1;pointer-events:auto;}' +
    '.sbhg-panel{position:fixed;top:0;right:0;bottom:0;width:min(420px,92vw);background:rgba(251,251,253,0.98);' +
      'backdrop-filter:blur(28px);-webkit-backdrop-filter:blur(28px);border-left:1px solid rgba(0,0,0,0.08);' +
      'box-shadow:-24px 0 70px rgba(0,0,0,0.22);z-index:10041;transform:translateX(105%);' +
      'transition:transform .38s cubic-bezier(.22,1,.36,1);display:flex;flex-direction:column;visibility:hidden;color:#1d1d1f;}' +
    '.sbhg-panel.open{transform:translateX(0);visibility:visible;}' +
    '.sbhg-head{display:flex;align-items:flex-start;gap:14px;padding:24px 22px 16px;border-bottom:1px solid rgba(0,0,0,0.07);}' +
    '.sbhg-head-icon{width:38px;height:38px;border-radius:11px;background:rgba(0,102,204,0.1);color:#0066CC;' +
      'display:flex;align-items:center;justify-content:center;flex-shrink:0;}' +
    '.sbhg-head-icon svg{width:19px;height:19px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;}' +
    '.sbhg-head-text{flex:1;min-width:0;}' +
    '.sbhg-eyebrow{font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:#0066CC;margin-bottom:4px;}' +
    '.sbhg-title{font-size:18px;font-weight:650;letter-spacing:-.01em;line-height:1.3;}' +
    '.sbhg-close{width:32px;height:32px;min-width:32px;border-radius:50%;border:none;background:rgba(0,0,0,0.05);' +
      'color:#1d1d1f;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:background .2s;margin-left:auto;}' +
    '.sbhg-close:hover{background:rgba(0,0,0,0.1);}' +
    '.sbhg-close svg{width:14px;height:14px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;}' +
    '.sbhg-body{padding:18px 22px 26px;overflow-y:auto;flex:1;}' +
    '.sbhg-intro{font-size:14px;line-height:1.55;color:#4b4b52;margin-bottom:20px;}' +
    '.sbhg-steps{list-style:none;display:flex;flex-direction:column;gap:16px;counter-reset:sbhg-step;}' +
    '.sbhg-step{display:flex;gap:14px;counter-increment:sbhg-step;}' +
    '.sbhg-step-num{flex-shrink:0;width:26px;height:26px;border-radius:50%;background:#0066CC;color:#fff;' +
      'font-size:12px;font-weight:700;display:flex;align-items:center;justify-content:center;}' +
    '.sbhg-step-num::before{content:counter(sbhg-step);}' +
    '.sbhg-step-text{padding-top:2px;}' +
    '.sbhg-step-title{font-size:14px;font-weight:650;margin-bottom:3px;letter-spacing:-.005em;}' +
    '.sbhg-step-desc{font-size:13px;line-height:1.55;color:#5b5b62;}' +
    '.sbhg-foot{padding:16px 22px 22px;border-top:1px solid rgba(0,0,0,0.06);font-size:12px;color:#86868b;line-height:1.5;}' +
    '.sbhg-foot a{color:#0066CC;text-decoration:none;font-weight:600;}' +
    '.sbhg-foot a:hover{text-decoration:underline;}' +
    /* dark mode: same attribute nav-theme.js sets on <html> (metals, summaverick) */
    'html[data-theme="dark"] .sbhg-tab{background:rgba(28,28,32,0.92);color:#f2f2f7;border-color:rgba(255,255,255,0.1);}' +
    'html[data-theme="dark"] .sbhg-panel{background:rgba(20,20,24,0.97);border-left-color:rgba(255,255,255,0.08);color:#f2f2f7;}' +
    'html[data-theme="dark"] .sbhg-head{border-bottom-color:rgba(255,255,255,0.08);}' +
    'html[data-theme="dark"] .sbhg-head-icon{background:rgba(76,156,255,0.16);color:#4c9cff;}' +
    'html[data-theme="dark"] .sbhg-eyebrow{color:#4c9cff;}' +
    'html[data-theme="dark"] .sbhg-close{background:rgba(255,255,255,0.08);color:#f2f2f7;}' +
    'html[data-theme="dark"] .sbhg-close:hover{background:rgba(255,255,255,0.16);}' +
    'html[data-theme="dark"] .sbhg-intro{color:#c8cdd8;}' +
    'html[data-theme="dark"] .sbhg-step-num{background:#4c9cff;color:#0d0d10;}' +
    'html[data-theme="dark"] .sbhg-step-desc{color:#a6aec0;}' +
    'html[data-theme="dark"] .sbhg-foot{border-top-color:rgba(255,255,255,0.08);color:#9a9aa2;}' +
    'html[data-theme="dark"] .sbhg-foot a{color:#4c9cff;}' +
    '@media(prefers-reduced-motion:reduce){.sbhg-tab,.sbhg-scrim,.sbhg-panel{transition:none!important;}}';

  var GUIDE_SVG =
    '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"></circle>' +
    '<path d="M9.5 9a2.5 2.5 0 1 1 3.4 2.3c-.7.3-1.4.9-1.4 1.9v.3"></path>' +
    '<line x1="12" y1="16.5" x2="12" y2="16.6"></line></svg>';

  var CLOSE_SVG = '<svg viewBox="0 0 24 24"><line x1="6" y1="6" x2="18" y2="18"></line><line x1="18" y1="6" x2="6" y2="18"></line></svg>';

  function ensureStyles() {
    if (document.getElementById(STYLE_ID)) return;
    var style = document.createElement('style');
    style.id = STYLE_ID;
    style.textContent = CSS;
    document.head.appendChild(style);
  }

  function esc(str) {
    if (str == null) return '';
    var div = document.createElement('div');
    div.textContent = String(str);
    return div.innerHTML;
  }

  function buildStepsHtml(steps) {
    if (!steps || !steps.length) return '';
    var html = '<ol class="sbhg-steps">';
    steps.forEach(function (step) {
      html +=
        '<li class="sbhg-step">' +
          '<span class="sbhg-step-num" aria-hidden="true"></span>' +
          '<div class="sbhg-step-text">' +
            '<div class="sbhg-step-title">' + esc(step.title) + '</div>' +
            '<div class="sbhg-step-desc">' + esc(step.desc) + '</div>' +
          '</div>' +
        '</li>';
    });
    html += '</ol>';
    return html;
  }

  function init(config) {
    var settings = config || {};
    var pageKey = settings.page || 'page';
    if (document.getElementById('sbHelpGuideTab')) return; // already initialized on this page

    ensureStyles();

    var seenKey = SEEN_PREFIX + pageKey;
    var isFirstVisit = false;
    try { isFirstVisit = !localStorage.getItem(seenKey); } catch (e) { isFirstVisit = false; }

    var tab = document.createElement('button');
    tab.type = 'button';
    tab.id = 'sbHelpGuideTab';
    tab.className = 'sbhg-tab';
    tab.setAttribute('aria-label', settings.tabLabel || 'Open page guide');
    tab.setAttribute('aria-haspopup', 'dialog');
    tab.setAttribute('aria-expanded', 'false');
    tab.innerHTML = GUIDE_SVG + '<span class="sbhg-tab-label">' + esc(settings.tabLabel || 'Guide') + '</span>' +
      (isFirstVisit ? '<span class="sbhg-tab-dot" aria-hidden="true"></span>' : '');

    var scrim = document.createElement('div');
    scrim.id = 'sbHelpGuideScrim';
    scrim.className = 'sbhg-scrim';

    var panel = document.createElement('div');
    panel.id = 'sbHelpGuidePanel';
    panel.className = 'sbhg-panel';
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'true');
    panel.setAttribute('aria-labelledby', 'sbHelpGuideTitle');

    var introHtml = settings.intro ? '<p class="sbhg-intro">' + esc(settings.intro) + '</p>' : '';
    var footHtml = '';
    if (settings.footerNote || settings.footerLink) {
      footHtml = '<div class="sbhg-foot">' + esc(settings.footerNote || '') +
        (settings.footerLink ? ' <a href="' + esc(settings.footerLink.href) + '">' + esc(settings.footerLink.label) + '</a>' : '') +
        '</div>';
    }

    panel.innerHTML =
      '<div class="sbhg-head">' +
        '<div class="sbhg-head-icon" aria-hidden="true">' + GUIDE_SVG + '</div>' +
        '<div class="sbhg-head-text">' +
          '<div class="sbhg-eyebrow">Page guide</div>' +
          '<div class="sbhg-title" id="sbHelpGuideTitle">' + esc(settings.title || 'How to explore this page') + '</div>' +
        '</div>' +
        '<button type="button" class="sbhg-close" id="sbHelpGuideClose" aria-label="Close page guide">' + CLOSE_SVG + '</button>' +
      '</div>' +
      '<div class="sbhg-body">' + introHtml + buildStepsHtml(settings.steps) + '</div>' +
      footHtml;

    document.body.appendChild(tab);
    document.body.appendChild(scrim);
    document.body.appendChild(panel);

    var closeBtn = panel.querySelector('#sbHelpGuideClose');

    function open() {
      scrim.classList.add('show');
      panel.classList.add('open');
      tab.setAttribute('aria-expanded', 'true');
      document.body.style.overflow = 'hidden';
      var dot = tab.querySelector('.sbhg-tab-dot');
      if (dot) dot.remove();
      try { localStorage.setItem(seenKey, '1'); } catch (e) {}
      window.setTimeout(function () { closeBtn.focus(); }, 50);
    }

    function close() {
      scrim.classList.remove('show');
      panel.classList.remove('open');
      tab.setAttribute('aria-expanded', 'false');
      document.body.style.overflow = '';
      tab.focus();
    }

    function toggle() {
      if (panel.classList.contains('open')) close(); else open();
    }

    tab.addEventListener('click', toggle);
    closeBtn.addEventListener('click', close);
    scrim.addEventListener('click', close);
    document.addEventListener('keydown', function (event) {
      if (event.key === 'Escape' && panel.classList.contains('open')) close();
    });

    window.SBHelpGuideState = window.SBHelpGuideState || {};
    window.SBHelpGuideState[pageKey] = { open: open, close: close, toggle: toggle };
  }

  window.SBHelpGuide = { init: init };
})();
