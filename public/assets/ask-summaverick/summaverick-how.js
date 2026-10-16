/**
 * Summaverick how-it-works — reveal the research timeline once.
 * No pin, no particles, no continuous loop.
 */
(function () {
  'use strict';

  var section = document.getElementById('summaverick-how');
  if (!section) return;

  var cta = section.querySelector('.sm-how__cta');
  var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var revealed = false;

  function reveal() {
    if (revealed) return;
    revealed = true;
    section.classList.add('is-revealed');
  }

  function watchChat() {
    var shell = document.getElementById('appShell');
    if (!shell || typeof MutationObserver === 'undefined') return;
    new MutationObserver(function () {
      if (shell.classList.contains('app-active')) {
        /* Conversation owns attention; leave the section as-is. */
      }
    }).observe(shell, { attributes: true, attributeFilter: ['class'] });
  }

  if (cta) {
    cta.addEventListener('click', function () {
      var input = document.getElementById('queryInput');
      if (!input) return;
      input.focus();
    });
  }

  if (reduced || !('IntersectionObserver' in window)) {
    reveal();
  } else {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting && entry.intersectionRatio > 0.28) {
          reveal();
          io.disconnect();
        }
      });
    }, { threshold: [0.28, 0.5] });
    io.observe(section);
  }

  watchChat();
})();
