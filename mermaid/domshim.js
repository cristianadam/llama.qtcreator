// Minimal DOM shim for running mermaid inside QuickJS (no browser available).
// Implements only the DOM surface mermaid's render pipeline touches:
//   - a small element/text node tree with attributes, style and classList
//   - just-enough querySelector(All) selector matching
//   - getBBox()/getBoundingClientRect() backed by the host's text measurement
//     (__qt.measureText, implemented in C++ with QFontMetrics, see
//     mermaidengine.cpp) so mermaid's layout (dagre & co.) gets real widths
//   - a DOMParser + NodeIterator pair good enough for DOMPurify's sanitize
//     path to run (and to pass text through unchanged)
//   - shims for the browser globals mermaid probes (window, navigator, timers
//     backed by microtasks, TextEncoder, atob/btoa, observers, ...)
//
// Host API expected on globalThis.__qt:
//   __qt.measureText(text, family, sizePx, weight, fontStyle)
//       -> { width, ascent, descent, height }
//   __qt.log(string)   (optional; used for console.*)

'use strict';

const SVG_NS = 'http://www.w3.org/2000/svg';

function __qtMeasureText() {
  try { return __qt.measureText(...arguments); } catch (e) { return { width: 0, ascent: 0, descent: 0, height: 0 }; }
}

function __qtLog(...args) {
  try { __qt.log(args.map(a => {
    try { return typeof a === 'string' ? a : JSON.stringify(a); } catch (e) { return String(a); }
  }).join(' ')); } catch (e) {}
}

class DomEvent {
  constructor(type) { this.type = type; this.preventDefault = () => {}; this.stopPropagation = () => {}; }
}

class ClassList {
  constructor(el) { this._el = el; }
  _set() { return (this._el.getAttribute('class') || '').split(/\s+/).filter(Boolean); }
  add(...cs) { const s = new Set(this._set()); cs.forEach(c => s.add(c)); this._el.setAttribute('class', [...s].join(' ')); }
  remove(...cs) { const s = new Set(this._set()); cs.forEach(c => s.delete(c)); this._el.setAttribute('class', [...s].join(' ')); }
  contains(c) { return this._set().includes(c); }
  toggle(c) { this.contains(c) ? this.remove(c) : this.add(c); return this.contains(c); }
  toString() { return this._set().join(' '); }
}

class StyleDeclaration {
  constructor(el) { this._el = el; this._map = new Map(); }
  setProperty(name, value, _priority) { this._map.set(name, String(value)); }
  removeProperty(name) { this._map.delete(name); }
  getPropertyValue(name) { return this._map.has(name) ? this._map.get(name) : ''; }
  get cssText() { return [...this._map].map(([k, v]) => k + ': ' + v + ';').join(' '); }
  set cssText(text) {
    this._map.clear();
    String(text).split(';').forEach(part => {
      const idx = part.indexOf(':');
      if (idx > 0) this._map.set(part.slice(0, idx).trim(), part.slice(idx + 1).trim());
    });
  }
  // camelCase property assignment (style.width = ...)
  get [Symbol.toStringTag]() { return 'CSSStyleDeclaration'; }
}

// Proxy-based style object so that style.foo = "bar" works for any property.
function makeStyle(el) {
  const decl = new StyleDeclaration(el);
  const camelToKebab = s => s.replace(/[A-Z]/g, m => '-' + m.toLowerCase());
  const kebabToCamel = s => s.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
  return new Proxy(decl, {
    get(target, prop) {
      if (typeof prop !== 'string') return Reflect.get(target, prop);
      if (prop in target) return Reflect.get(target, prop);
      const v = target.getPropertyValue(camelToKebab(prop));
      return v === '' ? '' : v;
    },
    set(target, prop, value) {
      if (typeof prop !== 'string' || prop in target) { Reflect.set(target, prop, value); return true; }
      target.setProperty(camelToKebab(prop), value);
      return true;
    }
  });
}

function parseLength(v, dflt = 0) {
  if (v == null) return dflt;
  if (typeof v === 'number') return v;
  const m = String(v).trim().match(/^(-?[\d.]+)(px|em|rem)?/);
  return m ? parseFloat(m[1]) : dflt;
}

class DomNode {
  constructor(nodeType, nodeName, data) {
    this.nodeType = nodeType; // 1 element, 3 text
    this.nodeName = (typeof nodeName === 'string' ? nodeName : '').toUpperCase();
    this.tagName = this.nodeName;
    this.localName = (typeof nodeName === 'string' ? nodeName : '').toLowerCase();
    if (nodeType === 3 && data != null) this._data = String(data);
    this.childNodes = [];
    this.parentNode = null;
    this._attributes = new Map();
    this.style = makeStyle(this);
    this.classList = new ClassList(this);
    this.namespaceURI = null;
    this.baseVal = undefined;
    this._listeners = new Map();
    this._hidden = false;
    // SVG geometry "base values" (rect/circle/line/ellipse/xlink)
    this._geom = new Proxy(this, {
      get(t, p) {
        if (p === 'animate') return { baseVal: { value: 0 } };
        const v = t.getAttribute(String(p));
        return v == null ? null : v;
      },
      set(t, p, v) { t.setAttribute(String(p), v); return true; }
    });
  }

  // ---- tree ----
  get firstChild() { return this.childNodes[0] || null; }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
  get nextSibling() {
    if (!this.parentNode) return null;
    const i = this.parentNode.childNodes.indexOf(this);
    return this.parentNode.childNodes[i + 1] || null;
  }
  get previousSibling() {
    if (!this.parentNode) return null;
    const i = this.parentNode.childNodes.indexOf(this);
    return this.parentNode.childNodes[i - 1] || null;
  }
  get parentElement() { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }

  get children() { return this.childNodes.filter(n => n.nodeType === 1); }
  get childElementCount() { return this.children.length; }

  appendChild(child) { return this.insertBefore(child, null); }
  insertBefore(child, ref) {
    if (child.parentNode) child.parentNode.removeChild(child);
    const i = ref ? this.childNodes.indexOf(ref) : this.childNodes.length;
    this.childNodes.splice(i, 0, child);
    child.parentNode = this;
    return child;
  }
  removeChild(child) {
    const i = this.childNodes.indexOf(child);
    if (i >= 0) this.childNodes.splice(i, 1);
    child.parentNode = null;
    return child;
  }
  replaceChild(newNode, oldNode) {
    const i = this.childNodes.indexOf(oldNode);
    if (i >= 0) { this.childNodes[i] = newNode; newNode.parentNode = this; oldNode.parentNode = null; }
    return oldNode;
  }
  insertAdjacentElement(pos, el) {
    if (pos === 'beforeend') return this.appendChild(el);
    if (pos === 'afterend') { this.parentNode && this.parentNode.insertBefore(el, this.nextSibling); return el; }
    if (pos === 'beforebegin') { this.parentNode && this.parentNode.insertBefore(el, this); return el; }
    return this.insertBefore(el, this.firstChild);
  }
  contains(node) {
    while (node) { if (node === this) return true; node = node.parentNode; }
    return false;
  }

  cloneNode(deep) {
    const c = new DomNode(this.nodeType, this.localName);
    c.namespaceURI = this.namespaceURI;
    for (const [k, v] of this._attributes) c.setAttribute(k, v);
    c.style = makeStyle(c);
    c.classList = new ClassList(c);
    c._geom = this._geom;
    if (deep) for (const child of this.childNodes) c.appendChild(child.cloneNode(true));
    return c;
  }

  // ---- attributes ----
  setAttribute(name, value) { this._attributes.set(name, String(value)); }
  getAttribute(name) { return this._attributes.has(name) ? this._attributes.get(name) : null; }
  hasAttribute(name) { return this._attributes.has(name); }
  removeAttribute(name) { this._attributes.delete(name); }
  setAttributeNS(ns, name, value) { this.setAttribute(name, value); }
  getAttributeNS(ns, name) { return this.getAttribute(name); }
  setAttributeNode(attr) { this.setAttribute(attr.name, attr.value); return attr; }
  getAttributeNode(name) {
    if (!this._attributes.has(name)) return null;
    return { name, value: this._attributes.get(name), nodeName: name, nodeType: 2 };
  }
  // NamedNodeMap (fresh snapshot on each access)
  get attributes() {
    const arr = [...this._attributes.entries()].map(([name, value]) => ({
      name, value, nodeName: name, nodeType: 2,
      ownerElement: this, specified: true,
      localName: name, namespaceURI: null
    }));
    const nm = { length: arr.length, item: i => arr[i] || null };
    arr.forEach((a, i) => { nm[i] = a; });
    return nm;
  }

  get id() { return this.getAttribute('id') || ''; }
  set id(v) { this._attributes.set('id', String(v)); }
  get className() { return this.getAttribute('class') || ''; }
  set className(v) { this._attributes.set('class', String(v)); }

  // ---- text ----
  get textContent() {
    if (this.nodeType === 3) return this._data || '';
    let out = '';
    for (const c of this.childNodes) out += c.textContent;
    return out;
  }
  set textContent(v) {
    this.childNodes = [];
    for (const c of Object.values(this._listeners) || {}) {}
    if (v != null && String(v).length > 0)
      this.appendChild(new DomNode(3, '#text', String(v)));
  }
  get data() { return this._data || ''; }
  set data(v) { this._data = String(v); }
  get nodeValue() { return this.textContent; }
  set nodeValue(v) { this.textContent = v; }

  // ---- geometry / measurement ----
  _fontProps() {
    const size = parseLength(this.style.getPropertyValue('font-size')
      || (this.parentNode ? this.parentNode._fontProps().size : 14), 14);
    return {
      family: this.style.getPropertyValue('font-family')
        || (this.parentNode ? this.parentNode._fontProps().family : 'sans-serif'),
      size,
      weight: parseInt(this.style.getPropertyValue('font-weight') || '400', 10),
      style: this.style.getPropertyValue('font-style') || 'normal'
    };
  }
  _textSize() {
    const text = this.textContent;
    const f = this._fontProps();
    return __qtMeasureText(text, f.family, f.size, f.weight, f.style);
  }
  // Like parseLength(), but resolves em/rem against this element's font-size
  // (mermaid positions text rows with y="-0.1em" dy="1.1em").
  _length(v, dflt = 0) {
    if (v == null) return dflt;
    if (typeof v === 'number') return v;
    const m = String(v).trim().match(/^(-?[\d.]+)(px|em|rem)?/);
    if (!m) return dflt;
    const n = parseFloat(m[1]);
    if (m[2]) return n * this._fontProps().size;
    return n;
  }
  // Baseline-relative top edge of a text row, honoring dominant-baseline
  // (inherited in SVG). Mermaid centers class/edge labels with it, so
  // ignoring it shifts the measured row by ~half a line height.
  _rowTop(y, m, el) {
    let db = null;
    for (let n = el; n && n.nodeType === 1; n = n.parentNode) {
      db = n.getAttribute('dominant-baseline') || n.style.getPropertyValue('dominant-baseline');
      if (db) break;
    }
    if (db === 'middle' || db === 'central') return y - m.height / 2;
    if (db === 'hanging') return y;
    if (db === 'text-after-edge') return y - m.descent;
    return y - m.ascent;
  }
  // Mermaid builds multi-line labels (class boxes, etc.) as a <text> with
  // <tspan> rows positioned via x/y/dx/dy. getBBox() must report the union
  // of the individual rows, tracking the SVG text cursor — sizing the whole
  // textContent as one line made mermaid lay out a one-line-tall box while
  // the tspans then rendered as N rows, detaching labels from their boxes.
  _multiLineTextBBox() {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    const union = (x, top, w, h) => {
      if (w < 0 && h < 0) return;
      if (x < minX) minX = x;
      if (top < minY) minY = top;
      if (x + w > maxX) maxX = x + w;
      if (top + h > maxY) maxY = top + h;
    };
    let curX = this._length(this.getAttribute('x'), 0);
    let curY = this._length(this.getAttribute('y'), 0);
    for (const c of this.childNodes) {
      if (c.nodeType === 3) {
        const text = c._data || '';
        if (!text.trim()) continue;
        const f = this._fontProps();
        const m = __qtMeasureText(text, f.family, f.size, f.weight, f.style);
        union(curX, this._rowTop(curY, m, this), m.width, m.height);
        curX += m.width;
      } else if (c.nodeType === 1 && c.localName === 'tspan') {
        if (c.hasAttribute('x')) curX = c._length(c.getAttribute('x'), 0);
        curX += c._length(c.getAttribute('dx'), 0);
        if (c.hasAttribute('y')) curY = c._length(c.getAttribute('y'), 0);
        curY += c._length(c.getAttribute('dy'), 0);
        const m = c._textSize();
        union(curX, this._rowTop(curY, m, c), m.width, m.height);
        curX += m.width;
      }
    }
    if (!isFinite(minX)) return { x: 0, y: 0, width: 0, height: 0 };
    return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
  }
  getBBox() {
    if (this.localName === 'style' || this.localName === 'metadata' || this.localName === 'script')
      return { x: 0, y: 0, width: 0, height: 0 };
    if (this.nodeType === 3) {
      const f = this.parentNode ? this.parentNode._fontProps() : { family: 'sans-serif', size: 14, weight: 400, style: 'normal' };
      const m = this.parentNode
        ? __qtMeasureText(this._data || '', f.family, f.size, f.weight, f.style)
        : { width: 0, ascent: 0, descent: 0, height: 14 };
      return { x: 0, y: -m.ascent, width: m.width, height: m.height };
    }
    const tag = this.localName;
    if (tag === 'text' || tag === 'tspan' || tag === 'title') {
      const hasTspanChild = this.childNodes.some(c => c.nodeType === 1 && c.localName === 'tspan');
      if (!hasTspanChild) {
        const m = this._textSize();
        const x = this._length(this.getAttribute('x'), 0);
        const y = this._length(this.getAttribute('y'), 0);
        return { x, y: this._rowTop(y, m, this), width: m.width, height: m.height };
      }
      return this._multiLineTextBBox();
    }
    if (tag === 'rect' || tag === 'image' || tag === 'foreignObject') {
      const x = parseLength(this.getAttribute('x'), 0);
      const y = parseLength(this.getAttribute('y'), 0);
      return { x, y, width: parseLength(this.getAttribute('width'), 0), height: parseLength(this.getAttribute('height'), 0) };
    }
    if (tag === 'circle') {
      const cx = parseLength(this.getAttribute('cx'), 0), cy = parseLength(this.getAttribute('cy'), 0), r = parseLength(this.getAttribute('r'), 0);
      return { x: cx - r, y: cy - r, width: 2 * r, height: 2 * r };
    }
    if (tag === 'ellipse') {
      const cx = parseLength(this.getAttribute('cx'), 0), cy = parseLength(this.getAttribute('cy'), 0);
      const rx = parseLength(this.getAttribute('rx'), 0), ry = parseLength(this.getAttribute('ry'), 0);
      return { x: cx - rx, y: cy - ry, width: 2 * rx, height: 2 * ry };
    }
    if (tag === 'line') {
      const x1 = parseLength(this.getAttribute('x1'), 0), y1 = parseLength(this.getAttribute('y1'), 0);
      const x2 = parseLength(this.getAttribute('x2'), 0), y2 = parseLength(this.getAttribute('y2'), 0);
      const w = parseLength(this.style.getPropertyValue('stroke-width') || this.getAttribute('stroke-width'), 1);
      return { x: Math.min(x1, x2) - w / 2, y: Math.min(y1, y2) - w / 2, width: Math.abs(x2 - x1) + w, height: Math.abs(y2 - y1) + w };
    }
    if (tag === 'path' || tag === 'polyline' || tag === 'polygon') {
      const d = this.getAttribute('d') || '';
      const nums = d.match(/-?[\d.]+/g);
      if (!nums) {
        const w = parseLength(this.style.getPropertyValue('stroke-width') || this.getAttribute('stroke-width'), 1);
        return { x: 0, y: 0, width: w, height: w };
      }
      let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
      for (const n of nums) {
        const v = parseFloat(n);
        if (v < minX) minX = v; if (v > maxX) maxX = v;
        if (v < minY) minY = v; if (v > maxY) maxY = v;
      }
      return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
    }
    // group/other: union of children, with each child's transform applied
    // (browsers include descendant transforms in a group's bbox — mermaid
    // positions its label rows with translate(), so ignoring this detaches
    // the measured box from the rendered rows)
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const c of this.childNodes) {
      let b = c.getBBox();
      if (b.width < 0 && b.height < 0) continue;
      const tr = c.getAttribute('transform');
      if (tr) {
        const t = tr.match(/translate\(\s*([-\d.e]+)[,\s]+([-\d.e]+)/);
        const sc = tr.match(/scale\(\s*([-\d.e]+)(?:[,\s]+([-\d.e]+))?/);
        const sx = sc ? parseFloat(sc[1]) : 1;
        const sy = sc ? (sc[2] != null ? parseFloat(sc[2]) : sx) : 1;
        const tx = t ? parseFloat(t[1]) : 0;
        const ty = t ? parseFloat(t[2]) : 0;
        b = { x: b.x * sx + tx, y: b.y * sy + ty, width: Math.abs(b.width * sx), height: Math.abs(b.height * sy) };
      }
      if (b.x < minX) minX = b.x;
      if (b.y < minY) minY = b.y;
      if (b.x + b.width > maxX) maxX = b.x + b.width;
      if (b.y + b.height > maxY) maxY = b.y + b.height;
    }
    if (!isFinite(minX)) return { x: 0, y: 0, width: 0, height: 0 };
    return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
  }
  getBoundingClientRect() {
    const b = this.getBBox();
    return { x: b.x, y: b.y, width: b.width, height: b.height, top: b.y, left: b.x, right: b.x + b.width, bottom: b.y + b.height };
  }
  // Block-level containers without intrinsic size stretch to their parent's
  // width, like in a browser (the <body> reports the default page size).
  // Mermaid's gantt renderer sizes the diagram to the render container's
  // offsetWidth - without this fallback it comes out zero wide.
  _inheritedSize() {
    const doc = globalThis.document;
    for (let p = this.parentNode; p && p.nodeType === 1; p = p.parentNode) {
      if (doc && p === doc.body) return { width: 800, height: 600 };
      const b = p.getBoundingClientRect();
      if (b.width || b.height) return { width: b.width || 800, height: b.height || 600 };
    }
    return { width: 800, height: 600 };
  }
  get clientWidth() {
    const b = this.getBoundingClientRect();
    return b.width || this._inheritedSize().width;
  }
  get clientHeight() {
    const b = this.getBoundingClientRect();
    return b.height || this._inheritedSize().height;
  }
  get offsetWidth() { return this.clientWidth; }
  get offsetHeight() { const b = this.getBoundingClientRect(); return b.height; }
  get scrollWidth() { return this.clientWidth; }
  get scrollHeight() { return this.clientHeight; }
  get offsetParent() { return null; }
  getComputedTextLength() { return this._textSize().width; }
  getSubStringLength(i, n) { const m = this._textSize(); return n && n < this.textContent.length ? m.width * n / this.textContent.length : m.width; }
  getStartPositionOfChar(i) { const m = this._textSize(); return { x: parseLength(this.getAttribute('x'), 0) + m.width * (i || 0) / Math.max(1, this.textContent.length), y: parseLength(this.getAttribute('y'), 0) }; }
  getEndPositionOfChar(i) { return this.getStartPositionOfChar((i || 0) + 1); }
  getExtentOfChar(i) { const s = this.getStartPositionOfChar(i), e = this.getEndPositionOfChar(i); return { x: s.x, y: s.y, width: e.x - s.x, height: 0 }; }
  getTotalLength() { return 0; }
  getPointAtLength(d) { return { x: 0, y: 0 }; }
  getTransformedBoundingBox() { return this.getBBox(); }

  // ---- serialization ----
  _escape(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); }
  // Qt's SVG module discards every positioning attribute on <tspan> (x, y,
  // dx, dy) and concatenates all tspans of a <text> into a single line, and
  // it rejects tspans nested in tspans. Mermaid, however, builds its labels
  // as <text> elements with <tspan> rows positioned via x/y/dx/dy. Emit each
  // positioned tspan row as its own <text> element with the resolved
  // absolute x/y (em units included) instead, carrying over the tspan's
  // class/font attributes. <text> elements without tspan children (or whose
  // tspans carry no positioning) serialize as-is: Qt reads their x/y.
  _textRowOut() {
    if (!this.childNodes.some(c => c.nodeType === 1 && c.localName === 'tspan'))
      return null;
    const kCarry = ['class', 'style', 'text-anchor', 'font-weight', 'font-style',
                    'font-size', 'font-family', 'fill'];
    const round = v => Math.round(v * 100) / 100;
    const rows = [];
    let cur = null;
    let curX = this._length(this.getAttribute('x'), 0);
    let curY = this._length(this.getAttribute('y'), 0);
    const newRow = (x, y, src) => {
      const attrs = [];
      const seen = new Set(['x', 'y', 'dx', 'dy']);
      for (const el of [this, src]) {
        if (!el) continue;
        for (const k of kCarry) {
          if (seen.has(k)) continue;
          const v = el.getAttribute(k) || (k === 'style' ? el.style.cssText : '');
          if (v == null || v === '') continue;
          attrs.push([k, v]);
          seen.add(k);
        }
      }
      cur = { x, y, attrs, parts: [] };
      rows.push(cur);
    };
    for (const c of this.childNodes) {
      if (c.nodeType === 3) {
        const t = c._data || '';
        if (!cur) newRow(curX, curY, null);
        cur.parts.push(t);
        const f = this._fontProps();
        curX += __qtMeasureText(t, f.family, f.size, f.weight, f.style).width;
      } else if (c.nodeType === 1 && c.localName === 'tspan') {
        let x = curX, y = curY;
        let positioned = false;
        if (c.hasAttribute('x')) { x = c._length(c.getAttribute('x'), 0); positioned = true; }
        x += c._length(c.getAttribute('dx'), 0);
        if (c.hasAttribute('y')) { y = c._length(c.getAttribute('y'), 0); positioned = true; }
        y += c._length(c.getAttribute('dy'), 0);
        if (positioned || !cur) newRow(x, y, c);
        cur.parts.push(c.textContent);
        curX = x + c._textSize().width;
        curY = y;
      }
    }
    if (!rows.length) return null;
    return rows.map(r => {
      const attrs = r.attrs.map(([k, v]) => ' ' + k + '="' + this._escape(v) + '"').join('')
                  + ' x="' + round(r.x) + '" y="' + round(r.y) + '"';
      const text = r.parts.join('');
      if (!text.trim()) return '';
      return '<text' + attrs + '>' + this._escape(text) + '</text>';
    }).join('');
  }
  serialize() {
    if (this.nodeType === 3) return this._escape(this._data || '');
    if (this.localName === 'text') {
      const rows = this._textRowOut();
      if (rows) return rows;
    }
    let out = '<' + this.localName;
    for (const [k, v] of this._attributes) {
      out += ' ' + k + '="' + this._escape(v) + '"';
    }
    const style = this.style.cssText;
    if (style) out += ' style="' + this._escape(style) + '"';
    if (this.childNodes.length === 0) return out + '/>';
    out += '>';
    for (const c of this.childNodes) out += c.serialize();
    out += '</' + this.localName + '>';
    return out;
  }
  get innerHTML() {
    let out = '';
    for (const c of this.childNodes) out += c.serialize();
    return out;
  }
  set innerHTML(v) {
    this.childNodes = [];
    const parsed = parseHtmlFragment(String(v), this.ownerDocument);
    for (const c of parsed) this.appendChild(c);
  }
  get outerHTML() { return this.serialize(); }
  set outerHTML(v) {
    if (this.parentNode) {
      const parsed = parseHtmlFragment(String(v), this.ownerDocument);
      while (parsed.length) this.parentNode.insertBefore(parsed.shift(), this.nextSibling);
      this.parentNode.removeChild(this);
    }
  }
  get innerText() { return this.textContent; }
  set innerText(v) { this.textContent = v; }
  get ownerSVGElement() {
    let n = this;
    while (n) { if (n.localName === 'svg') return n; n = n.parentNode; }
    return null;
  }

  querySelector(selector) {
    const r = this.querySelectorAll(selector);
    return r.length ? r[0] : null;
  }
  querySelectorAll(selector) {
    const result = [];
    const parts = selector.split(',').map(s => s.trim()).filter(Boolean);
    const walk = node => {
      if (node.nodeType === 1) {
        for (const p of parts) if (node._matches(p)) { result.push(node); break; }
      }
      for (const c of node.childNodes) walk(c);
    };
    for (const c of this.childNodes) walk(c);
    return result;
  }
  getElementsByTagName(tag) {
    const out = [];
    const walk = node => {
      if (node.nodeType === 1) {
        if (tag === '*' || node.localName === String(tag).toLowerCase()) out.push(node);
        for (const c of node.childNodes) walk(c);
      }
    };
    for (const c of this.childNodes) walk(c);
    return out;
  }
  get complete() { return true; }
  get matches() { return sel => this._matches(sel); }
  closest(selector) {
    let n = this;
    while (n && n.nodeType === 1) { if (n._matches(selector)) return n; n = n.parentNode; }
    return null;
  }

  // ---- events ----
  addEventListener(type, fn, _opts) {
    if (!this._listeners.has(type)) this._listeners.set(type, []);
    this._listeners.get(type).push(fn);
  }
  removeEventListener(type, fn) {
    const l = this._listeners.get(type);
    if (l) { const i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); }
  }
  dispatchEvent(ev) {
    const l = this._listeners.get(ev.type);
    if (l) for (const fn of l) fn.call(this, ev);
    return true;
  }
  click() { this.dispatchEvent(new DomEvent('click')); }
  focus() {}
  blur() {}
  remove() { this.parentNode && this.parentNode.removeChild(this); }
  replaceWith(...nodes) {
    if (!this.parentNode) return;
    this.parentNode.replaceChild(nodes[0], this);
    for (let i = 1; i < nodes.length; i++)
      this.parentNode.insertBefore(nodes[i], nodes[i - 1].nextSibling);
  }
  before(...nodes) { this.parentNode && this.parentNode.insertBefore(nodes[0], this); }
  after(...nodes) { this.parentNode && this.parentNode.insertBefore(nodes[0], this.nextSibling); }
  get isConnected() { return this.ownerDocument !== null; }
  get tabindex() { return 0; }
  set tabindex(v) {}

  get hidden() { return this._hidden; }
  set hidden(v) { this._hidden = !!v; }
  get visible() { return !this._hidden; }
  set visible(v) { this._hidden = !v; }
}

// lazy: elements created before installGlobals() finished still resolve it
Object.defineProperty(DomNode.prototype, 'ownerDocument', {
  configurable: true,
  get() { return globalThis.document || null; }
});

function parseHtmlFragment(html, doc) {
  // Very small fragment parser: elements, attributes, text. Enough for the
  // few places mermaid assigns innerHTML.
  const nodes = [];
  const stack = [];
  let pos = 0;
  const n = html.length;
  const push = node => { if (stack.length) stack[stack.length - 1].appendChild(node); else nodes.push(node); };
  while (pos < n) {
    if (html[pos] === '<') {
      if (html.startsWith('<!--', pos)) { const e = html.indexOf('-->', pos); pos = e < 0 ? n : e + 3; continue; }
      if (html[pos + 1] === '/') {
        const e = html.indexOf('>', pos);
        const name = html.slice(pos + 2, e).trim().toLowerCase();
        while (stack.length && stack[stack.length - 1].localName !== name) stack.pop();
        if (stack.length) stack.pop();
        pos = e + 1;
        continue;
      }
      const e = html.indexOf('>', pos);
      const tag = html.slice(pos + 1, e).trim();
      const selfClose = tag.endsWith('/');
      const name = (selfClose ? tag.slice(0, -1) : tag).split(/\s+/)[0].toLowerCase();
      const el = new DomNode(1, name);
      const attrRe = /([a-zA-Z_:][-a-zA-Z0-9_:.]*)\s*(?:=\s*("([^"]*)"|'([^']*)'|[^\s"'>]+))?/g;
      const inner = selfClose ? tag.slice(0, -1) : tag;
      const firstSpace = inner.search(/\s/);
      let m;
      attrRe.lastIndex = 0;
      if (firstSpace >= 0) {
        const attrPart = inner.slice(firstSpace);
        while ((m = attrRe.exec(attrPart)) !== null) {
          if (m[1] === inner.split(/\s+/)[0]) continue;
          const val = m[3] != null ? m[3] : m[4] != null ? m[4] : m[2];
          el.setAttribute(m[1], val != null ? val : '');
        }
      }
      push(el);
      if (!selfClose) stack.push(el);
      pos = e + 1;
      continue;
    }
    const next = html.indexOf('<', pos);
    const textEnd = next < 0 ? n : next;
    const text = html.slice(pos, textEnd);
    if (text.length) {
      const t = new DomNode(3, '#text', text);
      push(t);
    }
    pos = textEnd;
  }
  return nodes;
}

// ---- document ----
class CSSStyleSheet {
  constructor() { this._rules = []; }
  insertRule(rule, index) {
    const r = { cssText: String(rule) };
    if (index == null || index >= this._rules.length) this._rules.push(r);
    else this._rules.splice(index, 0, r);
    return index == null ? this._rules.length - 1 : index;
  }
  deleteRule(i) { this._rules.splice(i, 1); }
  replaceSync(css) {
    this._rules = String(css).split('}').map(s => s.trim()).filter(s => s).map(s => ({ cssText: s + '}' }));
  }
  get cssRules() { return this._rules; }
  get cssText() { return this._rules.map(r => r.cssText).join('\n'); }
}

const document = {
  nodeType: 9,
  nodeName: '#document',
  _idIndex: new Map(),
  createElement(tag) {
    const el = new DomNode(1, tag);
    if (el.id) document._idIndex.set(el.id, el);
    return el;
  },
  createElementNS(ns, tag) {
    const el = new DomNode(1, tag);
    el.namespaceURI = ns;
    return el;
  },
  createTextNode(text) { return new DomNode(3, '#text', String(text)); },
  createDocumentFragment() {
    const f = new DomNode(11, '#document-fragment');
    return f;
  },
  createEvent(type) { return new DomEvent(type); },
  getElementById(id) {
    // walk the live tree first (cheap enough), fall back to index
    for (const root of [document.body, document.documentElement]) {
      if (!root) continue;
      let found = null;
      const walk = node => {
        if (found) return;
        if (node.nodeType === 1 && node.getAttribute('id') === id) { found = node; return; }
        for (const c of node.childNodes) walk(c);
      };
      walk(root);
      if (found) return found;
    }
    return null;
  },
  querySelector(selector) {
    const all = document.querySelectorAll(selector);
    return all.length ? all[0] : null;
  },
  querySelectorAll(selector) {
    const result = [];
    for (const root of [document.body, document.documentElement]) {
      if (!root) continue;
      const parts = selector.split(',').map(s => s.trim()).filter(Boolean);
      const walk = node => {
        if (node.nodeType === 1) {
          for (const p of parts) if (node._matches(p)) { result.push(node); break; }
        }
        for (const c of node.childNodes) walk(c);
      };
      walk(root);
    }
    return result;
  },
  _body: null,
  _docEl: null,
  createNodeIterator(root, _whatToShow, _filter) {
    const stack = [...root.childNodes].reverse();
    return { nextNode() { while (stack.length) { const n = stack.pop(); if (n) { for (let i = n.childNodes.length - 1; i >= 0; i--) stack.push(n.childNodes[i]); return n; } } return null; } };
  },
  getElementsByTagName(tag) {
    const out = [];
    const walk = node => {
      if (node.nodeType === 1) {
        if (tag === '*' || node.localName === String(tag).toLowerCase()) out.push(node);
        for (const c of node.childNodes) walk(c);
      }
    };
    // may be called via .call(otherDoc, ...) – honour the receiver when it is
    // a document, fall back to the live one otherwise
    const roots = (this && this.nodeType === 9) ? [this.documentElement, this.body]
                                               : [document.body, document.documentElement];
    for (const r of roots)
      if (r && !r.contains(roots[0])) walk(r);
    return out;
  }
};

// ---- :nth-child( <an+b> ) : returns true if the 1-based index matches ----
function _nthChildMatch(arg, index) {
  arg = arg.trim().toLowerCase();
  if (arg === 'n') return true;
  const m = arg.match(/^(?:(-?\d*)n\s*([+-]\s*\d+)?)?$/);
  if (m) {
    let a = m[1];
    a = a === '' || a === '+' ? 1 : a === '-' ? -1 : parseInt(a, 10);
    const b = m[2] ? parseInt(m[2].replace(/\s+/g, ''), 10) : 0;
    if (a === 0) return index === b;
    const diff = index - b;
    return diff % a === 0 && diff / a >= 0;
  }
  if (/^-?\d+$/.test(arg)) return index === parseInt(arg, 10);
  return false;
}

// Evaluate a single pseudo-class against `this`. Returns false for any
// pseudo-class we do not implement (so it never accidentally matches).
DomNode.prototype._matchesPseudo = function (pseudo) {
  const p = pseudo.trim();
  const par = this.parentNode;
  if (p === 'first-child')
    return !par || par.childNodes.length === 0 || par.childNodes[0] === this;
  if (p === 'last-child')
    return !par || par.childNodes.length === 0 || par.childNodes[par.childNodes.length - 1] === this;
  if (p === 'only-child')
    return !par || par.childNodes.length === 1;
  if (p === 'empty')
    return this.childNodes.length === 0;
  if (p.startsWith('nth-child('))
    return _nthChildMatch(p.slice(10, -1),
      par ? par.childNodes.indexOf(this) + 1 : 1);
  if (p.startsWith('not(')) {
    const inner = p.slice(4, -1);
    return !inner.split(',').some(s => this._matches(s.trim()));
  }
  return false;
};

// Split a simple selector into its non-pseudo base and a list of
// ":name" / ":name(args)" pseudo-classes (args may contain commas/parens).
DomNode.prototype._splitPseudos = function (sel) {
  const pseudos = [];
  let base = '';
  let i = 0;
  while (i < sel.length) {
    if (sel[i] === ':') {
      let j = i + 1;
      let name = '';
      while (j < sel.length && sel[j] !== '(' && sel[j] !== ':' && sel[j] !== ' ')
        { name += sel[j]; j++; }
      let arg = null;
      if (j < sel.length && sel[j] === '(') {
        let depth = 1, k = j + 1, argStr = '';
        while (k < sel.length && depth > 0) {
          if (sel[k] === '(') depth++;
          else if (sel[k] === ')') { depth--; if (depth === 0) break; }
          argStr += sel[k]; k++;
        }
        arg = argStr; j = k + 1;
      }
      pseudos.push(name + (arg != null ? '(' + arg + ')' : ''));
      i = j;
    } else { base += sel[i]; i++; }
  }
  return { base, pseudos };
};

// simple selector matching: #id, tag, .class, [attr], [attr="v"], tag[attr="v"], .a.b,
// pseudo-classes (:first-child, :last-child, :only-child, :empty, :nth-child(), :not()),
// plus descendant combinators ("a b c")
DomNode.prototype._matchesSimple = function (sel) {
  sel = sel.trim();
  if (!sel) return false;
  const { base, pseudos } = this._splitPseudos(sel);
  if (base && !this._matchesBase(base)) return false;
  for (const p of pseudos) if (!this._matchesPseudo(p)) return false;
  return true;
};

DomNode.prototype._matchesBase = function (sel) {
  if (sel.startsWith('#')) return this.getAttribute('id') === sel.slice(1);
  let rest = sel;
  const m = rest.match(/^([a-zA-Z][\w-]*)/);
  if (m) { if (this.localName !== m[1]) return false; rest = rest.slice(m[1].length); }
  while (rest.length) {
    const cls = rest.match(/^\.([\w-]+)/);
    if (cls) { if (!this.classList.contains(cls[1])) return false; rest = rest.slice(cls[0].length); continue; }
    const attr = rest.match(/^\[([\w-]+)(?:([\^\*]?=)(["']?)([^"'\]]*)\3)?\]/);
    if (attr) {
      const v = this.getAttribute(attr[1]);
      if (v == null) return false;
      if (attr[2]) {
        const op = attr[2].replace('=', '');
        const want = attr[4] || '';
        if (op === '*' ? !v.includes(want) : op === '^' ? !v.startsWith(want) : v !== want)
          return false;
      }
      rest = rest.slice(attr[0].length);
      continue;
    }
    break;
  }
  return rest.length === 0;
};

DomNode.prototype._matches = function (sel) {
  const parts = sel.trim().split(/\s+/);
  if (parts.length === 1) return this._matchesSimple(parts[0]);
  // descendant combinator: the last part must match this node and each
  // earlier part must match some ancestor in order
  if (!this._matchesSimple(parts[parts.length - 1])) return false;
  let i = parts.length - 2;
  let anc = this.parentNode;
  while (i >= 0 && anc) {
    if (anc.nodeType === 1 && anc._matchesSimple(parts[i])) --i;
    anc = anc.parentNode;
  }
  return i < 0;
};

// body / documentElement are created lazily below.

// ---- window ----
const window = {
  getComputedStyle(el) {
    const style = el && el.style ? el.style : new StyleDeclaration(null);
    return {
      getPropertyValue(name) { return style.getPropertyValue(name); },
      get cssText() { return style.cssText; }
    };
  },
  requestAnimationFrame(fn) { try { fn(performance.now()); } catch (e) {} return 1; },
  cancelAnimationFrame() {},
  // QuickJS has no event loop: timers are backed by microtasks so they run
  // while the C++ host pumps the job queue.
  setTimeout(fn, _ms, ...args) { const id = ++__timerId; Promise.resolve().then(() => fn(...args)).catch(() => {}); return id; },
  clearTimeout() {},
  setInterval(fn, _ms, ...args) { return window.setTimeout(fn, 0, ...args); },
  clearInterval() {},
  addEventListener() {},
  removeEventListener() {},
  devicePixelRatio: 1,
  location: { href: 'about:blank', protocol: 'http:', host: 'localhost', search: '', hash: '' },
  history: { pushState() {}, replaceState() {}, length: 1 },
  matchMedia() { return { matches: false, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }; },
  atob(s) { return atobImpl(s); },
  btoa(s) { return btoaImpl(s); },
  getSelection() { return { getRangeAt() { return null; }, rangeCount: 0 }; },
  CustomEvent: DomEvent,
  Event: DomEvent,
  MutationObserver: class { observe() {} disconnect() {} takeRecords() { return []; } },
  ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
  IntersectionObserver: class { observe() {} unobserve() {} disconnect() {} },
  scrollTo() {},
  scroll() {},
  print() {},
  open() { return null; },
  focus() {},
  innerWidth: 1024,
  innerHeight: 768,
  getBoundingClientRect: () => ({ x: 0, y: 0, width: 1024, height: 768 })
};

let __timerId = 0;

class TextEncoder {
  encode(input) {
    const s = String(input);
    const out = new Uint8Array(s.length * 3);
    let o = 0;
    for (let i = 0; i < s.length; i++) {
      let c = s.codePointAt(i);
      if (c > 0xffff) i++;
      if (c < 0x80) out[o++] = c;
      else if (c < 0x800) { out[o++] = 0xc0 | (c >> 6); out[o++] = 0x80 | (c & 0x3f); }
      else if (c < 0x10000) { out[o++] = 0xe0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 0x3f); out[o++] = 0x80 | (c & 0x3f); }
      else { out[o++] = 0xf0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 0x3f); out[o++] = 0x80 | ((c >> 6) & 0x3f); out[o++] = 0x80 | (c & 0x3f); }
    }
    return out.subarray(0, o);
  }
}

class TextDecoder {
  decode(input) {
    if (input == null) return '';
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    let out = '';
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      if (b < 0x80) out += String.fromCharCode(b);
      else if (b < 0xe0) out += String.fromCharCode(((b & 0x1f) << 6) | (bytes[++i] & 0x3f));
      else if (b < 0xf0) out += String.fromCharCode(((b & 0x0f) << 12) | ((bytes[++i] & 0x3f) << 6) | (bytes[++i] & 0x3f));
      else {
        const c = ((b & 0x07) << 18) | ((bytes[++i] & 0x3f) << 12) | ((bytes[++i] & 0x3f) << 6) | (bytes[++i] & 0x3f);
        out += String.fromCodePoint(c);
      }
    }
    return out;
  }
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function atobImpl(s) {
  let out = '';
  s = String(s).replace(/\s/g, '');
  let buf = 0, bits = 0;
  for (const ch of s) {
    if (ch === '=') break;
    const v = B64.indexOf(ch);
    if (v < 0) continue;
    buf = (buf << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 0xff); }
  }
  return out;
}
function btoaImpl(s) {
  let out = '';
  let buf = 0, bits = 0;
  for (let i = 0; i < s.length; i++) {
    buf = (buf << 8) | s.charCodeAt(i); bits += 8;
    while (bits >= 6) { bits -= 6; out += B64[(buf >> bits) & 0x3f]; }
  }
  if (bits > 0) out += B64[(buf << (6 - bits)) & 0x3f];
  while (out.length % 4) out += '=';
  return out;
}
window.atob = atobImpl;
window.btoa = btoaImpl;

class DOMParser {
  parseFromString(html, _type) {
    const docEl = new DomNode(1, 'html');
    const head = new DomNode(1, 'head');
    const body = new DomNode(1, 'body');
    docEl.appendChild(head);
    docEl.appendChild(body);
    const nodes = parseHtmlFragment(String(html), globalThis.document);
    for (const n of nodes) body.appendChild(n);
    return {
      nodeType: 9,
      nodeName: '#document',
      documentElement: docEl,
      body,
      head,
      createElement: t => globalThis.document.createElement(t),
      createElementNS: (ns, t) => globalThis.document.createElementNS(ns, t),
      importNode: (n, deep) => n.cloneNode(!!deep),
      createTextNode: t => globalThis.document.createTextNode(t),
      querySelector: s => document.querySelector(s),
      querySelectorAll: s => document.querySelectorAll(s),
      getElementsByTagName: t => {
        const out = [];
        const walk = node => {
          if (node.nodeType === 1) {
            if (t === '*' || node.localName === String(t).toLowerCase()) out.push(node);
            for (const c of node.childNodes) walk(c);
          }
        };
        walk(docEl);
        return out;
      }
    };
  }
}

const navigator = {
  userAgent: 'Mozilla/5.0 (QuickJS MermaidShim)',
  platform: 'MacIntel',
  language: 'en-US',
  languages: ['en-US'],
  onLine: true,
  hardwareConcurrency: 4
};

const performance = {
  now() { return Date.now(); },
  timing: { navigationStart: Date.now() }
};

function installGlobals() {
  const g = globalThis;
  const body = new DomNode(1, 'body');
  const html = new DomNode(1, 'html');
  const head = new DomNode(1, 'head');
  html.appendChild(head);
  html.appendChild(body);
  document.body = body;
  document.documentElement = html;
  // body needs a sane width for mermaid's container measurement
  Object.defineProperty(body, 'getBoundingClientRect', {
    value() { return { x: 0, y: 0, width: 800, height: 600, top: 0, left: 0, right: 800, bottom: 600 }; }
  });
  g.window = window;
  g.self = window;
  g.document = document;
  window.document = document;
  window.self = window;
  // minimal document extras
  document.implementation = { createHTMLDocument() { return document; }, hasFeature() { return false; } };
  document.importNode = (node, deep) => node.cloneNode(!!deep);
  try {
    g.navigator = navigator;
  } catch (e) { // e.g. Node.js exposes a getter-only global navigator
    try { Object.defineProperty(g, 'navigator', { value: navigator, configurable: true }); } catch (e2) {}
  }
  if (!g.performance) g.performance = performance;
  g.CSSStyleSheet = CSSStyleSheet;
  g.HTMLElement = DomNode;
  g.SVGSVGElement = DomNode;
  g.SVGElement = DomNode;
  g.Element = DomNode;
  g.Node = DomNode;
  g.Text = DomNode;
  g.CustomEvent = DomEvent;
  g.Event = DomEvent;
  g.DOMParser = DOMParser;
  g.NodeFilter = {
    SHOW_ALL: 0xffffffff, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4,
    SHOW_CDATA_SECTION: 8, SHOW_ENTITY_REFERENCE: 16, SHOW_PROCESSING_INSTRUCTION: 32,
    SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024,
    SHOW_NOTATION: 2048, SHOW_NOTATION_NODE: 2048, SHOW_ENTITY: 64, SHOW_DECLARATION: 32768,
    FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3
  };
  g.MutationObserver = window.MutationObserver;
  g.ResizeObserver = window.ResizeObserver;
  g.IntersectionObserver = window.IntersectionObserver;
  g.console = {
    log: (...a) => __qtLog('[log]', ...a),
    warn: (...a) => __qtLog('[warn]', ...a),
    error: (...a) => __qtLog('[error]', ...a),
    info: (...a) => __qtLog('[info]', ...a),
    debug: (...a) => __qtLog('[debug]', ...a),
    trace: (...a) => __qtLog('[trace]', ...a),
    assert: (cond, ...a) => { if (!cond) __qtLog('[assert]', ...a); }
  };
  g.requestAnimationFrame = window.requestAnimationFrame;
  g.cancelAnimationFrame = window.cancelAnimationFrame;
  g.setTimeout = window.setTimeout;
  g.clearTimeout = window.clearTimeout;
  g.setInterval = window.setInterval;
  g.clearInterval = window.clearInterval;
  g.getComputedStyle = el => window.getComputedStyle(el);
  g.atob = atobImpl;
  g.btoa = btoaImpl;
  g.TextEncoder = TextEncoder;
  g.TextDecoder = TextDecoder;
  if (!g.structuredClone)
    g.structuredClone = value => JSON.parse(JSON.stringify(value));
  if (!g.crypto) {
    g.crypto = {
      getRandomValues(arr) { for (let i = 0; i < arr.length; i++) arr[i] = Math.floor(Math.random() * 256); return arr; },
      randomUUID() {
        return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
          const r = Math.random() * 16 | 0;
          return (c === 'x' ? r : (r & 0x3 | 0x8)).toString(16);
        });
      }
    };
  }
  for (const name of ['CSSStyleSheet', 'HTMLElement', 'SVGSVGElement', 'SVGElement', 'Element', 'Node', 'Text', 'CustomEvent', 'Event', 'MutationObserver', 'ResizeObserver', 'IntersectionObserver', 'DOMParser', 'NodeFilter', 'Error', 'TypeError', 'RangeError', 'SyntaxError', 'Promise', 'Map', 'Set', 'JSON', 'Math', 'Date', 'RegExp', 'Object', 'Array'])
    if (g[name] !== undefined)
      window[name] = g[name];
  return g;
}

installGlobals();
