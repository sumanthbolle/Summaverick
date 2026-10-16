(function (root, factory) {
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  if (root && root.document) api.init(root.document, root);
})(typeof window !== 'undefined' ? window : null, function () {
  var CLOSE_DELAY = 180;

  function init(doc, runtime) {
    runtime = runtime || {};
    if (doc.documentElement && doc.documentElement.classList) {
      doc.documentElement.classList.add('nav-enhanced');
    }
    var drops = Array.prototype.slice.call(doc.querySelectorAll('[data-nav-drop]'));
    var timers = new Map();
    var removers = [];
    var canHover = runtime.matchMedia && runtime.matchMedia('(hover: hover) and (pointer: fine)').matches;
    var setTimer = runtime.setTimeout ? runtime.setTimeout.bind(runtime) : setTimeout;
    var clearTimer = runtime.clearTimeout ? runtime.clearTimeout.bind(runtime) : clearTimeout;

    function button(drop) {
      return drop.querySelector('.nav-drop__btn');
    }

    function cancel(drop) {
      if (!timers.has(drop)) return;
      clearTimer(timers.get(drop));
      timers.delete(drop);
    }

    function close(drop) {
      cancel(drop);
      drop.classList.remove('open');
      var trigger = button(drop);
      if (trigger) trigger.setAttribute('aria-expanded', 'false');
    }

    function open(drop) {
      drops.forEach(function (other) {
        if (other !== drop) close(other);
      });
      cancel(drop);
      drop.classList.add('open');
      var trigger = button(drop);
      if (trigger) trigger.setAttribute('aria-expanded', 'true');
    }

    function scheduleClose(drop) {
      cancel(drop);
      timers.set(drop, setTimer(function () {
        timers.delete(drop);
        close(drop);
      }, CLOSE_DELAY));
    }

    function listen(target, type, handler) {
      target.addEventListener(type, handler);
      removers.push(function () {
        target.removeEventListener(type, handler);
      });
    }

    drops.forEach(function (drop) {
      var trigger = button(drop);
      if (!trigger || !drop.querySelector('.nav-drop__menu')) return;

      listen(trigger, 'click', function (event) {
        event.preventDefault();
        if (drop.classList.contains('open')) close(drop);
        else open(drop);
      });

      if (canHover) {
        listen(drop, 'pointerenter', function () {
          open(drop);
        });
        listen(drop, 'pointerleave', function () {
          scheduleClose(drop);
        });
      }

      listen(drop, 'focusout', function (event) {
        if (!drop.contains(event.relatedTarget)) scheduleClose(drop);
      });
    });

    listen(doc, 'click', function (event) {
      drops.forEach(function (drop) {
        if (!drop.contains(event.target)) close(drop);
      });
    });

    listen(doc, 'keydown', function (event) {
      if (event.key !== 'Escape') return;
      drops.forEach(function (drop) {
        if (!drop.classList.contains('open')) return;
        close(drop);
        var trigger = button(drop);
        if (trigger) trigger.focus();
      });
    });

    initStandardMobileMenu(doc, listen);
    initSiteMobileMenu(doc, listen);

    return {
      open: open,
      close: close,
      destroy: function () {
        drops.forEach(close);
        removers.forEach(function (remove) {
          remove();
        });
      }
    };
  }

  function initStandardMobileMenu(doc, listen) {
    var trigger = doc.querySelector && doc.querySelector('.menu-toggle');
    var panel = doc.querySelector && doc.querySelector('.mobile-menu[data-mobile-nav]');
    if (!trigger || !panel) return;

    function sync() {
      trigger.setAttribute('aria-expanded', panel.classList.contains('active') ? 'true' : 'false');
    }

    sync();
    listen(trigger, 'click', sync);
    Array.prototype.slice.call(panel.querySelectorAll('a')).forEach(function (link) {
      listen(link, 'click', sync);
    });
    listen(doc, 'keydown', function (event) {
      if (event.key !== 'Escape' || !panel.classList.contains('active')) return;
      panel.classList.remove('active');
      trigger.classList.remove('active');
      sync();
      trigger.focus();
    });
  }

  function initSiteMobileMenu(doc, listen) {
    var trigger = doc.querySelector && doc.querySelector('[data-site-menu-toggle]');
    var panel = doc.querySelector && doc.querySelector('[data-site-mobile-menu]');
    if (!trigger || !panel) return;

    function close() {
      panel.classList.remove('open');
      trigger.setAttribute('aria-expanded', 'false');
    }

    listen(trigger, 'click', function (event) {
      event.preventDefault();
      var willOpen = !panel.classList.contains('open');
      close();
      if (willOpen) {
        panel.classList.add('open');
        trigger.setAttribute('aria-expanded', 'true');
      }
    });

    listen(doc, 'keydown', function (event) {
      if (event.key !== 'Escape' || !panel.classList.contains('open')) return;
      close();
      trigger.focus();
    });
  }

  return {
    CLOSE_DELAY: CLOSE_DELAY,
    init: init
  };
});
