/* RetroKM planner: two small page helpers shared by index.html and desk.html.
 *
 *   ask(question, yesLabel, onYes)  a yes/no question inside the page (some
 *                                   viewers, such as claude.ai's, never show
 *                                   confirm() dialogs)
 *   offer(name, text, type)         save a file; where the page sits in a
 *                                   frame that blocks downloads, show the text
 *                                   with a Copy button instead
 */
(function (root) {
  'use strict';
  const box = () => document.getElementById('ask');

  function button(label, cls, onclick) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = cls;
    b.textContent = label;
    b.addEventListener('click', onclick);
    return b;
  }
  function close() {
    const el = box();
    el.hidden = true;
    el.replaceChildren();
  }

  function ask(question, yesLabel, onYes) {
    const el = box();
    const p = document.createElement('p');
    p.textContent = question;
    const row = document.createElement('div');
    row.className = 'bar';
    row.append(button(yesLabel, 'btn primary', () => { close(); onYes(); }), button('Cancel', 'btn', close));
    el.replaceChildren(p, row);
    el.hidden = false;
    row.firstChild.focus();
  }

  function framed() {
    try { return window.self !== window.top; } catch (e) { return true; }
  }

  function offer(name, text, type) {
    if (!framed()) {
      const url = URL.createObjectURL(new Blob([text], { type }));
      const a = document.createElement('a');
      a.href = url;
      a.download = name;
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      return;
    }
    const el = box();
    const p = document.createElement('p');
    p.textContent = `This viewer cannot save files. Copy the text of ${name} below and paste it into a file of that name.`;
    const ta = document.createElement('textarea');
    ta.id = 'ask-text';
    ta.readOnly = true;
    ta.value = text;
    ta.rows = 10;
    ta.setAttribute('aria-label', name);
    const row = document.createElement('div');
    row.className = 'bar';
    const copy = button('Copy', 'btn primary', () => {
      const done = () => { copy.textContent = 'Copied'; };
      const fallback = () => { ta.select(); try { document.execCommand('copy'); done(); } catch (e) { copy.textContent = 'Select the text and copy it'; } };
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, fallback);
      else fallback();
    });
    row.append(copy, button('Close', 'btn', close));
    el.replaceChildren(p, ta, row);
    el.hidden = false;
    el.scrollIntoView({ block: 'nearest' });
  }

  root.RetroKMUI = { ask, offer };
})(typeof globalThis !== 'undefined' ? globalThis : this);
